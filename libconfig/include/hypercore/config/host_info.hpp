// Host facts that validation compares against.
//
// Injected rather than read from globals so tests can pin a synthetic host
// ("4 cores, 8 GiB") and get deterministic E7 (CPU out of range) and W2 (RAM
// overcommit) outcomes regardless of the machine running the suite.
#pragma once

#include <cstdint>

namespace hypercore::config {

struct HostInfo {
  unsigned cpu_count = 0;        // number of usable host cores (nproc)
  std::uint64_t ram_bytes = 0;   // total physical RAM in bytes

  // Probe the real host: hardware_concurrency() for cores, /proc/meminfo for
  // RAM. Defined in host_info.cpp. Falls back to conservative values if a
  // source is unavailable (never returns 0 cpu_count).
  static HostInfo detect();
};

}  // namespace hypercore::config
