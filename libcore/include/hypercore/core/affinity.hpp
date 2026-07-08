// CPU pinning with verification (requirement #3b).
//
// We do NOT trust that sched_setaffinity succeeded just because the syscall
// returned 0: we read /proc/<pid>/status back and parse Cpus_allowed_list,
// then confirm it equals the set we asked for. The daemon reports
// cpus_verified based on this readback, not on the syscall return.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace hypercore::core {

struct AffinityResult {
  bool applied = false;    // sched_setaffinity returned success
  bool verified = false;   // readback matched the requested set
  std::set<int> requested;
  std::set<int> actual;    // what /proc/<pid>/status reported
  std::string error;       // populated on failure
};

// Pin `pid` to exactly `cores`, then read back and verify.
AffinityResult pin_and_verify(int pid, const std::vector<std::int64_t>& cores);

// Parse a Linux "Cpus_allowed_list" style range string, e.g. "1,3-5,7" into a
// set {1,3,4,5,7}. Exposed for unit testing (host-independent).
std::set<int> parse_cpu_list(const std::string& list);

// Read the current Cpus_allowed_list for a pid from /proc. Empty set on error.
std::set<int> read_proc_cpus_allowed(int pid, std::string& err);

}  // namespace hypercore::core
