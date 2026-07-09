#include "hypercore/core/proc_stats.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace hypercore::core {

namespace {

// Sum of all CPU jiffies from /proc/stat's first "cpu" line. 0 on error.
std::uint64_t read_total_jiffies() {
  std::ifstream f("/proc/stat");
  if (!f) return 0;
  std::string cpu;
  f >> cpu;  // "cpu"
  if (cpu != "cpu") return 0;
  std::uint64_t total = 0, v = 0;
  while (f >> v) total += v;
  return total;
}

// utime (14th) + stime (15th) from /proc/<pid>/stat. The comm field (2nd) may
// contain spaces/parens, so we scan past the final ')'.
bool read_proc_jiffies(int pid, std::uint64_t& out) {
  std::ostringstream p;
  p << "/proc/" << pid << "/stat";
  std::ifstream f(p.str());
  if (!f) return false;
  std::string data((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
  auto rp = data.rfind(')');
  if (rp == std::string::npos) return false;
  std::istringstream rest(data.substr(rp + 1));
  // After ')': fields 3..; utime is field 14 => index 11 past comm, stime 15.
  std::vector<std::string> f2;
  std::string tok;
  while (rest >> tok) f2.push_back(tok);
  // f2[0] is state (field 3). utime=field14 -> f2[11], stime=field15 -> f2[12].
  if (f2.size() < 13) return false;
  try {
    out = std::stoull(f2[11]) + std::stoull(f2[12]);
  } catch (...) {
    return false;
  }
  return true;
}

std::uint64_t read_vmrss_bytes(int pid) {
  std::ostringstream p;
  p << "/proc/" << pid << "/status";
  std::ifstream f(p.str());
  if (!f) return 0;
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("VmRSS:", 0) == 0) {
      std::istringstream ls(line.substr(6));
      std::uint64_t kib = 0;
      ls >> kib;
      return kib * 1024ull;
    }
  }
  return 0;
}

}  // namespace

bool ProcSampler::sample(int pid) {
  if (pid <= 0) return false;
  std::uint64_t proc_j = 0;
  if (!read_proc_jiffies(pid, proc_j)) return false;
  std::uint64_t total_j = read_total_jiffies();
  rss_bytes_ = read_vmrss_bytes(pid);

  if (have_prev_ && total_j > prev_total_jiffies_) {
    std::uint64_t dproc = proc_j - prev_proc_jiffies_;
    std::uint64_t dtotal = total_j - prev_total_jiffies_;
    // Percentage of total host CPU capacity across all cores. Multiply by ncpu
    // so a fully-busy single vCPU on an N-core host reads ~100%/N * N... we
    // report share of ONE core (so a busy 1-vcpu guest ~= 100%).
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;
    cpu_percent_ =
        dtotal > 0 ? (100.0 * static_cast<double>(dproc) /
                      static_cast<double>(dtotal)) * static_cast<double>(ncpu)
                   : 0.0;
  } else {
    cpu_percent_ = 0.0;
  }
  prev_proc_jiffies_ = proc_j;
  prev_total_jiffies_ = total_j;
  have_prev_ = true;
  return true;
}

}  // namespace hypercore::core
