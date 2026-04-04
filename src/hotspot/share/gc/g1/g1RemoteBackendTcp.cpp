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
static const uint32_t RE_RESP_COLLECTION_RESULT = 0x84;

TCPExecutorBackend::TCPExecutorBackend()
  : _fd(-1), _connected(false), _seq_id(0),
    _next_slot(0), _total_evicted(0), _total_fetched(0) {}

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

  size_t slot_id = (hint_slot_id != (size_t)-1) ? hint_slot_id : _next_slot++;
  size_t byte_size = word_size * HeapWordSize;

  // CMD_EVICT_OBJECT: header(12) + slot_id(8) + klass(8) + word_size(4) + bytes
  size_t msg_size = 32 + byte_size;
  uint8_t* msg = (uint8_t*)os::malloc(msg_size, mtGC);
  *(uint32_t*)(msg + 0) = RE_CMD_EVICT_OBJECT;
  *(uint32_t*)(msg + 4) = (uint32_t)msg_size;
  *(uint64_t*)(msg + 8) = _seq_id++;
  *(uint64_t*)(msg + 16) = slot_id;        // after header: offset 12..19 in protocol, but we use simplified layout
  *(uint64_t*)(msg + 24) = (uint64_t)klass;
  // We need to fit word_size in there too. Protocol says offset 28 is word_size(4).
  // But we started payload at offset 16. Let me pack tightly:
  // [0:4] type [4:8] length [8:16] seq_id [16:24] slot_id [24:32] klass_addr
  // We're at 32 bytes for header+fields. word_size needs to go at 32.
  // But msg_size = 32 + byte_size and we copy bytes at offset 32.
  // Fix: extend header to include word_size.
  os::free(msg);

  // Corrected layout: 36 byte header + bytes
  msg_size = 36 + byte_size;
  msg = (uint8_t*)os::malloc(msg_size, mtGC);
  *(uint32_t*)(msg + 0)  = RE_CMD_EVICT_OBJECT;
  *(uint32_t*)(msg + 4)  = (uint32_t)msg_size;
  *(uint64_t*)(msg + 8)  = _seq_id++;
  *(uint64_t*)(msg + 16) = slot_id;
  *(uint64_t*)(msg + 24) = (uint64_t)klass;
  *(uint32_t*)(msg + 32) = (uint32_t)word_size;
  memcpy(msg + 36, obj_bytes, byte_size);

  bool ok = send_msg(msg, msg_size);
  os::free(msg);
  if (!ok) return (size_t)-1;

  uint8_t resp[64];
  size_t resp_len = 0;
  if (!recv_msg(resp, sizeof(resp), &resp_len)) return (size_t)-1;

  _total_evicted++;
  return slot_id;
}

Klass* TCPExecutorBackend::fetch(size_t slot_id, void* dest, size_t* out_word_size) {
  if (!_connected) return nullptr;

  // CMD_FETCH_OBJECT: header(12) + slot_id(8) = 20
  uint8_t msg[20];
  *(uint32_t*)(msg + 0) = RE_CMD_FETCH_OBJECT;
  *(uint32_t*)(msg + 4) = 20;
  *(uint64_t*)(msg + 8) = _seq_id++;
  *(uint64_t*)(msg + 12) = slot_id;

  // Hmm, the header is 12 bytes (type+length+seq_id), so slot_id starts at 12.
  // But we wrote seq_id at offset 8 (8 bytes), which overlaps with length field.
  // Let me use the proper protocol layout:
  // [0:4]=type [4:8]=length [8:16]=seq_id → that's 16 bytes for header
  // No, protocol says header is 12: type(4)+length(4)+seq_id(8) → but 4+4+8=16.
  // Actually remote_protocol.h says: uint32_t type, uint32_t length, uint64_t seq_id = 16 bytes.
  // So header is 16 bytes, not 12. Let me fix.

  // Corrected: header is 16 bytes
  uint8_t fetch_msg[24];  // header(16) + slot_id(8)
  *(uint32_t*)(fetch_msg + 0) = RE_CMD_FETCH_OBJECT;
  *(uint32_t*)(fetch_msg + 4) = 24;
  *(uint64_t*)(fetch_msg + 8) = _seq_id++;
  *(uint64_t*)(fetch_msg + 16) = slot_id;

  if (!send_msg(fetch_msg, 24)) return nullptr;

  // RESP_OBJECT_DATA: header(16) + slot_id(8) + klass(8) + word_size(4) + bytes
  uint8_t* resp = (uint8_t*)os::malloc(128 * 1024, mtGC);
  size_t resp_len = 0;
  if (!recv_msg(resp, 128 * 1024, &resp_len)) {
    os::free(resp);
    return nullptr;
  }

  if (*(uint32_t*)resp != RE_RESP_OBJECT_DATA) {
    os::free(resp);
    return nullptr;
  }

  // Parse response: header(16) + slot_id(8) + klass(8) + word_size(4) + bytes
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

  // CMD_REPORT_ROOTS: header(16) + num_roots(4) + slot_ids[num_roots * 8]
  size_t msg_size = 20 + num_roots * 8;
  uint8_t* msg = (uint8_t*)os::malloc(msg_size, mtGC);
  *(uint32_t*)(msg + 0) = RE_CMD_REPORT_ROOTS;
  *(uint32_t*)(msg + 4) = (uint32_t)msg_size;
  *(uint64_t*)(msg + 8) = _seq_id++;
  *(uint32_t*)(msg + 16) = (uint32_t)num_roots;
  // Copy slot_ids (converting size_t to uint64_t)
  uint64_t* dst_ids = (uint64_t*)(msg + 20);
  for (size_t i = 0; i < num_roots; i++) {
    dst_ids[i] = (uint64_t)root_slot_ids[i];
  }

  send_msg(msg, msg_size);
  os::free(msg);

  // Wait for OK
  uint8_t resp[64];
  size_t resp_len = 0;
  recv_msg(resp, sizeof(resp), &resp_len);
}

void TCPExecutorBackend::collect_dead(size_t** out_dead_ids, size_t* out_num_dead,
                                      size_t* out_bytes_freed) {
  if (!_connected) {
    *out_dead_ids = nullptr; *out_num_dead = 0; *out_bytes_freed = 0;
    return;
  }

  // CMD_REQUEST_COLLECTION: header only (16 bytes)
  uint8_t msg[16];
  *(uint32_t*)(msg + 0) = RE_CMD_REQUEST_COLLECTION;
  *(uint32_t*)(msg + 4) = 16;
  *(uint64_t*)(msg + 8) = _seq_id++;
  send_msg(msg, 16);

  // RESP_COLLECTION_RESULT: header(16) + num_dead(4) + num_live(4) + bytes_freed(8) + dead_ids[]
  uint8_t* resp = (uint8_t*)os::malloc(1024 * 1024, mtGC);
  size_t resp_len = 0;
  if (!recv_msg(resp, 1024 * 1024, &resp_len)) {
    os::free(resp);
    *out_dead_ids = nullptr; *out_num_dead = 0; *out_bytes_freed = 0;
    return;
  }

  if (*(uint32_t*)resp != RE_RESP_COLLECTION_RESULT) {
    os::free(resp);
    *out_dead_ids = nullptr; *out_num_dead = 0; *out_bytes_freed = 0;
    return;
  }

  // Parse: header(16) + num_dead(4) + num_live(4) + bytes_freed(8) + dead_ids[]
  uint32_t num_dead = *(uint32_t*)(resp + 16);
  // uint32_t num_live = *(uint32_t*)(resp + 20);
  uint64_t bytes_freed = *(uint64_t*)(resp + 24);
  uint64_t* dead_ids_raw = (uint64_t*)(resp + 32);

  // Convert to size_t array
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

  uint8_t msg[24];
  *(uint32_t*)(msg + 0) = RE_CMD_DISCARD_SLOT;
  *(uint32_t*)(msg + 4) = 24;
  *(uint64_t*)(msg + 8) = _seq_id++;
  *(uint64_t*)(msg + 16) = slot_id;
  send_msg(msg, 24);

  uint8_t resp[64];
  size_t resp_len = 0;
  recv_msg(resp, sizeof(resp), &resp_len);
}

size_t TCPExecutorBackend::slot_word_size(size_t slot_id) const {
  // For TCP backend, we don't have local metadata. The executor has the size.
  // For now, return 0 — the fetch response includes the size.
  // A proper implementation would cache metadata locally.
  return 0;
}
