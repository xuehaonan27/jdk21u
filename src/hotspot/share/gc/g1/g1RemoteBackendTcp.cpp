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
static const uint32_t RE_CMD_FETCH_AROUND          = 0x18;
static const uint32_t RE_CMD_FETCH_EXACT           = 0x1C;
static const uint32_t RE_CMD_EVICT_SEGMENT         = 0x1D;
static const uint32_t RE_CMD_FETCH_SEGMENT         = 0x1E;
static const uint32_t RE_CMD_DISCARD_SEGMENT       = 0x1F;
static const uint32_t RE_CMD_EVICT_SEGMENT_STAGED  = 0x20;
static const uint32_t RE_CMD_FETCH_SEGMENT_STAGED  = 0x21;
static const uint32_t RE_CMD_STAGE_WRITE           = 0x22;
static const uint32_t RE_CMD_STAGE_READ            = 0x23;
static const uint32_t RE_RESP_TRACE_RESULT          = 0x86;
static const uint32_t RE_RESP_BATCH_OBJECT_DATA     = 0x87;
static const uint32_t RE_RESP_SEGMENT_DATA          = 0x88;
static const uint32_t RE_RESP_SEGMENT_STAGED        = 0x89;
static const uint32_t RE_RESP_STAGE_DATA            = 0x8A;
static const size_t   RE_MAX_MSG_SIZE              = 16 * 1024 * 1024;

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
    log_warning(gc)("TCPExecutor: message too large: %u > %zu — draining", net_len, max_len);
    // Drain the oversized message to keep the TCP stream synchronized
    uint8_t drain[4096];
    size_t remaining = net_len;
    while (remaining > 0) {
      size_t chunk = (remaining < sizeof(drain)) ? remaining : sizeof(drain);
      if (!recv_all(drain, chunk)) return false;
      remaining -= chunk;
    }
    return false;
  }
  if (!recv_all(buf, net_len)) return false;
  if (actual_len) *actual_len = net_len;
  return true;
}

bool TCPExecutorBackend::stage_write_locked(uint64_t remote_offset,
                                            const void* data,
                                            size_t len) {
  const size_t header_size = 32;
  const size_t max_payload = RE_MAX_MSG_SIZE - header_size;
  const uint8_t* src = (const uint8_t*)data;
  size_t done = 0;

  while (done < len) {
    size_t chunk = MIN2(len - done, max_payload);
    size_t msg_size = header_size + chunk;
    uint8_t* msg = (uint8_t*)os::malloc(msg_size, mtGC);
    if (msg == nullptr) {
      return false;
    }

    *(uint32_t*)(msg + 0) = RE_CMD_STAGE_WRITE;
    *(uint32_t*)(msg + 4) = (uint32_t)msg_size;
    *(uint64_t*)(msg + 8) = _seq_id++;
    *(uint64_t*)(msg + 16) = remote_offset + done;
    *(uint64_t*)(msg + 24) = (uint64_t)chunk;
    memcpy(msg + header_size, src + done, chunk);

    bool sent = send_msg(msg, msg_size);
    os::free(msg);
    if (!sent) {
      return false;
    }

    uint8_t resp[64];
    size_t resp_len = 0;
    if (!recv_msg(resp, sizeof(resp), &resp_len) ||
        resp_len < 16 || *(uint32_t*)resp != RE_RESP_OK) {
      log_warning(gc)("TCPExecutor: staged write failed at offset="
                      UINT64_FORMAT " chunk=" SIZE_FORMAT,
                      remote_offset + done, chunk);
      return false;
    }

    done += chunk;
  }

  return true;
}

bool TCPExecutorBackend::stage_read_locked(uint64_t remote_offset,
                                           void* dest,
                                           size_t len) {
  const size_t request_size = 32;
  const size_t response_header_size = 32;
  const size_t max_payload = RE_MAX_MSG_SIZE - response_header_size;
  uint8_t* dst = (uint8_t*)dest;
  size_t done = 0;

  while (done < len) {
    size_t chunk = MIN2(len - done, max_payload);
    uint8_t msg[request_size];
    *(uint32_t*)(msg + 0) = RE_CMD_STAGE_READ;
    *(uint32_t*)(msg + 4) = request_size;
    *(uint64_t*)(msg + 8) = _seq_id++;
    *(uint64_t*)(msg + 16) = remote_offset + done;
    *(uint64_t*)(msg + 24) = (uint64_t)chunk;

    if (!send_msg(msg, sizeof(msg))) {
      return false;
    }

    size_t resp_cap = response_header_size + chunk;
    uint8_t* resp = (uint8_t*)os::malloc(resp_cap, mtGC);
    if (resp == nullptr) {
      return false;
    }

    size_t resp_len = 0;
    bool ok = recv_msg(resp, resp_cap, &resp_len);
    if (ok && resp_len >= response_header_size &&
        *(uint32_t*)(resp + 0) == RE_RESP_STAGE_DATA) {
      uint32_t resp_msg_len = *(uint32_t*)(resp + 4);
      uint64_t resp_offset = *(uint64_t*)(resp + 16);
      size_t resp_byte_size = (size_t)*(uint64_t*)(resp + 24);
      ok = resp_msg_len <= resp_len &&
           resp_msg_len >= response_header_size &&
           resp_offset == remote_offset + done &&
           resp_byte_size == chunk &&
           resp_byte_size <= resp_msg_len - response_header_size;
      if (ok) {
        memcpy(dst + done, resp + response_header_size, chunk);
      }
    } else {
      ok = false;
    }

    os::free(resp);
    if (!ok) {
      log_warning(gc)("TCPExecutor: staged read failed at offset="
                      UINT64_FORMAT " chunk=" SIZE_FORMAT,
                      remote_offset + done, chunk);
      return false;
    }

    done += chunk;
  }

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
    uint8_t msg[16];
    *(uint32_t*)(msg + 0) = RE_CMD_SHUTDOWN;
    *(uint32_t*)(msg + 4) = sizeof(msg);
    *(uint64_t*)(msg + 8) = _seq_id++;
    send_msg(msg, sizeof(msg));  // best-effort
    ::close(_fd);
    _fd = -1;
    _connected = false;
  }
}

bool TCPExecutorBackend::supports_segments() const {
  return true;
}

bool TCPExecutorBackend::evict_segment(uint64_t segment_id, uintptr_t vaddr_base,
                                       const void* bytes, size_t byte_size,
                                       uint32_t flags) {
  if (!_connected || segment_id == 0 || bytes == nullptr || byte_size == 0) {
    return false;
  }

  const size_t header_size = 48;
  bool use_staged = RDMAMsgBufSize <= header_size ||
                    byte_size > RDMAMsgBufSize - header_size ||
                    byte_size > RE_MAX_MSG_SIZE - header_size;

  if (use_staged) {
    const size_t staged_msg_size = 56;
    uint8_t msg[staged_msg_size];

    io_lock();
    if (!stage_write_locked(0, bytes, byte_size)) {
      io_unlock();
      return false;
    }

    *(uint32_t*)(msg + 0) = RE_CMD_EVICT_SEGMENT_STAGED;
    *(uint32_t*)(msg + 4) = staged_msg_size;
    *(uint64_t*)(msg + 8) = _seq_id++;
    *(uint64_t*)(msg + 16) = segment_id;
    *(uint64_t*)(msg + 24) = (uint64_t)vaddr_base;
    *(uint64_t*)(msg + 32) = (uint64_t)byte_size;
    *(uint32_t*)(msg + 40) = flags;
    *(uint32_t*)(msg + 44) = 0;
    *(uint64_t*)(msg + 48) = 0;

    bool sent = send_msg(msg, sizeof(msg));
    uint8_t resp[64];
    size_t resp_len = 0;
    bool ok = sent &&
              recv_msg(resp, sizeof(resp), &resp_len) &&
              resp_len >= 16 && *(uint32_t*)resp == RE_RESP_OK;
    io_unlock();
    return ok;
  }

  if (byte_size > (size_t)UINT32_MAX - header_size) {
    return false;
  }

  size_t msg_size = header_size + byte_size;
  uint8_t* msg = (uint8_t*)os::malloc(msg_size, mtGC);
  if (msg == nullptr) {
    return false;
  }

  *(uint32_t*)(msg + 0) = RE_CMD_EVICT_SEGMENT;
  *(uint32_t*)(msg + 4) = (uint32_t)msg_size;
  *(uint64_t*)(msg + 8) = 0;
  *(uint64_t*)(msg + 16) = segment_id;
  *(uint64_t*)(msg + 24) = (uint64_t)vaddr_base;
  *(uint64_t*)(msg + 32) = (uint64_t)byte_size;
  *(uint32_t*)(msg + 40) = flags;
  *(uint32_t*)(msg + 44) = 0;
  memcpy(msg + header_size, bytes, byte_size);

  io_lock();
  *(uint64_t*)(msg + 8) = _seq_id++;
  bool sent = send_msg(msg, msg_size);
  os::free(msg);
  if (!sent) {
    io_unlock();
    return false;
  }

  uint8_t resp[64];
  size_t resp_len = 0;
  bool ok = recv_msg(resp, sizeof(resp), &resp_len) &&
            resp_len >= 16 && *(uint32_t*)resp == RE_RESP_OK;
  io_unlock();
  return ok;
}

bool TCPExecutorBackend::fetch_segment(uint64_t segment_id,
                                       uintptr_t* out_vaddr_base,
                                       void* dest,
                                       size_t byte_capacity,
                                       size_t* out_byte_size,
                                       uint32_t* out_flags) {
  if (!_connected || segment_id == 0 || dest == nullptr || byte_capacity == 0) {
    return false;
  }

  const size_t header_size = 48;
  bool use_staged = RDMAMsgBufSize <= header_size ||
                    byte_capacity > RDMAMsgBufSize - header_size ||
                    byte_capacity > RE_MAX_MSG_SIZE - header_size;

  if (use_staged) {
    io_lock();

    uint8_t msg[40];
    *(uint32_t*)(msg + 0) = RE_CMD_FETCH_SEGMENT_STAGED;
    *(uint32_t*)(msg + 4) = sizeof(msg);
    *(uint64_t*)(msg + 8) = _seq_id++;
    *(uint64_t*)(msg + 16) = segment_id;
    *(uint64_t*)(msg + 24) = 0;
    *(uint64_t*)(msg + 32) = (uint64_t)byte_capacity;
    if (!send_msg(msg, sizeof(msg))) {
      io_unlock();
      return false;
    }

    uint8_t resp[64];
    size_t resp_len = 0;
    if (!recv_msg(resp, sizeof(resp), &resp_len) || resp_len < header_size ||
        *(uint32_t*)(resp + 0) != RE_RESP_SEGMENT_STAGED) {
      io_unlock();
      return false;
    }

    uint32_t resp_msg_len = *(uint32_t*)(resp + 4);
    uint64_t resp_segment_id = *(uint64_t*)(resp + 16);
    uintptr_t resp_vaddr_base = (uintptr_t)*(uint64_t*)(resp + 24);
    size_t resp_byte_size = (size_t)*(uint64_t*)(resp + 32);
    uint32_t resp_flags = *(uint32_t*)(resp + 40);

    if (resp_msg_len > resp_len || resp_msg_len < header_size ||
        resp_segment_id != segment_id || resp_byte_size > byte_capacity) {
      io_unlock();
      return false;
    }

    bool ok = stage_read_locked(0, dest, resp_byte_size);
    io_unlock();
    if (!ok) {
      return false;
    }

    if (out_vaddr_base != nullptr) {
      *out_vaddr_base = resp_vaddr_base;
    }
    if (out_byte_size != nullptr) {
      *out_byte_size = resp_byte_size;
    }
    if (out_flags != nullptr) {
      *out_flags = resp_flags;
    }
    return true;
  }

  size_t max_response_bytes = MIN2(RE_MAX_MSG_SIZE, byte_capacity + header_size);
  if (max_response_bytes < byte_capacity) {
    max_response_bytes = RE_MAX_MSG_SIZE;
  }

  io_lock();

  uint8_t msg[32];
  *(uint32_t*)(msg + 0) = RE_CMD_FETCH_SEGMENT;
  *(uint32_t*)(msg + 4) = sizeof(msg);
  *(uint64_t*)(msg + 8) = _seq_id++;
  *(uint64_t*)(msg + 16) = segment_id;
  *(uint64_t*)(msg + 24) = (uint64_t)max_response_bytes;
  if (!send_msg(msg, sizeof(msg))) {
    io_unlock();
    return false;
  }

  uint32_t net_len = 0;
  if (!recv_all(&net_len, 4)) {
    io_unlock();
    return false;
  }

  if (net_len > RE_MAX_MSG_SIZE) {
    uint8_t drain[4096];
    size_t remaining = net_len;
    while (remaining > 0) {
      size_t chunk = MIN2(remaining, sizeof(drain));
      if (!recv_all(drain, chunk)) {
        break;
      }
      remaining -= chunk;
    }
    io_unlock();
    return false;
  }

  uint8_t* resp = (uint8_t*)os::malloc(net_len, mtGC);
  if (resp == nullptr) {
    io_unlock();
    return false;
  }
  if (!recv_all(resp, net_len)) {
    os::free(resp);
    io_unlock();
    return false;
  }

  io_unlock();

  if (net_len < header_size || *(uint32_t*)resp != RE_RESP_SEGMENT_DATA) {
    os::free(resp);
    return false;
  }

  uint32_t resp_msg_len = *(uint32_t*)(resp + 4);
  uint64_t resp_segment_id = *(uint64_t*)(resp + 16);
  uintptr_t resp_vaddr_base = (uintptr_t)*(uint64_t*)(resp + 24);
  size_t resp_byte_size = (size_t)*(uint64_t*)(resp + 32);
  uint32_t resp_flags = *(uint32_t*)(resp + 40);

  if (resp_msg_len > net_len || resp_msg_len > max_response_bytes ||
      resp_msg_len < header_size || resp_segment_id != segment_id ||
      resp_byte_size > byte_capacity ||
      resp_byte_size > resp_msg_len - header_size) {
    log_warning(gc)("TCPExecutor: malformed segment fetch response: segment="
                    UINT64_FORMAT " hdr_len=%u actual=%u bytes="
                    SIZE_FORMAT " cap=" SIZE_FORMAT,
                    segment_id, resp_msg_len, net_len, resp_byte_size,
                    byte_capacity);
    os::free(resp);
    return false;
  }

  memcpy(dest, resp + header_size, resp_byte_size);
  if (out_vaddr_base != nullptr) {
    *out_vaddr_base = resp_vaddr_base;
  }
  if (out_byte_size != nullptr) {
    *out_byte_size = resp_byte_size;
  }
  if (out_flags != nullptr) {
    *out_flags = resp_flags;
  }
  os::free(resp);
  return true;
}

bool TCPExecutorBackend::fetch_segment_part(uint64_t segment_id,
                                            size_t segment_offset,
                                            void* dest,
                                            size_t byte_size,
                                            uintptr_t* out_vaddr_base,
                                            uint32_t* out_flags) {
  if (!_connected || segment_id == 0 || dest == nullptr || byte_size == 0) {
    return false;
  }

  const size_t header_size = 48;
  bool use_staged = RDMAMsgBufSize <= header_size ||
                    byte_size > RDMAMsgBufSize - header_size ||
                    byte_size > RE_MAX_MSG_SIZE - header_size;

  if (use_staged) {
    io_lock();

    uint8_t msg[56];
    *(uint32_t*)(msg + 0) = RE_CMD_FETCH_SEGMENT_STAGED;
    *(uint32_t*)(msg + 4) = sizeof(msg);
    *(uint64_t*)(msg + 8) = _seq_id++;
    *(uint64_t*)(msg + 16) = segment_id;
    *(uint64_t*)(msg + 24) = 0;
    *(uint64_t*)(msg + 32) = (uint64_t)byte_size;
    *(uint64_t*)(msg + 40) = (uint64_t)segment_offset;
    *(uint64_t*)(msg + 48) = (uint64_t)byte_size;
    if (!send_msg(msg, sizeof(msg))) {
      io_unlock();
      return false;
    }

    uint8_t resp[64];
    size_t resp_len = 0;
    if (!recv_msg(resp, sizeof(resp), &resp_len) || resp_len < header_size ||
        *(uint32_t*)(resp + 0) != RE_RESP_SEGMENT_STAGED) {
      io_unlock();
      return false;
    }

    uint32_t resp_msg_len = *(uint32_t*)(resp + 4);
    uint64_t resp_segment_id = *(uint64_t*)(resp + 16);
    uintptr_t resp_vaddr_base = (uintptr_t)*(uint64_t*)(resp + 24);
    size_t resp_byte_size = (size_t)*(uint64_t*)(resp + 32);
    uint32_t resp_flags = *(uint32_t*)(resp + 40);

    if (resp_msg_len > resp_len || resp_msg_len < header_size ||
        resp_segment_id != segment_id || resp_byte_size != byte_size) {
      io_unlock();
      return false;
    }

    bool ok = stage_read_locked(0, dest, resp_byte_size);
    io_unlock();
    if (!ok) {
      return false;
    }

    if (out_vaddr_base != nullptr) {
      *out_vaddr_base = resp_vaddr_base;
    }
    if (out_flags != nullptr) {
      *out_flags = resp_flags;
    }
    return true;
  }

  size_t max_response_bytes = MIN2(RE_MAX_MSG_SIZE, byte_size + header_size);
  if (max_response_bytes < byte_size) {
    max_response_bytes = RE_MAX_MSG_SIZE;
  }

  io_lock();

  uint8_t msg[48];
  *(uint32_t*)(msg + 0) = RE_CMD_FETCH_SEGMENT;
  *(uint32_t*)(msg + 4) = sizeof(msg);
  *(uint64_t*)(msg + 8) = _seq_id++;
  *(uint64_t*)(msg + 16) = segment_id;
  *(uint64_t*)(msg + 24) = (uint64_t)max_response_bytes;
  *(uint64_t*)(msg + 32) = (uint64_t)segment_offset;
  *(uint64_t*)(msg + 40) = (uint64_t)byte_size;
  if (!send_msg(msg, sizeof(msg))) {
    io_unlock();
    return false;
  }

  uint32_t net_len = 0;
  if (!recv_all(&net_len, 4)) {
    io_unlock();
    return false;
  }

  if (net_len > RE_MAX_MSG_SIZE) {
    uint8_t drain[4096];
    size_t remaining = net_len;
    while (remaining > 0) {
      size_t chunk = MIN2(remaining, sizeof(drain));
      if (!recv_all(drain, chunk)) {
        break;
      }
      remaining -= chunk;
    }
    io_unlock();
    return false;
  }

  uint8_t* resp = (uint8_t*)os::malloc(net_len, mtGC);
  if (resp == nullptr) {
    io_unlock();
    return false;
  }
  if (!recv_all(resp, net_len)) {
    os::free(resp);
    io_unlock();
    return false;
  }

  io_unlock();

  if (net_len < header_size || *(uint32_t*)resp != RE_RESP_SEGMENT_DATA) {
    os::free(resp);
    return false;
  }

  uint32_t resp_msg_len = *(uint32_t*)(resp + 4);
  uint64_t resp_segment_id = *(uint64_t*)(resp + 16);
  uintptr_t resp_vaddr_base = (uintptr_t)*(uint64_t*)(resp + 24);
  size_t resp_byte_size = (size_t)*(uint64_t*)(resp + 32);
  uint32_t resp_flags = *(uint32_t*)(resp + 40);

  if (resp_msg_len > net_len || resp_msg_len > max_response_bytes ||
      resp_msg_len < header_size || resp_segment_id != segment_id ||
      resp_byte_size != byte_size ||
      resp_byte_size > resp_msg_len - header_size) {
    log_warning(gc)("TCPExecutor: malformed partial segment fetch response: "
                    "segment=" UINT64_FORMAT " offset=" SIZE_FORMAT
                    " hdr_len=%u actual=%u bytes=" SIZE_FORMAT
                    " expected=" SIZE_FORMAT,
                    segment_id, segment_offset, resp_msg_len, net_len,
                    resp_byte_size, byte_size);
    os::free(resp);
    return false;
  }

  memcpy(dest, resp + header_size, resp_byte_size);
  if (out_vaddr_base != nullptr) {
    *out_vaddr_base = resp_vaddr_base;
  }
  if (out_flags != nullptr) {
    *out_flags = resp_flags;
  }
  os::free(resp);
  return true;
}

void TCPExecutorBackend::discard_segment(uint64_t segment_id) {
  if (!_connected || segment_id == 0) {
    return;
  }

  io_lock();

  uint8_t msg[24];
  *(uint32_t*)(msg + 0) = RE_CMD_DISCARD_SEGMENT;
  *(uint32_t*)(msg + 4) = sizeof(msg);
  *(uint64_t*)(msg + 8) = _seq_id++;
  *(uint64_t*)(msg + 16) = segment_id;
  send_msg(msg, sizeof(msg));

  uint8_t resp[64];
  size_t resp_len = 0;
  recv_msg(resp, sizeof(resp), &resp_len);

  io_unlock();
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

  // Read length prefix first, then allocate appropriately
  uint32_t net_len = 0;
  if (!recv_all(&net_len, 4)) { io_unlock(); return nullptr; }

  size_t alloc_len = (net_len < 4096) ? 4096 : net_len;
  uint8_t* resp = (uint8_t*)os::malloc(alloc_len, mtGC);
  if (!recv_all(resp, net_len)) {
    os::free(resp);
    io_unlock();
    return nullptr;
  }

  io_unlock();

  if (*(uint32_t*)resp != RE_RESP_OBJECT_DATA) {
    os::free(resp);
    return nullptr;
  }

  if (net_len < 36) {
    log_warning(gc)("TCPExecutor: fetch response too short: %u bytes", net_len);
    os::free(resp);
    return nullptr;
  }

  uint64_t resp_klass_val = *(uint64_t*)(resp + 24);
  uint32_t resp_ws = *(uint32_t*)(resp + 32);
  size_t byte_size = resp_ws * HeapWordSize;

  if (36 + byte_size > net_len) {
    log_warning(gc)("TCPExecutor: fetch resp_ws=%u claims %zuB but response only %u bytes",
                    resp_ws, byte_size, net_len);
    os::free(resp);
    return nullptr;
  }

  memcpy(dest, resp + 36, byte_size);

  if (out_word_size) *out_word_size = resp_ws;
  os::free(resp);
  _total_fetched++;
  return (Klass*)resp_klass_val;
}

static size_t parse_tcp_batch_fetch_response(const char* caller,
                                             uint8_t* resp,
                                             size_t resp_len,
                                             size_t max_response_bytes,
                                             G1RemoteBackend::FetchBatchClosure* cl) {
  if (resp_len < 24) {
    log_warning(gc)("TCPExecutor: %s response too short: " SIZE_FORMAT " bytes",
                    caller, resp_len);
    return 0;
  }

  if (*(uint32_t*)resp != RE_RESP_BATCH_OBJECT_DATA) {
    log_warning(gc)("TCPExecutor: %s unexpected response cmd=0x%x",
                    caller, *(uint32_t*)resp);
    return 0;
  }

  uint32_t resp_msg_len = *(uint32_t*)(resp + 4);
  if (resp_msg_len > resp_len || resp_msg_len > max_response_bytes) {
    log_warning(gc)("TCPExecutor: %s malformed response length: hdr=%u "
                    "actual=" SIZE_FORMAT " max=" SIZE_FORMAT,
                    caller, resp_msg_len, resp_len, max_response_bytes);
    return 0;
  }

  uint32_t num_objects = *(uint32_t*)(resp + 16);
  uint8_t* cursor = resp + 24;
  uint8_t* end = resp + resp_msg_len;
  size_t fetched = 0;

  for (uint32_t i = 0; i < num_objects; i++) {
    if (cursor + 32 > end) {
      log_warning(gc)("TCPExecutor: %s truncated entry header at %u/%u",
                      caller, i, num_objects);
      break;
    }
    uintptr_t entry_handle = (uintptr_t)*(uint64_t*)(cursor + 0);
    size_t entry_slot = (size_t)*(uint64_t*)(cursor + 8);
    Klass* entry_klass = (Klass*)(uintptr_t)*(uint64_t*)(cursor + 16);
    uint32_t word_size = *(uint32_t*)(cursor + 24);
    uint32_t byte_size = *(uint32_t*)(cursor + 28);
    cursor += 32;

    if ((size_t)word_size * HeapWordSize != byte_size || cursor + byte_size > end) {
      log_warning(gc)("TCPExecutor: %s malformed entry %u/%u "
                      "(ws=%u bytes=%u remaining=" SIZE_FORMAT ")",
                      caller, i, num_objects, word_size, byte_size,
                      (size_t)(end - cursor));
      break;
    }

    cl->do_object(entry_handle, entry_slot, entry_klass, word_size, cursor);
    cursor += byte_size;
    fetched++;
  }

  return fetched;
}

bool TCPExecutorBackend::supports_batch_fetch() const {
  return true;
}

size_t TCPExecutorBackend::fetch_batch_around(uintptr_t handle_id, size_t slot_id,
                                              uint max_objects, uint slot_window,
                                              size_t max_response_bytes,
                                              FetchBatchClosure* cl) {
  if (!_connected || cl == nullptr || max_objects == 0) {
    return 0;
  }

  if (max_response_bytes == 0 || max_response_bytes > RE_MAX_MSG_SIZE) {
    max_response_bytes = RE_MAX_MSG_SIZE;
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

  uint8_t msg[48];
  *(uint32_t*)(msg + 0) = RE_CMD_FETCH_AROUND;
  *(uint32_t*)(msg + 4) = sizeof(msg);
  *(uint64_t*)(msg + 8) = 0;
  *(uint64_t*)(msg + 16) = (uint64_t)handle_id;
  *(uint64_t*)(msg + 24) = (uint64_t)slot_id;
  *(uint32_t*)(msg + 32) = max_objects;
  *(uint32_t*)(msg + 36) = slot_window;
  *(uint64_t*)(msg + 40) = (uint64_t)max_response_bytes;

  uint8_t* resp = (uint8_t*)os::malloc(max_response_bytes, mtGC);
  if (resp == nullptr) {
    return 0;
  }

  io_lock();
  *(uint64_t*)(msg + 8) = _seq_id++;
  bool ok = send_msg(msg, sizeof(msg));
  size_t resp_len = 0;
  if (ok) {
    ok = recv_msg(resp, max_response_bytes, &resp_len);
  }
  io_unlock();

  if (!ok) {
    os::free(resp);
    return 0;
  }

  size_t fetched = parse_tcp_batch_fetch_response("fetch_batch_around",
                                                  resp, resp_len,
                                                  max_response_bytes, cl);
  os::free(resp);
  _total_fetched += fetched;
  return fetched;
}

bool TCPExecutorBackend::supports_exact_batch_fetch() const {
  return true;
}

size_t TCPExecutorBackend::fetch_batch_exact(const uintptr_t* handle_ids,
                                             const size_t* slot_ids,
                                             size_t count,
                                             size_t max_response_bytes,
                                             FetchBatchClosure* cl) {
  if (!_connected || cl == nullptr || handle_ids == nullptr ||
      slot_ids == nullptr || count == 0) {
    return 0;
  }

  if (max_response_bytes == 0 || max_response_bytes > RE_MAX_MSG_SIZE) {
    max_response_bytes = RE_MAX_MSG_SIZE;
  }
  if (max_response_bytes < 24 + 32) {
    return 0;
  }

  const size_t header_size = 32;
  const size_t entry_size = 16;
  size_t max_by_request = (RE_MAX_MSG_SIZE > header_size) ?
      ((RE_MAX_MSG_SIZE - header_size) / entry_size) : 0;
  size_t max_by_response = (max_response_bytes - 24) / 32;
  size_t max_objects = MIN2(max_by_request, max_by_response);
  if (count > max_objects) {
    count = max_objects;
  }
  if (count == 0) {
    return 0;
  }

  size_t msg_size = header_size + count * entry_size;
  uint8_t* msg = (uint8_t*)os::malloc(msg_size, mtGC);
  uint8_t* resp = (uint8_t*)os::malloc(max_response_bytes, mtGC);
  if (msg == nullptr || resp == nullptr) {
    if (msg != nullptr) os::free(msg);
    if (resp != nullptr) os::free(resp);
    return 0;
  }

  *(uint32_t*)(msg + 0) = RE_CMD_FETCH_EXACT;
  *(uint32_t*)(msg + 4) = (uint32_t)msg_size;
  *(uint64_t*)(msg + 8) = 0;
  *(uint32_t*)(msg + 16) = (uint32_t)count;
  *(uint32_t*)(msg + 20) = 0;
  *(uint64_t*)(msg + 24) = (uint64_t)max_response_bytes;
  uint8_t* cursor = msg + header_size;
  for (size_t i = 0; i < count; i++) {
    *(uint64_t*)(cursor + 0) = (uint64_t)handle_ids[i];
    *(uint64_t*)(cursor + 8) = (uint64_t)slot_ids[i];
    cursor += entry_size;
  }

  io_lock();
  *(uint64_t*)(msg + 8) = _seq_id++;
  bool ok = send_msg(msg, msg_size);
  size_t resp_len = 0;
  if (ok) {
    ok = recv_msg(resp, max_response_bytes, &resp_len);
  }
  io_unlock();

  os::free(msg);
  if (!ok) {
    os::free(resp);
    return 0;
  }

  size_t fetched = parse_tcp_batch_fetch_response("fetch_batch_exact",
                                                  resp, resp_len,
                                                  max_response_bytes, cl);
  os::free(resp);
  _total_fetched += fetched;
  return fetched;
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

  // Executor recv buffer is REMOTE_MAX_MSG_SIZE. Chunk if needed.
  size_t max_per_msg = (RE_MAX_MSG_SIZE - 20) / 8;
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
