/*
 * Copyright (c) 2026, LIBAPTH Research. All rights reserved.
 */

#include "precompiled.hpp"
#include "gc/g1/g1RemoteMemoryManager.hpp"
#include "gc/g1/g1CollectedHeap.hpp"

G1RemoteMemoryManager::G1RemoteMemoryManager(G1CollectedHeap* g1h)
  : _g1h(g1h), _handle_allocator(), _table_lock(0) {
  memset(_table, 0, sizeof(_table));
}

G1RemoteMemoryManager::~G1RemoteMemoryManager() {
  // Free all HandleEntry objects in the table
  for (size_t i = 0; i < TABLE_SIZE; i++) {
    HandleEntry* e = _table[i];
    while (e != nullptr) {
      HandleEntry* next = e->_next;
      delete e;
      e = next;
    }
    _table[i] = nullptr;
  }
}
