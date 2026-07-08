// Host fact probing. Best-effort and defensive: never returns cpu_count 0,
// so downstream range checks (E7) always have a sane bound even on exotic
// hosts.

#include "hypercore/config/host_info.hpp"

#include <fstream>
#include <limits>
#include <string>
#include <thread>

namespace hypercore::config {

namespace {

// Read MemTotal (in KiB) from /proc/meminfo. Returns 0 if unavailable.
std::uint64_t read_meminfo_total_bytes() {
  std::ifstream f("/proc/meminfo");
  if (!f) return 0;
  std::string key;
  std::uint64_t kib = 0;
  std::string unit;
  // Format: "MemTotal:       16327456 kB"
  while (f >> key) {
    if (key == "MemTotal:") {
      f >> kib >> unit;
      return kib * 1024ull;
    }
    f.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
  }
  return 0;
}

}  // namespace

HostInfo HostInfo::detect() {
  HostInfo h;
  unsigned hc = std::thread::hardware_concurrency();
  h.cpu_count = hc == 0 ? 1 : hc;  // 0 means "unknown"; assume at least 1
  h.ram_bytes = read_meminfo_total_bytes();
  return h;
}

}  // namespace hypercore::config
