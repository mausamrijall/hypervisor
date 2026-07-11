// Sequential SSH port allocator.
//
// Replaces the FNV-hash-based static port assignment with a persistent
// allocation map stored at <runtime_dir>/ports.json. Ports are assigned
// sequentially from the range [PORT_MIN, PORT_MAX] and released when a VM is
// stopped or destroyed. This eliminates hash collisions between guests sharing
// the same port range.
//
// The allocation file is a simple JSON object mapping VM name -> port number:
//   { "web": 20000, "db": 20001 }
//
// Thread safety: callers must serialize (the supervisor already holds the
// single-threaded reconcile + health tick model; this needs no extra locking).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace hypercore::core {

constexpr int PORT_MIN = 20000;
constexpr int PORT_MAX = 29999;

// Acquire a port for `vm_name`. If the VM already has an entry in the map
// its existing port is returned (idempotent across daemon restarts/adoption).
// Returns the assigned port, or 0 on failure (full range, I/O error).
int portmap_acquire(const std::string& runtime_dir, const std::string& vm_name);

// Release the port held by `vm_name`. No-op if the VM has no entry.
void portmap_release(const std::string& runtime_dir, const std::string& vm_name);

// Look up the port assigned to `vm_name` without modifying the map.
// Returns 0 if no port is assigned.
int portmap_lookup(const std::string& runtime_dir, const std::string& vm_name);

}  // namespace hypercore::core
