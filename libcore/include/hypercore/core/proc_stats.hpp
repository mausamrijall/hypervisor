// Host-side per-process resource sampling for the dashboard (Phase 4).
//
// We report the QEMU *process's* host resource use — CPU% derived from
// /proc/<pid>/stat utime+stime deltas between samples, and resident memory
// from /proc/<pid>/status VmRSS. This needs no guest cooperation and reflects
// what the VM actually costs the host, which is the number an operator wants
// when deciding placement.
#pragma once

#include <cstdint>

namespace hypercore::core {

// Rolling CPU sampler for one process. Call sample() periodically; cpu_percent
// reflects usage since the previous sample.
class ProcSampler {
 public:
  // Update from /proc/<pid>/stat and /proc/<pid>/status. Returns false if the
  // process could not be read (e.g. gone). On the first successful call the
  // CPU% is 0 (no prior baseline) but rss is valid immediately.
  bool sample(int pid);

  double cpu_percent() const { return cpu_percent_; }
  std::uint64_t rss_bytes() const { return rss_bytes_; }
  void reset() { *this = ProcSampler{}; }

 private:
  bool have_prev_ = false;
  std::uint64_t prev_proc_jiffies_ = 0;  // utime+stime
  std::uint64_t prev_total_jiffies_ = 0; // whole-system CPU time
  double cpu_percent_ = 0.0;
  std::uint64_t rss_bytes_ = 0;
};

}  // namespace hypercore::core
