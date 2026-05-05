/*
 * RDMAExecutorBackend — JVM-side RDMA client for the remote executor.
 *
 * Uses ibverbs for:
 *   Control: RDMA SEND/RECV (reliable connected QP) — same protocol as TCP
 *   Data: RDMA WRITE (eviction), RDMA READ (fetch) — one-sided, bypasses remote CPU
 *
 * Bootstrap via TCP (exchange QP metadata), then all communication is RDMA.
 */

#include "precompiled.hpp"

#ifdef REMOTE_EXECUTOR_USE_RDMA

#include "gc/g1/g1RemoteBackendRdma.hpp"
#include "gc/g1/g1CollectedHeap.hpp"
#include "gc/shared/gc_globals.hpp"
#include "runtime/globals.hpp"
#include "logging/log.hpp"
#include "runtime/os.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <infiniband/verbs.h>

// Protocol constants (match remote_protocol.h)
static const uint32_t RE_CMD_HELLO              = 0x01;
static const uint32_t RE_CMD_EVICT_OBJECT       = 0x03;
static const uint32_t RE_CMD_REPORT_ROOTS       = 0x04;
static const uint32_t RE_CMD_REQUEST_COLLECTION = 0x05;
static const uint32_t RE_CMD_FETCH_OBJECT       = 0x06;
static const uint32_t RE_CMD_DISCARD_SLOT       = 0x07;
static const uint32_t RE_CMD_SHUTDOWN           = 0xFF;
static const uint32_t RE_RESP_OK                = 0x81;
static const uint32_t RE_RESP_OBJECT_DATA       = 0x83;
static const uint32_t RE_RESP_COLLECTION_RESULT = 0x84;
static const uint32_t RE_CMD_DIRECTORY_UPSERT       = 0x10;
static const uint32_t RE_CMD_LOCALIZE_BATCH         = 0x11;
static const uint32_t RE_CMD_EVICT_WITH_EDGES       = 0x12;
static const uint32_t RE_CMD_REPORT_REMOTE_ROOTS_V2 = 0x13;
static const uint32_t RE_CMD_TRACE_AND_REPORT       = 0x17;
static const uint32_t RE_CMD_FETCH_AROUND           = 0x18;
static const uint32_t RE_CMD_BATCH_EVICT_HOMOG_STAGED_WITH_EDGES = 0x1A;
static const uint32_t RE_RESP_TRACE_RESULT           = 0x86;
static const uint32_t RE_RESP_BATCH_OBJECT_DATA      = 0x87;

// RDMA parameters are set via JVM flags (g1_globals.hpp):
//   -XX:RDMAMsgBufSize=65536    (SEND/RECV buffer, default 64K)
//   -XX:RDMADataBufSize=4194304 (WRITE/READ staging, default 4M)
//   -XX:RDMACQDepth=256
//   -XX:RDMASQDepth=128
//   -XX:RDMARQDepth=128
// Increase RDMADataBufSize for large objects; ensure ulimit -l covers it.

// QP metadata for bootstrap (must match executor's rdma_qp_info_t)
struct RdmaQPInfo {
    uint32_t qpn;
    uint32_t psn;
    uint16_t lid;
    uint8_t  gid[16];
    uint32_t rkey;
    uint64_t base_addr;
    uint64_t arena_size;
    uint8_t  active_mtu;
} __attribute__((packed));

static enum ibv_mtu choose_path_mtu(uint8_t local_mtu, uint8_t remote_mtu) {
  uint8_t mtu = local_mtu < remote_mtu ? local_mtu : remote_mtu;
  if (mtu < IBV_MTU_256 || mtu > IBV_MTU_4096) {
    return IBV_MTU_1024;
  }
  return (enum ibv_mtu)mtu;
}

// ================================================================
// TCP helpers for bootstrap
// ================================================================

static bool tcp_send_exact(int fd, const void* buf, size_t len) {
  const uint8_t* p = (const uint8_t*)buf;
  while (len > 0) {
    ssize_t n = ::send(fd, p, len, 0);
    if (n <= 0) { if (n < 0 && errno == EINTR) continue; return false; }
    p += n; len -= n;
  }
  return true;
}

static bool tcp_recv_exact(int fd, void* buf, size_t len) {
  uint8_t* p = (uint8_t*)buf;
  while (len > 0) {
    ssize_t n = ::recv(fd, p, len, 0);
    if (n <= 0) { if (n < 0 && errno == EINTR) continue; return false; }
    p += n; len -= n;
  }
  return true;
}

// ================================================================
// CQ completion wait — uses LIBAPTH's apth_rdma_wait for cooperative yield
// ================================================================
// Instead of busy-spinning (burning 100% CPU), yield to LIBAPTH scheduler.
// Fast path: single poll (~10ns). If not ready: register with RDMA poller,
// yield (~20ns context switch), another thread runs. Poller detects completion
// (~50ns) and wakes us. Total: ~90ns vs ~5μs kernel context switch.

#ifdef USE_LIBAPTH
#include "apth.h"
// RDMA API from LIBAPTH — declared here because apth.h guards them behind
// APTH_USE_RDMA which isn't set in the JDK build (and precompiled headers
// would ignore a source-level #define anyway).
extern "C" {
  int apth_rdma_register_cq(struct ibv_cq *cq);
  int apth_rdma_wait(struct ibv_cq *cq, uint64_t wr_id, struct ibv_wc *wc);
}
#endif

static bool poll_cq_wait(struct ibv_cq* cq, uint64_t wr_id, struct ibv_wc* wc) {
#ifdef USE_LIBAPTH
  // LIBAPTH cooperative wait: fast poll → yield → wake on completion
  int ret = apth_rdma_wait(cq, wr_id, wc);
  if (ret != 0) {
    log_warning(gc)("RDMA: apth_rdma_wait failed: errno=%d", errno);
    return false;
  }
  if (wc->status != IBV_WC_SUCCESS) {
    log_warning(gc)("RDMA: completion error: status=%d", wc->status);
    return false;
  }
  return true;
#else
  // Fallback: busy-poll (no LIBAPTH)
  int ne;
  do { ne = ibv_poll_cq(cq, 1, wc); } while (ne == 0);
  if (ne < 0 || wc->status != IBV_WC_SUCCESS) {
    log_warning(gc)("RDMA: poll_cq failed: ne=%d status=%d", ne, wc->status);
    return false;
  }
  return true;
#endif
}

// ================================================================
// Constructor / Destructor
// ================================================================

RDMAExecutorBackend::RDMAExecutorBackend()
  : _ctx(nullptr), _pd(nullptr), _send_cq(nullptr), _recv_cq(nullptr),
    _qp(nullptr), _local_mr(nullptr),
    _remote_base_addr(0), _remote_rkey(0), _remote_arena_size(0),
    _tcp_fd(-1), _connected(false), _seq_id(0),
    _next_slot(0), _total_evicted(0), _total_fetched(0),
    _pending_localize_ids(nullptr), _pending_localize_count(0),
    _pending_localize_capacity(0), _io_lock(0) {
}

RDMAExecutorBackend::~RDMAExecutorBackend() {
  shutdown();
}

// ================================================================
// RDMA resource setup
// ================================================================

bool RDMAExecutorBackend::setup_rdma_resources() {
  // Open first IB device
  struct ibv_device** dev_list = ibv_get_device_list(NULL);
  if (!dev_list || !dev_list[0]) {
    log_warning(gc)("RDMA: no IB devices found");
    return false;
  }
  _ctx = ibv_open_device(dev_list[0]);
  ibv_free_device_list(dev_list);
  if (!_ctx) { log_warning(gc)("RDMA: ibv_open_device failed"); return false; }

  _pd = ibv_alloc_pd(_ctx);
  if (!_pd) { log_warning(gc)("RDMA: ibv_alloc_pd failed"); return false; }

  _send_cq = ibv_create_cq(_ctx, RDMACQDepth, NULL, NULL, 0);
  _recv_cq = ibv_create_cq(_ctx, RDMACQDepth, NULL, NULL, 0);
  if (!_send_cq || !_recv_cq) { log_warning(gc)("RDMA: ibv_create_cq failed"); return false; }

  // NOTE: CQ registration with LIBAPTH poller is deferred to after successful
  // TCP bootstrap (in initialize()). Registering here would start the poller
  // thread, and if init fails the poller crashes on destroyed CQs.

  struct ibv_qp_init_attr qp_init;
  memset(&qp_init, 0, sizeof(qp_init));
  qp_init.send_cq = _send_cq;
  qp_init.recv_cq = _recv_cq;
  qp_init.cap.max_send_wr = RDMASQDepth;
  qp_init.cap.max_recv_wr = RDMARQDepth;
  qp_init.cap.max_send_sge = 1;
  qp_init.cap.max_recv_sge = 1;
  qp_init.cap.max_inline_data = 64;
  qp_init.qp_type = IBV_QPT_RC;

  _qp = ibv_create_qp(_pd, &qp_init);
  if (!_qp) { log_warning(gc)("RDMA: ibv_create_qp failed"); return false; }

  // Allocate and register buffers: send, recv, data staging
  // We use a single large MR for all three, with offsets:
  //   [0, RDMAMsgBufSize)            = send buffer
  //   [RDMAMsgBufSize, 2*MAX)        = recv buffer
  //   [2*MAX, 2*MAX + RDMADataBufSize) = data staging (for RDMA WRITE/READ)
  size_t total_size = 2 * RDMAMsgBufSize + RDMADataBufSize;
  void* buf = os::malloc(total_size, mtGC);
  if (!buf) { log_warning(gc)("RDMA: buffer malloc failed"); return false; }
  memset(buf, 0, total_size);

  // Defense against fastswap (the custom kernel module that swaps pages over
  // RDMA): mlock the buffer so the kernel cannot swap it out. Without this,
  // fetch_remote_object → libc memcpy can SIGSEGV at SEGV_ACCERR when the
  // recv buffer's page is reclaimed under cgroup memory pressure, despite
  // ibv_reg_mr's pinning. Observed crash signature: libc.so.6+0x18b963 with
  // si_addr in the recv buffer range.
  if (mlock(buf, total_size) != 0) {
    log_warning(gc)("RDMA: mlock failed for " SIZE_FORMAT " bytes (errno=%d: %s). "
                    "Page faults during fetch may SIGSEGV under memory pressure. "
                    "Fix: 'ulimit -l unlimited'.",
                    total_size, errno, os::strerror(errno));
    // Continue without mlock — registration may still work, and the SIGSEGV
    // is intermittent. Better to attempt running than to refuse to start.
  }

  _local_mr = ibv_reg_mr(_pd, buf, total_size,
                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                          IBV_ACCESS_REMOTE_READ);
  if (!_local_mr) {
    log_warning(gc)("RDMA: ibv_reg_mr failed for " SIZE_FORMAT " bytes (errno=%d: %s). "
                    "Check 'ulimit -l' (locked memory limit). RDMA pins pages. "
                    "Fix: 'ulimit -l unlimited' or /etc/security/limits.conf. "
                    "Or reduce: -XX:RDMADataBufSize=<smaller> -XX:RDMAMsgBufSize=<smaller>",
                    total_size, errno, os::strerror(errno));
    munlock(buf, total_size);
    os::free(buf);
    return false;
  }

  // Transition QP to INIT
  struct ibv_qp_attr attr;
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_INIT;
  attr.pkey_index = 0;
  attr.port_num = 1;
  attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_LOCAL_WRITE;
  if (ibv_modify_qp(_qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
    log_warning(gc)("RDMA: QP to INIT failed"); return false;
  }

  log_info(gc)("RDMA: resources created (QP=%u)", _qp->qp_num);
  return true;
}

bool RDMAExecutorBackend::exchange_qp_info() {
  // Fill local info
  struct ibv_port_attr port_attr;
  ibv_query_port(_ctx, 1, &port_attr);

  union ibv_gid gid;
  ibv_query_gid(_ctx, 1, 0, &gid);

  uint32_t local_psn = lrand48() & 0xFFFFFF;

  RdmaQPInfo local_info;
  memset(&local_info, 0, sizeof(local_info));
  local_info.qpn = _qp->qp_num;
  local_info.psn = local_psn;
  local_info.lid = port_attr.lid;
  local_info.active_mtu = (uint8_t)port_attr.active_mtu;
  memcpy(local_info.gid, &gid, 16);
  local_info.rkey = _local_mr->rkey;
  local_info.base_addr = (uint64_t)_local_mr->addr;
  local_info.arena_size = RDMADataBufSize;

  // Exchange via TCP
  RdmaQPInfo remote_info;
  if (!tcp_send_exact(_tcp_fd, &local_info, sizeof(local_info))) return false;
  if (!tcp_recv_exact(_tcp_fd, &remote_info, sizeof(remote_info))) return false;

  _remote_rkey = remote_info.rkey;
  _remote_base_addr = remote_info.base_addr;
  _remote_arena_size = remote_info.arena_size;

  log_info(gc)("RDMA: local QP=%u PSN=%u LID=%u, remote QP=%u PSN=%u LID=%u rkey=0x%x",
               local_info.qpn, local_info.psn, local_info.lid,
               remote_info.qpn, remote_info.psn, remote_info.lid, remote_info.rkey);
  enum ibv_mtu path_mtu = choose_path_mtu(local_info.active_mtu, remote_info.active_mtu);
  log_info(gc)("RDMA: path MTU enum=%d (local=%u remote=%u)",
               path_mtu, local_info.active_mtu, remote_info.active_mtu);

  // Transition QP: INIT → RTR
  struct ibv_qp_attr rtr_attr;
  memset(&rtr_attr, 0, sizeof(rtr_attr));
  rtr_attr.qp_state = IBV_QPS_RTR;
  rtr_attr.path_mtu = path_mtu;
  rtr_attr.dest_qp_num = remote_info.qpn;
  rtr_attr.rq_psn = remote_info.psn;
  rtr_attr.max_dest_rd_atomic = 1;
  rtr_attr.min_rnr_timer = 12;
  rtr_attr.ah_attr.dlid = remote_info.lid;
  rtr_attr.ah_attr.sl = 0;
  rtr_attr.ah_attr.src_path_bits = 0;
  rtr_attr.ah_attr.port_num = 1;
  rtr_attr.ah_attr.is_global = 0;

  // Check if GID is non-zero (RoCE)
  bool has_gid = false;
  for (int i = 0; i < 16; i++) { if (remote_info.gid[i]) { has_gid = true; break; } }
  if (has_gid) {
    rtr_attr.ah_attr.is_global = 1;
    memcpy(&rtr_attr.ah_attr.grh.dgid, remote_info.gid, 16);
    rtr_attr.ah_attr.grh.hop_limit = 255;
    rtr_attr.ah_attr.grh.sgid_index = 0;
  }

  if (ibv_modify_qp(_qp, &rtr_attr,
      IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
      IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
    log_warning(gc)("RDMA: QP to RTR failed"); return false;
  }

  // Transition QP: RTR → RTS
  struct ibv_qp_attr rts_attr;
  memset(&rts_attr, 0, sizeof(rts_attr));
  rts_attr.qp_state = IBV_QPS_RTS;
  rts_attr.timeout = 14;
  rts_attr.retry_cnt = 7;
  rts_attr.rnr_retry = 7;
  rts_attr.sq_psn = local_psn;
  rts_attr.max_rd_atomic = 1;

  if (ibv_modify_qp(_qp, &rts_attr,
      IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
      IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC)) {
    log_warning(gc)("RDMA: QP to RTS failed"); return false;
  }

  log_info(gc)("RDMA: QP ready (RTS)");
  return true;
}

// ================================================================
// RDMA SEND/RECV for control messages
// ================================================================

bool RDMAExecutorBackend::rdma_send_msg(const void* data, size_t len) {
  if (len > RDMAMsgBufSize) return false;
  void* send_buf = _local_mr->addr;  // offset 0 = send buffer
  memcpy(send_buf, data, len);

  struct ibv_sge sge;
  sge.addr = (uintptr_t)send_buf;
  sge.length = (uint32_t)len;
  sge.lkey = _local_mr->lkey;

  struct ibv_send_wr wr;
  memset(&wr, 0, sizeof(wr));
  wr.wr_id = 1;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_SEND;
  wr.send_flags = IBV_SEND_SIGNALED;

  struct ibv_send_wr* bad_wr = NULL;
  if (ibv_post_send(_qp, &wr, &bad_wr)) return false;

  struct ibv_wc wc;
  return poll_cq_wait(_send_cq, wr.wr_id, &wc);
}

// Post a recv buffer WITHOUT waiting. Caller must follow with rdma_wait_recv.
// Splitting post/wait lets callers post recv BEFORE send, avoiding the
// race where peer's response arrives at our QP before our recv is posted
// (results in RNR retries, possible silent loss, and eventual hang).
bool RDMAExecutorBackend::rdma_post_recv() {
  void* recv_buf = (char*)_local_mr->addr + RDMAMsgBufSize;
  struct ibv_sge sge;
  sge.addr = (uintptr_t)recv_buf;
  sge.length = RDMAMsgBufSize;
  sge.lkey = _local_mr->lkey;

  struct ibv_recv_wr wr;
  memset(&wr, 0, sizeof(wr));
  wr.sg_list = &sge;
  wr.num_sge = 1;
  /* wr.wr_id = 0 from memset — under io_lock there's at most one in-flight
     recv at a time so wr_id collisions are impossible. */

  struct ibv_recv_wr* bad_wr = NULL;
  return ibv_post_recv(_qp, &wr, &bad_wr) == 0;
}

bool RDMAExecutorBackend::rdma_wait_recv(void* buf, size_t max_len, size_t* actual_len) {
  void* recv_buf = (char*)_local_mr->addr + RDMAMsgBufSize;
  struct ibv_wc wc;
  if (!poll_cq_wait(_recv_cq, /*wr_id*/0, &wc)) return false;

  size_t len = wc.byte_len;
  if (len > max_len) len = max_len;
  memcpy(buf, recv_buf, len);
  if (actual_len) *actual_len = len;
  return true;
}

// Legacy combined post+wait. Keeps the old race for callers that haven't
// migrated; new code should call rdma_post_recv() before the send and
// rdma_wait_recv() after.
bool RDMAExecutorBackend::rdma_recv_msg(void* buf, size_t max_len, size_t* actual_len) {
  if (!rdma_post_recv()) return false;
  return rdma_wait_recv(buf, max_len, actual_len);
}

static size_t rdma_localize_batch_capacity() {
  size_t max_per_msg = (RDMAMsgBufSize > 20) ? ((RDMAMsgBufSize - 20) / 8) : 1;
  return MAX2((size_t)1, MIN2(max_per_msg, (size_t)4096));
}

bool RDMAExecutorBackend::ensure_localize_buffer_locked() {
  if (_pending_localize_ids != nullptr) return true;

  _pending_localize_capacity = rdma_localize_batch_capacity();
  _pending_localize_ids =
      (uintptr_t*)os::malloc(_pending_localize_capacity * sizeof(uintptr_t), mtGC);
  if (_pending_localize_ids == nullptr) {
    _pending_localize_capacity = 0;
    return false;
  }
  return true;
}

bool RDMAExecutorBackend::send_localize_batch_locked(const uintptr_t* handle_ids, size_t count) {
  if (!_connected || count == 0) return true;

  size_t max_per_msg = rdma_localize_batch_capacity();
  size_t offset = 0;
  while (offset < count) {
    size_t chunk = MIN2(count - offset, max_per_msg);
    size_t msg_size = 20 + chunk * 8;
    uint8_t* msg = (uint8_t*)os::malloc(msg_size, mtGC);
    if (msg == nullptr) return false;

    *(uint32_t*)(msg + 0)  = RE_CMD_LOCALIZE_BATCH;
    *(uint32_t*)(msg + 4)  = (uint32_t)msg_size;
    *(uint64_t*)(msg + 8)  = _seq_id++;
    *(uint32_t*)(msg + 16) = (uint32_t)chunk;
    memcpy(msg + 20, handle_ids + offset, chunk * sizeof(uintptr_t));

    if (!rdma_post_recv()) { os::free(msg); return false; }
    bool ok = rdma_send_msg(msg, msg_size);
    os::free(msg);
    if (!ok) return false;

    uint8_t resp[64];
    size_t resp_len = 0;
    if (!rdma_wait_recv(resp, sizeof(resp), &resp_len)) return false;
    if (resp_len < 4 || *(uint32_t*)resp != RE_RESP_OK) return false;

    offset += chunk;
  }
  return true;
}

bool RDMAExecutorBackend::flush_localize_batch_locked() {
  if (_pending_localize_count == 0) return true;
  bool ok = send_localize_batch_locked(_pending_localize_ids, _pending_localize_count);
  if (ok) {
    _pending_localize_count = 0;
  }
  return ok;
}

// ================================================================
// RDMA one-sided data operations
// ================================================================

bool RDMAExecutorBackend::rdma_write(uint64_t remote_offset, const void* local_buf, size_t len) {
  void* data_buf = (char*)_local_mr->addr + 2 * RDMAMsgBufSize;
  memcpy(data_buf, local_buf, len);

  struct ibv_sge sge;
  sge.addr = (uintptr_t)data_buf;
  sge.length = (uint32_t)len;
  sge.lkey = _local_mr->lkey;

  struct ibv_send_wr wr;
  memset(&wr, 0, sizeof(wr));
  wr.wr_id = 2;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_RDMA_WRITE;
  wr.send_flags = IBV_SEND_SIGNALED;
  wr.wr.rdma.remote_addr = _remote_base_addr + remote_offset;
  wr.wr.rdma.rkey = _remote_rkey;

  struct ibv_send_wr* bad_wr = NULL;
  if (ibv_post_send(_qp, &wr, &bad_wr)) return false;

  struct ibv_wc wc;
  return poll_cq_wait(_send_cq, wr.wr_id, &wc);
}

bool RDMAExecutorBackend::rdma_read(uint64_t remote_offset, void* local_buf, size_t len) {
  void* data_buf = (char*)_local_mr->addr + 2 * RDMAMsgBufSize;

  struct ibv_sge sge;
  sge.addr = (uintptr_t)data_buf;
  sge.length = (uint32_t)len;
  sge.lkey = _local_mr->lkey;

  struct ibv_send_wr wr;
  memset(&wr, 0, sizeof(wr));
  wr.wr_id = 3;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_RDMA_READ;
  wr.send_flags = IBV_SEND_SIGNALED;
  wr.wr.rdma.remote_addr = _remote_base_addr + remote_offset;
  wr.wr.rdma.rkey = _remote_rkey;

  struct ibv_send_wr* bad_wr = NULL;
  if (ibv_post_send(_qp, &wr, &bad_wr)) return false;

  struct ibv_wc wc;
  if (!poll_cq_wait(_send_cq, wr.wr_id, &wc)) return false;

  memcpy(local_buf, data_buf, len);
  return true;
}

// ================================================================
// Lifecycle
// ================================================================

bool RDMAExecutorBackend::initialize() {
  const char* host = RemoteExecutorHost;
  int port = (int)RemoteExecutorPort;

  if (!setup_rdma_resources()) return false;

  // TCP connect for bootstrap
  log_info(gc)("RDMA: trying to open TCP socket");
  _tcp_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (_tcp_fd < 0) { log_warning(gc)("RDMA: socket failed"); return false; }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
    log_warning(gc)("RDMA: invalid host %s", host);
    return false;
  }

  log_info(gc)("RDMA: trying to connect to TCP socket");
  if (::connect(_tcp_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    log_warning(gc)("RDMA: TCP connect to %s:%d failed: %s", host, port, os::strerror(errno));
    return false;
  }

  log_info(gc)("RDMA: trying to exchange QP info");
  if (!exchange_qp_info()) return false;

#ifdef USE_LIBAPTH
  // Register CQs with LIBAPTH's RDMA poller AFTER QP is RTS but BEFORE any
  // RDMA operations. The poller must monitor CQs for apth_rdma_wait to work.
  // Safe to start poller here: CQs are valid and QP is connected.
  if (apth_rdma_register_cq(_send_cq) != 0) {
    log_warning(gc)("RDMA: failed to register send_cq with LIBAPTH poller");
  }
  if (apth_rdma_register_cq(_recv_cq) != 0) {
    log_warning(gc)("RDMA: failed to register recv_cq with LIBAPTH poller");
  }
  log_info(gc)("RDMA: CQs registered with LIBAPTH poller for cooperative wait");
#endif

  // Pre-post recv buffers
  for (int i = 0; i < 4; i++) {
    void* recv_buf = (char*)_local_mr->addr + RDMAMsgBufSize;
    struct ibv_sge sge;
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)recv_buf;
    sge.length = (uint32_t)RDMAMsgBufSize;
    sge.lkey = _local_mr->lkey;
    struct ibv_recv_wr wr;
    memset(&wr, 0, sizeof(wr));
    wr.sg_list = &sge; wr.num_sge = 1;
    struct ibv_recv_wr* bad_wr = NULL;
    ibv_post_recv(_qp, &wr, &bad_wr);
  }

  // Send hello via RDMA SEND — remote_cmd_hello_t (40 bytes)
  uint8_t hello[40];
  memset(hello, 0, sizeof(hello));
  *(uint32_t*)(hello + 0)  = RE_CMD_HELLO;           // hdr.type
  *(uint32_t*)(hello + 4)  = 40;                      // hdr.length
  *(uint64_t*)(hello + 8)  = _seq_id++;               // hdr.seq_id
  *(uint32_t*)(hello + 16) = 1;                       // protocol_version
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  *(uint64_t*)(hello + 20) = (uint64_t)g1h->reserved().start();  // heap_base
  *(uint64_t*)(hello + 28) = (uint64_t)g1h->max_capacity();      // heap_size
  *(uint32_t*)(hello + 36) = (uint32_t)MinObjAlignmentInBytes;   // min_obj_alignment

  log_info(gc)("RDMA: sending hello");
  // Pre-post recv to avoid RNR race with peer's response.
  if (!rdma_post_recv()) { log_warning(gc)("RDMA: hello post_recv failed"); return false; }
  if (!rdma_send_msg(hello, 40)) { log_warning(gc)("RDMA: hello send failed"); return false; }

  uint8_t resp[64];
  size_t resp_len = 0;
  if (!rdma_wait_recv(resp, sizeof(resp), &resp_len)) { log_warning(gc)("RDMA: hello recv failed"); return false; }
  if (*(uint32_t*)resp != RE_RESP_OK) { log_warning(gc)("RDMA: hello rejected"); return false; }

  _connected = true;
  log_info(gc)("Remote backend: rdma-executor connected to %s:%d via RDMA", host, port);
  return true;
}

void RDMAExecutorBackend::shutdown() {
  if (_connected) {
    io_lock();
    flush_localize_batch_locked();
    uint8_t msg[16];
    memset(msg, 0, sizeof(msg));
    *(uint32_t*)msg = RE_CMD_SHUTDOWN;
    *(uint32_t*)(msg + 4) = 16;
    rdma_send_msg(msg, 16);
    io_unlock();
    _connected = false;
  }
  if (_pending_localize_ids != nullptr) {
    os::free(_pending_localize_ids);
    _pending_localize_ids = nullptr;
    _pending_localize_count = 0;
    _pending_localize_capacity = 0;
  }
  if (_qp) { ibv_destroy_qp(_qp); _qp = nullptr; }
  if (_send_cq) { ibv_destroy_cq(_send_cq); _send_cq = nullptr; }
  if (_recv_cq) { ibv_destroy_cq(_recv_cq); _recv_cq = nullptr; }
  if (_local_mr) {
    void* buf = _local_mr->addr;
    size_t total_size = 2 * RDMAMsgBufSize + RDMADataBufSize;
    ibv_dereg_mr(_local_mr);
    munlock(buf, total_size);
    os::free(buf);
    _local_mr = nullptr;
  }
  if (_pd) { ibv_dealloc_pd(_pd); _pd = nullptr; }
  if (_ctx) { ibv_close_device(_ctx); _ctx = nullptr; }
  if (_tcp_fd >= 0) { ::close(_tcp_fd); _tcp_fd = -1; }
}

// ================================================================
// Backend operations — same protocol as TCPExecutorBackend
// but using RDMA SEND/RECV instead of TCP
// ================================================================

size_t RDMAExecutorBackend::evict(const void* obj_bytes, size_t word_size,
                                  Klass* klass, size_t hint_slot_id) {
  if (!_connected) return (size_t)-1;

  io_lock();
  if (!flush_localize_batch_locked()) { io_unlock(); return (size_t)-1; }

  // Pre-post recv to win the race vs. peer's response.
  if (!rdma_post_recv()) { io_unlock(); return (size_t)-1; }

  size_t slot_id = (hint_slot_id != (size_t)-1) ? hint_slot_id : _next_slot++;
  size_t byte_size = word_size * HeapWordSize;

  size_t msg_size = 36 + byte_size;
  uint8_t* msg = (uint8_t*)os::malloc(msg_size, mtGC);
  *(uint32_t*)(msg + 0)  = RE_CMD_EVICT_OBJECT;
  *(uint32_t*)(msg + 4)  = (uint32_t)msg_size;
  *(uint64_t*)(msg + 8)  = _seq_id++;
  *(uint64_t*)(msg + 16) = slot_id;
  *(uint64_t*)(msg + 24) = (uint64_t)klass;
  *(uint32_t*)(msg + 32) = (uint32_t)word_size;
  memcpy(msg + 36, obj_bytes, byte_size);

  bool ok = rdma_send_msg(msg, msg_size);
  os::free(msg);
  if (!ok) { io_unlock(); return (size_t)-1; }

  uint8_t resp[64];
  size_t resp_len = 0;
  if (!rdma_wait_recv(resp, sizeof(resp), &resp_len)) { io_unlock(); return (size_t)-1; }

  io_unlock();

  _total_evicted++;
  return slot_id;
}

Klass* RDMAExecutorBackend::fetch(size_t slot_id, void* dest, size_t* out_word_size) {
  if (!_connected) return nullptr;

  io_lock();

  // Post recv BEFORE send to eliminate the RNR race: if the executor
  // responds before our recv is posted, the response triggers an RNR
  // condition (or worse, silently drops if peer doesn't retry), causing
  // our subsequent rdma_recv_msg to hang waiting for a completion that
  // never arrives. Pre-posting guarantees the buffer is ready.
  if (!rdma_post_recv()) { io_unlock(); return nullptr; }

  uint8_t msg[24];
  *(uint32_t*)(msg + 0) = RE_CMD_FETCH_OBJECT;
  *(uint32_t*)(msg + 4) = 24;
  *(uint64_t*)(msg + 8) = _seq_id++;
  *(uint64_t*)(msg + 16) = slot_id;
  if (!rdma_send_msg(msg, 24)) { io_unlock(); return nullptr; }

  void* recv_buf = (char*)_local_mr->addr + RDMAMsgBufSize;
  struct ibv_wc wc;
  if (!poll_cq_wait(_recv_cq, /*wr_id*/0, &wc)) { io_unlock(); return nullptr; }

  uint8_t* resp = (uint8_t*)recv_buf;
  size_t resp_len = wc.byte_len;
  if (resp_len < 36) {
    log_warning(gc)("RDMAExecutor: fetch response too short: " SIZE_FORMAT " bytes", resp_len);
    io_unlock();
    return nullptr;
  }

  if (*(uint32_t*)resp != RE_RESP_OBJECT_DATA) {
    io_unlock();
    return nullptr;
  }

  uint32_t resp_msg_len = *(uint32_t*)(resp + 4);
  uint64_t resp_klass = *(uint64_t*)(resp + 24);
  uint32_t resp_ws = *(uint32_t*)(resp + 32);
  size_t byte_size = (size_t)resp_ws * HeapWordSize;

  if (36 + byte_size > resp_len || 36 + byte_size > resp_msg_len) {
    log_warning(gc)("RDMAExecutor: fetch resp_ws=%u claims " SIZE_FORMAT
                    "B but response only " SIZE_FORMAT " bytes (header length %u)",
                    resp_ws, byte_size, resp_len, resp_msg_len);
    io_unlock();
    return nullptr;
  }

  memcpy(dest, resp + 36, byte_size);
  if (out_word_size) *out_word_size = resp_ws;

  io_unlock();
  _total_fetched++;
  return (Klass*)resp_klass;
}

bool RDMAExecutorBackend::supports_batch_fetch() const {
  return true;
}

size_t RDMAExecutorBackend::fetch_batch_around(uintptr_t handle_id, size_t slot_id,
                                               uint max_objects, uint slot_window,
                                               size_t max_response_bytes,
                                               FetchBatchClosure* cl) {
  if (!_connected || cl == nullptr || max_objects == 0) return 0;

  if (max_response_bytes == 0 || max_response_bytes > RDMAMsgBufSize) {
    max_response_bytes = RDMAMsgBufSize;
  }
  if (max_response_bytes < 24 + 32) {
    return 0;
  }
  size_t max_by_header = (max_response_bytes - 24) / 32;
  if (max_objects > max_by_header) {
    max_objects = (uint)max_by_header;
  }
  if (max_objects == 0) {
    return 0;
  }

  io_lock();
  if (!flush_localize_batch_locked()) { io_unlock(); return 0; }

  if (!rdma_post_recv()) { io_unlock(); return 0; }

  uint8_t msg[48];
  *(uint32_t*)(msg + 0) = RE_CMD_FETCH_AROUND;
  *(uint32_t*)(msg + 4) = sizeof(msg);
  *(uint64_t*)(msg + 8) = _seq_id++;
  *(uint64_t*)(msg + 16) = (uint64_t)handle_id;
  *(uint64_t*)(msg + 24) = (uint64_t)slot_id;
  *(uint32_t*)(msg + 32) = max_objects;
  *(uint32_t*)(msg + 36) = slot_window;
  *(uint64_t*)(msg + 40) = (uint64_t)max_response_bytes;
  if (!rdma_send_msg(msg, sizeof(msg))) { io_unlock(); return 0; }

  void* recv_buf = (char*)_local_mr->addr + RDMAMsgBufSize;
  struct ibv_wc wc;
  if (!poll_cq_wait(_recv_cq, /*wr_id*/0, &wc)) { io_unlock(); return 0; }

  uint8_t* resp = (uint8_t*)recv_buf;
  size_t resp_len = wc.byte_len;
  if (resp_len < 24) {
    log_warning(gc)("RDMAExecutor: batch fetch response too short: " SIZE_FORMAT " bytes", resp_len);
    io_unlock();
    return 0;
  }

  if (*(uint32_t*)resp != RE_RESP_BATCH_OBJECT_DATA) {
    io_unlock();
    return 0;
  }

  uint32_t resp_msg_len = *(uint32_t*)(resp + 4);
  if (resp_msg_len > resp_len || resp_msg_len > max_response_bytes) {
    log_warning(gc)("RDMAExecutor: malformed batch fetch length: hdr=%u actual="
                    SIZE_FORMAT " max=" SIZE_FORMAT,
                    resp_msg_len, resp_len, max_response_bytes);
    io_unlock();
    return 0;
  }

  uint8_t* stable = (uint8_t*)os::malloc(resp_msg_len, mtGC);
  if (stable == nullptr) {
    io_unlock();
    return 0;
  }
  memcpy(stable, resp, resp_msg_len);
  io_unlock();

  uint32_t num_objects = *(uint32_t*)(stable + 16);
  uint8_t* cursor = stable + 24;
  uint8_t* end = stable + resp_msg_len;
  size_t fetched = 0;

  for (uint32_t i = 0; i < num_objects; i++) {
    if (cursor + 32 > end) {
      log_warning(gc)("RDMAExecutor: truncated batch fetch entry header at %u/%u",
                      i, num_objects);
      break;
    }
    uintptr_t entry_handle = (uintptr_t)*(uint64_t*)(cursor + 0);
    size_t entry_slot = (size_t)*(uint64_t*)(cursor + 8);
    Klass* entry_klass = (Klass*)(uintptr_t)*(uint64_t*)(cursor + 16);
    uint32_t word_size = *(uint32_t*)(cursor + 24);
    uint32_t byte_size = *(uint32_t*)(cursor + 28);
    cursor += 32;

    if ((size_t)word_size * HeapWordSize != byte_size || cursor + byte_size > end) {
      log_warning(gc)("RDMAExecutor: malformed batch fetch entry %u/%u "
                      "(ws=%u bytes=%u remaining=" SIZE_FORMAT ")",
                      i, num_objects, word_size, byte_size, (size_t)(end - cursor));
      break;
    }

    cl->do_object(entry_handle, entry_slot, entry_klass, word_size, cursor);
    cursor += byte_size;
    fetched++;
  }

  os::free(stable);
  _total_fetched += fetched;
  return fetched;
}

void RDMAExecutorBackend::report_roots(const size_t* root_slot_ids, size_t num_roots) {
  if (!_connected) return;

  io_lock();
  if (!flush_localize_batch_locked()) { io_unlock(); return; }

  size_t msg_size = 20 + num_roots * 8;
  uint8_t* msg = (uint8_t*)os::malloc(msg_size, mtGC);
  *(uint32_t*)(msg + 0) = RE_CMD_REPORT_ROOTS;
  *(uint32_t*)(msg + 4) = (uint32_t)msg_size;
  *(uint64_t*)(msg + 8) = _seq_id++;
  *(uint32_t*)(msg + 16) = (uint32_t)num_roots;
  uint64_t* ids = (uint64_t*)(msg + 20);
  for (size_t i = 0; i < num_roots; i++) ids[i] = (uint64_t)root_slot_ids[i];

  // Pre-post recv to avoid RNR race.
  if (!rdma_post_recv()) { os::free(msg); io_unlock(); return; }
  rdma_send_msg(msg, msg_size);
  os::free(msg);

  uint8_t resp[64];
  size_t resp_len = 0;
  rdma_wait_recv(resp, sizeof(resp), &resp_len);

  io_unlock();
}

void RDMAExecutorBackend::collect_dead(size_t** out_dead_ids, size_t* out_num_dead,
                                       size_t* out_bytes_freed) {
  if (!_connected) {
    *out_dead_ids = nullptr; *out_num_dead = 0; *out_bytes_freed = 0;
    return;
  }

  io_lock();
  if (!flush_localize_batch_locked()) {
    io_unlock();
    *out_dead_ids = nullptr; *out_num_dead = 0; *out_bytes_freed = 0;
    return;
  }

  if (!rdma_post_recv()) { io_unlock(); *out_dead_ids = nullptr; *out_num_dead = 0; *out_bytes_freed = 0; return; }

  uint8_t msg[16];
  *(uint32_t*)(msg + 0) = RE_CMD_REQUEST_COLLECTION;
  *(uint32_t*)(msg + 4) = 16;
  *(uint64_t*)(msg + 8) = _seq_id++;
  rdma_send_msg(msg, 16);

  uint8_t* resp = (uint8_t*)os::malloc(1024 * 1024, mtGC);
  size_t resp_len = 0;
  if (!rdma_wait_recv(resp, 1024 * 1024, &resp_len)) {
    os::free(resp);
    io_unlock();
    *out_dead_ids = nullptr; *out_num_dead = 0; *out_bytes_freed = 0;
    return;
  }

  io_unlock();

  if (*(uint32_t*)resp != RE_RESP_COLLECTION_RESULT) {
    os::free(resp);
    *out_dead_ids = nullptr; *out_num_dead = 0; *out_bytes_freed = 0;
    return;
  }

  uint32_t num_dead = *(uint32_t*)(resp + 16);
  uint64_t bytes_freed = *(uint64_t*)(resp + 24);
  uint64_t* dead_raw = (uint64_t*)(resp + 32);

  size_t* dead = (size_t*)os::malloc(num_dead * sizeof(size_t), mtGC);
  for (uint32_t i = 0; i < num_dead; i++) dead[i] = (size_t)dead_raw[i];

  *out_dead_ids = dead;
  *out_num_dead = num_dead;
  *out_bytes_freed = bytes_freed;
  os::free(resp);
}

void RDMAExecutorBackend::trace_and_report(uintptr_t** out_dead_ids, size_t* out_num_dead,
                                           size_t* out_bytes_freed,
                                           uintptr_t** out_cross_src, uintptr_t** out_cross_tgt,
                                           size_t* out_num_cross) {
  *out_dead_ids = nullptr; *out_num_dead = 0; *out_bytes_freed = 0;
  *out_cross_src = nullptr; *out_cross_tgt = nullptr; *out_num_cross = 0;
  if (!_connected) return;

  io_lock();
  if (!flush_localize_batch_locked()) { io_unlock(); return; }

  if (!rdma_post_recv()) { io_unlock(); return; }

  uint8_t msg[16];
  *(uint32_t*)(msg + 0) = RE_CMD_TRACE_AND_REPORT;
  *(uint32_t*)(msg + 4) = 16;
  *(uint64_t*)(msg + 8) = _seq_id++;
  if (!rdma_send_msg(msg, 16)) { io_unlock(); return; }

  uint8_t* resp = (uint8_t*)os::malloc(4 * 1024 * 1024, mtGC);
  size_t resp_len = 0;
  if (!rdma_wait_recv(resp, 4 * 1024 * 1024, &resp_len)) {
    os::free(resp);
    io_unlock();
    return;
  }

  io_unlock();

  if (resp_len < 4 || *(uint32_t*)resp != RE_RESP_TRACE_RESULT) {
    os::free(resp);
    return;
  }

  uint32_t num_dead    = *(uint32_t*)(resp + 16);
  uint32_t num_live    = *(uint32_t*)(resp + 20);
  uint64_t bytes_freed = *(uint64_t*)(resp + 24);
  uint32_t num_cross   = *(uint32_t*)(resp + 32);

  *out_num_dead = num_dead;
  *out_bytes_freed = bytes_freed;
  *out_num_cross = num_cross;

  uint64_t* dead_raw = (uint64_t*)(resp + 40);
  if (num_dead > 0) {
    uintptr_t* dead = (uintptr_t*)os::malloc(num_dead * sizeof(uintptr_t), mtGC);
    for (uint32_t i = 0; i < num_dead; i++) dead[i] = (uintptr_t)dead_raw[i];
    *out_dead_ids = dead;
  }

  if (num_cross > 0) {
    uint8_t* cross_raw = resp + 40 + num_dead * 8;
    uintptr_t* cross_src = (uintptr_t*)os::malloc(num_cross * sizeof(uintptr_t), mtGC);
    uintptr_t* cross_tgt = (uintptr_t*)os::malloc(num_cross * sizeof(uintptr_t), mtGC);
    for (uint32_t i = 0; i < num_cross; i++) {
      cross_src[i] = (uintptr_t)*(uint64_t*)(cross_raw + i * 16);
      cross_tgt[i] = (uintptr_t)*(uint64_t*)(cross_raw + i * 16 + 8);
    }
    *out_cross_src = cross_src;
    *out_cross_tgt = cross_tgt;
  }

  log_info(gc)("trace_and_report: %u dead, %u live, " UINT64_FORMAT " bytes freed, %u cross-edges",
               num_dead, num_live, bytes_freed, num_cross);
  os::free(resp);
}

void RDMAExecutorBackend::discard_slot(size_t slot_id) {
  if (!_connected) return;

  io_lock();
  if (!flush_localize_batch_locked()) { io_unlock(); return; }

  if (!rdma_post_recv()) { io_unlock(); return; }

  uint8_t msg[24];
  *(uint32_t*)(msg + 0) = RE_CMD_DISCARD_SLOT;
  *(uint32_t*)(msg + 4) = 24;
  *(uint64_t*)(msg + 8) = _seq_id++;
  *(uint64_t*)(msg + 16) = slot_id;
  rdma_send_msg(msg, 24);
  uint8_t resp[64];
  size_t resp_len = 0;
  rdma_wait_recv(resp, sizeof(resp), &resp_len);

  io_unlock();
}

int RDMAExecutorBackend::batch_evict(const void* msg_buf, size_t msg_len) {
  if (!_connected) return -1;

  io_lock();
  if (!flush_localize_batch_locked()) { io_unlock(); return -1; }
  if (!rdma_post_recv()) { io_unlock(); return -1; }
  bool ok = rdma_send_msg(msg_buf, msg_len);
  if (!ok) { io_unlock(); return -1; }

  uint8_t resp[64];
  size_t resp_len = 0;
  if (!rdma_wait_recv(resp, sizeof(resp), &resp_len)) { io_unlock(); return -1; }

  io_unlock();
  return (*(uint32_t*)resp == RE_RESP_OK) ? 0 : -1;
}

bool RDMAExecutorBackend::supports_batch_evict() const {
  return true;
}

size_t RDMAExecutorBackend::max_batch_evict_message_size() const {
  return RDMAMsgBufSize;
}

bool RDMAExecutorBackend::supports_staged_homogeneous_batch_evict() const {
  return true;
}

size_t RDMAExecutorBackend::max_staged_batch_data_size() const {
  return MIN2((size_t)RDMADataBufSize, _remote_arena_size);
}

int RDMAExecutorBackend::batch_evict_staged_homogeneous(const void* msg_buf, size_t msg_len,
                                                        const void* data_buf, size_t data_len,
                                                        uint64_t remote_data_offset) {
  if (!_connected) return -1;
  if (msg_len > RDMAMsgBufSize) return -1;
  if (data_len > max_staged_batch_data_size()) return -1;
  if (remote_data_offset > _remote_arena_size ||
      data_len > _remote_arena_size - remote_data_offset) {
    return -1;
  }

  io_lock();
  if (!flush_localize_batch_locked()) { io_unlock(); return -1; }
  if (data_len > 0 && !rdma_write(remote_data_offset, data_buf, data_len)) {
    io_unlock();
    return -1;
  }
  if (!rdma_post_recv()) { io_unlock(); return -1; }
  bool ok = rdma_send_msg(msg_buf, msg_len);
  if (!ok) { io_unlock(); return -1; }

  uint8_t resp[64];
  size_t resp_len = 0;
  if (!rdma_wait_recv(resp, sizeof(resp), &resp_len)) { io_unlock(); return -1; }

  io_unlock();
  return (resp_len >= 4 && *(uint32_t*)resp == RE_RESP_OK) ? 0 : -1;
}

size_t RDMAExecutorBackend::slot_word_size(size_t /*slot_id*/) const {
  return 0;  // Remote metadata — fetch response includes size
}

// ============================================================
// V2 Protocol Implementations
// ============================================================

size_t RDMAExecutorBackend::evict_with_edges(const void* obj_bytes, size_t word_size,
                                             Klass* klass, uintptr_t handle_id,
                                             const EdgeInfo* edges, uint32_t num_edges,
                                             size_t hint_slot_id) {
  if (!_connected) return (size_t)-1;

  io_lock();
  if (!flush_localize_batch_locked()) { io_unlock(); return (size_t)-1; }

  size_t slot_id = (hint_slot_id == (size_t)-1) ? _next_slot++ : hint_slot_id;
  size_t byte_size = word_size * HeapWordSize;
  size_t edge_bytes = num_edges * 12;  // field_offset(4) + target_handle_id(8)
  size_t msg_size = 48 + byte_size + edge_bytes;
  uint8_t* msg = (uint8_t*)os::malloc(msg_size, mtGC);

  *(uint32_t*)(msg + 0)  = RE_CMD_EVICT_WITH_EDGES;
  *(uint32_t*)(msg + 4)  = (uint32_t)msg_size;
  *(uint64_t*)(msg + 8)  = _seq_id++;
  *(uint64_t*)(msg + 16) = slot_id;
  *(uint64_t*)(msg + 24) = handle_id;
  *(uint64_t*)(msg + 32) = (uint64_t)(uintptr_t)klass;
  *(uint32_t*)(msg + 40) = (uint32_t)word_size;
  *(uint32_t*)(msg + 44) = num_edges;
  memcpy(msg + 48, obj_bytes, byte_size);

  uint8_t* edge_ptr = msg + 48 + byte_size;
  for (uint32_t i = 0; i < num_edges; i++) {
    *(uint32_t*)(edge_ptr)     = edges[i].field_offset;
    *(uint64_t*)(edge_ptr + 4) = edges[i].target_handle_id;
    edge_ptr += 12;
  }

  if (!rdma_post_recv()) { os::free(msg); io_unlock(); return (size_t)-1; }
  bool ok = rdma_send_msg(msg, msg_size);
  os::free(msg);
  if (!ok) { io_unlock(); return (size_t)-1; }

  uint8_t resp[64];
  size_t resp_len = 0;
  if (!rdma_wait_recv(resp, sizeof(resp), &resp_len)) { io_unlock(); return (size_t)-1; }

  io_unlock();

  if (resp_len >= 4 && *(uint32_t*)resp == RE_RESP_OK) {
    _total_evicted++;
    return slot_id;
  }
  return (size_t)-1;
}

void RDMAExecutorBackend::localize_batch(const uintptr_t* handle_ids, size_t count) {
  if (!_connected || count == 0) return;

  io_lock();

  if (!ensure_localize_buffer_locked()) {
    send_localize_batch_locked(handle_ids, count);
    io_unlock();
    return;
  }

  size_t offset = 0;
  while (offset < count) {
    if (_pending_localize_count == _pending_localize_capacity &&
        !flush_localize_batch_locked()) {
      break;
    }
    size_t space = _pending_localize_capacity - _pending_localize_count;
    size_t chunk = MIN2(count - offset, space);
    memcpy(_pending_localize_ids + _pending_localize_count,
           handle_ids + offset,
           chunk * sizeof(uintptr_t));
    _pending_localize_count += chunk;
    offset += chunk;

    if (_pending_localize_count == _pending_localize_capacity &&
        !flush_localize_batch_locked()) {
      break;
    }
  }

  io_unlock();
}

void RDMAExecutorBackend::report_remote_roots_v2(const uintptr_t* handle_ids, size_t count) {
  if (!_connected) return;

  io_lock();
  if (!flush_localize_batch_locked()) { io_unlock(); return; }

  // Max roots per message: (RDMAMsgBufSize - 20 header) / 8 bytes per root
  size_t max_per_msg = (RDMAMsgBufSize - 20) / 8;
  size_t remaining = count;
  size_t offset = 0;

  // Always clear first so executor doesn't accumulate across GC cycles
  {
    uint8_t clear_msg[20];
    *(uint32_t*)(clear_msg + 0)  = RE_CMD_REPORT_REMOTE_ROOTS_V2;
    *(uint32_t*)(clear_msg + 4)  = 20;
    *(uint64_t*)(clear_msg + 8)  = _seq_id++;
    *(uint32_t*)(clear_msg + 16) = 0;  // clear
    if (!rdma_post_recv()) { io_unlock(); return; }
    rdma_send_msg(clear_msg, 20);
    uint8_t resp[64]; size_t resp_len = 0;
    rdma_wait_recv(resp, sizeof(resp), &resp_len);
  }

  while (remaining > 0) {
    size_t chunk = MIN2(remaining, max_per_msg);
    size_t msg_size = 20 + chunk * 8;
    uint8_t* msg = (uint8_t*)os::malloc(msg_size, mtGC);
    *(uint32_t*)(msg + 0)  = RE_CMD_REPORT_REMOTE_ROOTS_V2;
    *(uint32_t*)(msg + 4)  = (uint32_t)msg_size;
    *(uint64_t*)(msg + 8)  = _seq_id++;
    *(uint32_t*)(msg + 16) = (uint32_t)chunk;
    memcpy(msg + 20, handle_ids + offset, chunk * 8);

    if (!rdma_post_recv()) { os::free(msg); io_unlock(); return; }
    rdma_send_msg(msg, msg_size);
    os::free(msg);

    uint8_t resp[64]; size_t resp_len = 0;
    rdma_wait_recv(resp, sizeof(resp), &resp_len);

    offset += chunk;
    remaining -= chunk;
  }

  io_unlock();
}

void RDMAExecutorBackend::directory_upsert(const uintptr_t* handle_ids,
                                           const uint32_t* states,
                                           const size_t* slot_ids,
                                           size_t count) {
  if (!_connected || count == 0) return;

  io_lock();
  if (!flush_localize_batch_locked()) { io_unlock(); return; }

  size_t msg_size = 20 + count * 20;
  uint8_t* msg = (uint8_t*)os::malloc(msg_size, mtGC);
  *(uint32_t*)(msg + 0)  = RE_CMD_DIRECTORY_UPSERT;
  *(uint32_t*)(msg + 4)  = (uint32_t)msg_size;
  *(uint64_t*)(msg + 8)  = _seq_id++;
  *(uint32_t*)(msg + 16) = (uint32_t)count;

  uint8_t* ptr = msg + 20;
  for (size_t i = 0; i < count; i++) {
    *(uint64_t*)(ptr)      = handle_ids[i];
    *(uint32_t*)(ptr + 8)  = states[i];
    *(uint64_t*)(ptr + 12) = slot_ids[i];
    ptr += 20;
  }

  if (!rdma_post_recv()) { os::free(msg); io_unlock(); return; }
  rdma_send_msg(msg, msg_size);
  os::free(msg);

  uint8_t resp[64];
  size_t resp_len = 0;
  rdma_wait_recv(resp, sizeof(resp), &resp_len);

  io_unlock();
}

#endif // REMOTE_EXECUTOR_USE_RDMA
