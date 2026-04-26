/*
 * TCPExecutorBackend — connects to remote_executor process via TCP.
 * Protocol matches remote_executor/remote_protocol.h.
 */

#include "precompiled.hpp"
#include "gc/g1/g1RemoteBackendTcp.hpp"
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

// Protocol constants (must match remote_protocol.h)
static const uint32_t RE_CMD_HELLO              = 0x01;
static const uint32_t RE_CMD_EVICT_OBJECT       = 0x03;
static const uint32_t RE_CMD_REPORT_ROOTS       = 0x04;
static const uint32_t RE_CMD_REQUEST_COLLECTION = 0x05;
static const uint32_t RE_CMD_FETCH_OBJECT       = 0x06;
static const uint32_t RE_CMD_DISCARD_SLOT       = 0x07;
static const uint32_t RE_CMD_SHUTDOWN           = 0xFF;
static const uint32_t RE_RESP_OK                = 0x81;
static const uint32_t RE_RESP_OBJECT_DATA       = 0x83;

// V2 protocol constants
static const uint32_t RE_CMD_DIRECTORY_UPSERT       = 0x10;
static const uint32_t RE_CMD_LOCALIZE_BATCH         = 0x11;
static const uint32_t RE_CMD_EVICT_WITH_EDGES       = 0x12;
static const uint32_t RE_CMD_REPORT_REMOTE_ROOTS_V2 = 0x13;
static const uint32_t RE_RESP_COLLECTION_RESULT = 0x84;
static const uint32_t RE_CMD_TRACE_AND_REPORT      = 0x17;
static const uint32_t RE_RESP_TRACE_RESULT          = 0x86;

TCPExecutorBackend::TCPExecutorBackend()
  : _fd(-1), _connected(false), _seq_id(0),
    _next_slot(0), _total_evicted(0), _total_fetched(0), _io_lock(0) {}

TCPExecutorBackend::~TCPExecutorBackend() {
  shutdown();
}

// ================================================================
// Low-level TCP helpers
// ================================================================

bool TCPExecutorBackend::send_all(const void* data, size_t len) {
  const uint8_t* p = (const uint8_t*)data;
  size_t remaining = len;
  while (remaining > 0) {
    ssize_t n = ::send(_fd, p, remaining, MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      log_warning(gc)("TCPExecutor: send failed: %s", os::strerror(errno));
      return false;
    }
    p += n;
    remaining -= n;
  }
  return true;
}

bool TCPExecutorBackend::recv_all(void* buf, size_t len) {
  uint8_t* p = (uint8_t*)buf;
  size_t remaining = len;
  while (remaining > 0) {
    ssize_t n = ::recv(_fd, p, remaining, 0);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      log_warning(gc)("TCPExecutor: recv failed: %s", os::strerror(errno));
      return false;
    }
    p += n;
    remaining -= n;
  }
  return true;
}

bool TCPExecutorBackend::send_msg(const void* data, size_t len) {
  uint32_t net_len = (uint32_t)len;
  if (!send_all(&net_len, 4)) return false;
  return send_all(data, len);
}

bool TCPExecutorBackend::recv_msg(void* buf, size_t max_len, size_t* actual_len) {
  uint32_t net_len = 0;
  if (!recv_all(&net_len, 4)) return false;
  if (net_len > max_len) {
    log_warning(gc)("TCPExecutor: message too large: %u > %zu", net_len, max_len);
    return false;
  }
  if (!recv_all(buf, net_len)) return false;
  if (actual_len) *actual_len = net_len;
  return true;
}

// ================================================================
// Lifecycle
// ================================================================

bool TCPExecutorBackend::initialize() {
  const char* host = RemoteExecutorHost;
  int port = (int)RemoteExecutorPort;

  _fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (_fd < 0) {
    log_warning(gc)("TCPExecutor: socket() failed: %s", os::strerror(errno));
    return false;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
    log_warning(gc)("TCPExecutor: invalid host %s", host);
    ::close(_fd); _fd = -1;
    return false;
  }

  if (::connect(_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    log_warning(gc)("TCPExecutor: connect to %s:%d failed: %s", host, port, os::strerror(errno));
    ::close(_fd); _fd = -1;
    return false;
  }

  int nodelay = 1;
  setsockopt(_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  _connected = true;

  log_info(gc)("Remote backend: tcp-executor connected to %s:%d", host, port);
  return send_hello();
}

bool TCPExecutorBackend::send_hello() {
  // remote_cmd_hello_t: hdr(16) + version(4) + heap_base(8) + heap_size(8) + align(4) = 40
  uint8_t msg[40];
  memset(msg, 0, sizeof(msg));
  *(uint32_t*)(msg + 0)  = RE_CMD_HELLO;           // hdr.type
  *(uint32_t*)(msg + 4)  = 40;                      // hdr.length
  *(uint64_t*)(msg + 8)  = _seq_id++;               // hdr.seq_id
  *(uint32_t*)(msg + 16) = 1;                       // protocol_version
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  *(uint64_t*)(msg + 20) = (uint64_t)g1h->reserved().start();  // heap_base
  *(uint64_t*)(msg + 28) = (uint64_t)g1h->max_capacity();      // heap_size
  *(uint32_t*)(msg + 36) = (uint32_t)MinObjAlignmentInBytes;   // min_obj_alignment

  if (!send_msg(msg, 40)) return false;

  uint8_t resp[64];
  size_t resp_len = 0;
  if (!recv_msg(resp, sizeof(resp), &resp_len)) return false;

  if (*(uint32_t*)resp != RE_RESP_OK) {
    log_warning(gc)("TCPExecutor: hello failed");
    return false;
  }
  log_info(gc)("TCPExecutor: hello OK");
  return true;
}

void TCPExecutorBackend::shutdown() {
  if (_connected && _fd >= 0) {
    uint8_t msg[12];
    *(uint32_t*)(msg + 0) = RE_CMD_SHUTDOWN;
    *(uint32_t*)(msg + 4) = 12;
    *(uint32_t*)(msg + 8) = 0;
    send_msg(msg, 12);  // best-effort
    ::close(_fd);
    _fd = -1;
    _connected = false;
  }
}

// ================================================================
// Operations
// ================================================================

size_t TCPExecutorBackend::evict(const void* obj_bytes, size_t word_size,
                                 Klass* klass, size_t hint_slot_id) {
  if (!_connected) return (size_t)-1;

  io_lock();

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

  bool ok = send_msg(msg, msg_size);
  os::free(msg);
  if (!ok) { io_unlock(); return (size_t)-1; }

  uint8_t resp[64];
  size_t resp_len = 0;
  if (!recv_msg(resp, sizeof(resp), &resp_len)) { io_unlock(); return (size_t)-1; }

  io_unlock();

  _total_evicted++;
  return slot_id;
}

Klass* TCPExecutorBackend::fetch(size_t slot_id, void* dest, size_t* out_word_size) {
  if (!_connected) return nullptr;

  io_lock();

  uint8_t fetch_msg[24];  // header(16) + slot_id(8)
  *(uint32_t*)(fetch_msg + 0) = RE_CMD_FETCH_OBJECT;
  *(uint32_t*)(fetch_msg + 4) = 24;
  *(uint64_t*)(fetch_msg + 8) = _seq_id++;
  *(uint64_t*)(fetch_msg + 16) = slot_id;

  if (!send_msg(fetch_msg, 24)) { io_unlock(); return nullptr; }

  uint8_t* resp = (uint8_t*)os::malloc(128 * 1024, mtGC);
  size_t resp_len = 0;
  if (!recv_msg(resp, 128 * 1024, &resp_len)) {
    os::free(resp);
    io_unlock();
    return nullptr;
  }

  io_unlock();

  if (*(uint32_t*)resp != RE_RESP_OBJECT_DATA) {
    os::free(resp);
    return nullptr;
  }

  uint64_t resp_klass_val = *(uint64_t*)(resp + 24);
  uint32_t resp_ws = *(uint32_t*)(resp + 32);
  size_t byte_size = resp_ws * HeapWordSize;
  memcpy(dest, resp + 36, byte_size);

  if (out_word_size) *out_word_size = resp_ws;
  os::free(resp);
  _total_fetched++;
  return (Klass*)resp_klass_val;
}

void TCPExecutorBackend::report_roots(const size_t* root_slot_ids, size_t num_roots) {
  if (!_connected) return;

  io_lock();

  size_t msg_size = 20 + num_roots * 8;
  uint8_t* msg = (uint8_t*)os::malloc(msg_size, mtGC);
  *(uint32_t*)(msg + 0) = RE_CMD_REPORT_ROOTS;
  *(uint32_t*)(msg + 4) = (uint32_t)msg_size;
  *(uint64_t*)(msg + 8) = _seq_id++;
  *(uint32_t*)(msg + 16) = (uint32_t)num_roots;
  uint64_t* dst_ids = (uint64_t*)(msg + 20);
  for (size_t i = 0; i < num_roots; i++) {
    dst_ids[i] = (uint64_t)root_slot_ids[i];
  }

  send_msg(msg, msg_size);
  os::free(msg);

  uint8_t resp[64];
  size_t resp_len = 0;
  recv_msg(resp, sizeof(resp), &resp_len);

  io_unlock();
}

void TCPExecutorBackend::collect_dead(size_t** out_dead_ids, size_t* out_num_dead,
                                      size_t* out_bytes_freed) {
  if (!_connected) {
    *out_dead_ids = nullptr; *out_num_dead = 0; *out_bytes_freed = 0;
    return;
  }

  io_lock();

  uint8_t msg[16];
  *(uint32_t*)(msg + 0) = RE_CMD_REQUEST_COLLECTION;
  *(uint32_t*)(msg + 4) = 16;
  *(uint64_t*)(msg + 8) = _seq_id++;
  send_msg(msg, 16);

  uint8_t* resp = (uint8_t*)os::malloc(1024 * 1024, mtGC);
  size_t resp_len = 0;
  if (!recv_msg(resp, 1024 * 1024, &resp_len)) {
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
  uint64_t* dead_ids_raw = (uint64_t*)(resp + 32);

  size_t* dead_ids = (size_t*)os::malloc(num_dead * sizeof(size_t), mtGC);
  for (uint32_t i = 0; i < num_dead; i++) {
    dead_ids[i] = (size_t)dead_ids_raw[i];
  }

  *out_dead_ids = dead_ids;
  *out_num_dead = num_dead;
  *out_bytes_freed = bytes_freed;

  os::free(resp);
}

void TCPExecutorBackend::discard_slot(size_t slot_id) {
  if (!_connected) return;

  io_lock();

  uint8_t msg[24];
  *(uint32_t*)(msg + 0) = RE_CMD_DISCARD_SLOT;
  *(uint32_t*)(msg + 4) = 24;
  *(uint64_t*)(msg + 8) = _seq_id++;
  *(uint64_t*)(msg + 16) = slot_id;
  send_msg(msg, 24);

  uint8_t resp[64];
  size_t resp_len = 0;
  recv_msg(resp, sizeof(resp), &resp_len);

  io_unlock();
}

size_t TCPExecutorBackend::slot_word_size(size_t slot_id) const {
  return 0;
}

// ============================================================
// V2 Protocol Implementations
// ============================================================

size_t TCPExecutorBackend::evict_with_edges(const void* obj_bytes, size_t word_size,
                                            Klass* klass, uintptr_t handle_id,
                                            const EdgeInfo* edges, uint32_t num_edges,
                                            size_t hint_slot_id) {
  if (!_connected) return (size_t)-1;

  io_lock();

  size_t slot_id = (hint_slot_id == (size_t)-1) ? _next_slot++ : hint_slot_id;
  size_t byte_size = word_size * HeapWordSize;
  // Header(16) + slot_id(8) + handle_id(8) + klass(8) + word_size(4) + num_edges(4) + bytes + edges
  size_t edge_bytes = num_edges * (4 + 8);  // field_offset(4) + target_handle_id(8)
  size_t msg_size = 16 + 8 + 8 + 8 + 4 + 4 + byte_size + edge_bytes;
  uint8_t* msg = (uint8_t*)os::malloc(msg_size, mtGC);

  *(uint32_t*)(msg + 0) = RE_CMD_EVICT_WITH_EDGES;
  *(uint32_t*)(msg + 4) = (uint32_t)msg_size;
  *(uint64_t*)(msg + 8) = _seq_id++;
  *(uint64_t*)(msg + 16) = slot_id;
  *(uint64_t*)(msg + 24) = handle_id;
  *(uint64_t*)(msg + 32) = (uint64_t)(uintptr_t)klass;
  *(uint32_t*)(msg + 40) = (uint32_t)word_size;
  *(uint32_t*)(msg + 44) = num_edges;
  memcpy(msg + 48, obj_bytes, byte_size);
  // Pack edges
  uint8_t* edge_ptr = msg + 48 + byte_size;
  for (uint32_t i = 0; i < num_edges; i++) {
    *(uint32_t*)(edge_ptr) = edges[i].field_offset;
    *(uint64_t*)(edge_ptr + 4) = edges[i].target_handle_id;
    edge_ptr += 12;
  }

  send_msg(msg, msg_size);
  os::free(msg);

  uint8_t resp[64];
  size_t resp_len = 0;
  recv_msg(resp, sizeof(resp), &resp_len);

  io_unlock();

  if (resp_len >= 4 && *(uint32_t*)resp == RE_RESP_OK) {
    _total_evicted++;
    return slot_id;
  }
  return (size_t)-1;
}

void TCPExecutorBackend::localize_batch(const uintptr_t* handle_ids, size_t count) {
  if (!_connected || count == 0) return;

  io_lock();

  size_t msg_size = 16 + 4 + count * 8;
  uint8_t* msg = (uint8_t*)os::malloc(msg_size, mtGC);
  *(uint32_t*)(msg + 0) = RE_CMD_LOCALIZE_BATCH;
  *(uint32_t*)(msg + 4) = (uint32_t)msg_size;
  *(uint64_t*)(msg + 8) = _seq_id++;
  *(uint32_t*)(msg + 16) = (uint32_t)count;
  memcpy(msg + 20, handle_ids, count * 8);

  send_msg(msg, msg_size);
  os::free(msg);

  uint8_t resp[64];
  size_t resp_len = 0;
  recv_msg(resp, sizeof(resp), &resp_len);

  io_unlock();
}

void TCPExecutorBackend::report_remote_roots_v2(const uintptr_t* handle_ids, size_t count) {
  if (!_connected) return;

  io_lock();

  // Executor recv buffer is REMOTE_MAX_MSG_SIZE (4MB). Chunk if needed.
  size_t max_per_msg = (4 * 1024 * 1024 - 20) / 8;
  size_t remaining = count;
  size_t offset = 0;

  // Clear first so executor doesn't accumulate across GC cycles
  {
    uint8_t clear_msg[20];
    *(uint32_t*)(clear_msg + 0) = RE_CMD_REPORT_REMOTE_ROOTS_V2;
    *(uint32_t*)(clear_msg + 4) = 20;
    *(uint64_t*)(clear_msg + 8) = _seq_id++;
    *(uint32_t*)(clear_msg + 16) = 0;
    send_msg(clear_msg, 20);
    uint8_t resp[64]; size_t resp_len = 0;
    recv_msg(resp, sizeof(resp), &resp_len);
  }

  while (remaining > 0) {
    size_t chunk = MIN2(remaining, max_per_msg);
    size_t msg_size = 20 + chunk * 8;
    uint8_t* msg = (uint8_t*)os::malloc(msg_size, mtGC);
    *(uint32_t*)(msg + 0) = RE_CMD_REPORT_REMOTE_ROOTS_V2;
    *(uint32_t*)(msg + 4) = (uint32_t)msg_size;
    *(uint64_t*)(msg + 8) = _seq_id++;
    *(uint32_t*)(msg + 16) = (uint32_t)chunk;
    memcpy(msg + 20, handle_ids + offset, chunk * 8);

    send_msg(msg, msg_size);
    os::free(msg);

    uint8_t resp[64]; size_t resp_len = 0;
    recv_msg(resp, sizeof(resp), &resp_len);

    offset += chunk;
    remaining -= chunk;
  }

  io_unlock();
}

void TCPExecutorBackend::trace_and_report(uintptr_t** out_dead_ids, size_t* out_num_dead,
                                          size_t* out_bytes_freed,
                                          uintptr_t** out_cross_src, uintptr_t** out_cross_tgt,
                                          size_t* out_num_cross) {
  *out_dead_ids = nullptr; *out_num_dead = 0; *out_bytes_freed = 0;
  *out_cross_src = nullptr; *out_cross_tgt = nullptr; *out_num_cross = 0;
  if (!_connected) return;

  io_lock();

  uint8_t msg[16];
  *(uint32_t*)(msg + 0) = RE_CMD_TRACE_AND_REPORT;
  *(uint32_t*)(msg + 4) = 16;
  *(uint64_t*)(msg + 8) = _seq_id++;
  if (!send_msg(msg, 16)) { io_unlock(); return; }

  // RESP_TRACE_RESULT can be large: dead_handle_ids + cross_edges
  uint8_t* resp = (uint8_t*)os::malloc(4 * 1024 * 1024, mtGC);
  size_t resp_len = 0;
  if (!recv_msg(resp, 4 * 1024 * 1024, &resp_len)) {
    os::free(resp);
    io_unlock();
    return;
  }

  io_unlock();

  if (resp_len < 4 || *(uint32_t*)resp != RE_RESP_TRACE_RESULT) {
    os::free(resp);
    return;
  }

  // Parse RESP_TRACE_RESULT:
  // hdr(16) + num_dead(4) + num_live(4) + bytes_freed(8) + num_cross_edges(4) + _pad(4)
  // = 40 bytes header, then dead_handle_ids[num_dead], then cross_edges[num_cross_edges]
  uint32_t num_dead       = *(uint32_t*)(resp + 16);
  uint32_t num_live       = *(uint32_t*)(resp + 20);
  uint64_t bytes_freed    = *(uint64_t*)(resp + 24);
  uint32_t num_cross      = *(uint32_t*)(resp + 32);

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
    // Cross-edges start after dead_handle_ids
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

void TCPExecutorBackend::directory_upsert(const uintptr_t* handle_ids,
                                          const uint32_t* states,
                                          const size_t* slot_ids,
                                          size_t count) {
  if (!_connected || count == 0) return;

  io_lock();

  size_t msg_size = 16 + 4 + count * 20;
  uint8_t* msg = (uint8_t*)os::malloc(msg_size, mtGC);
  *(uint32_t*)(msg + 0) = RE_CMD_DIRECTORY_UPSERT;
  *(uint32_t*)(msg + 4) = (uint32_t)msg_size;
  *(uint64_t*)(msg + 8) = _seq_id++;
  *(uint32_t*)(msg + 16) = (uint32_t)count;

  uint8_t* ptr = msg + 20;
  for (size_t i = 0; i < count; i++) {
    *(uint64_t*)(ptr) = handle_ids[i];
    *(uint32_t*)(ptr + 8) = states[i];
    *(uint64_t*)(ptr + 12) = slot_ids[i];
    ptr += 20;
  }

  send_msg(msg, msg_size);
  os::free(msg);

  uint8_t resp[64];
  size_t resp_len = 0;
  recv_msg(resp, sizeof(resp), &resp_len);

  io_unlock();
}
