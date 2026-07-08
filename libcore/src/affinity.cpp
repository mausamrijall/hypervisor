#include "hypercore/core/affinity.hpp"

#include <sched.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace hypercore::core {

std::set<int> parse_cpu_list(const std::string& list) {
  // Grammar: comma-separated tokens, each either "N" or "N-M".
  std::set<int> out;
  std::stringstream ss(list);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    if (tok.empty()) continue;
    auto dash = tok.find('-');
    try {
      if (dash == std::string::npos) {
        out.insert(std::stoi(tok));
      } else {
        int lo = std::stoi(tok.substr(0, dash));
        int hi = std::stoi(tok.substr(dash + 1));
        for (int i = lo; i <= hi; ++i) out.insert(i);
      }
    } catch (...) {
      // Ignore an unparseable token; the caller compares sets and will treat a
      // partial/empty readback as "not verified" rather than crash.
    }
  }
  return out;
}

std::set<int> read_proc_cpus_allowed(int pid, std::string& err) {
  std::ostringstream path;
  path << "/proc/" << pid << "/status";
  std::ifstream f(path.str());
  if (!f) {
    err = "cannot open " + path.str();
    return {};
  }
  std::string line;
  while (std::getline(f, line)) {
    // Prefer the human-readable list form the kernel provides.
    constexpr const char* kKey = "Cpus_allowed_list:";
    if (line.rfind(kKey, 0) == 0) {
      std::string rest = line.substr(std::strlen(kKey));
      // Trim leading whitespace.
      std::size_t start = rest.find_first_not_of(" \t");
      if (start != std::string::npos) rest = rest.substr(start);
      return parse_cpu_list(rest);
    }
  }
  err = "Cpus_allowed_list not found in " + path.str();
  return {};
}

AffinityResult pin_and_verify(int pid, const std::vector<std::int64_t>& cores) {
  AffinityResult r;
  for (std::int64_t c : cores)
    if (c >= 0) r.requested.insert(static_cast<int>(c));

  cpu_set_t set;
  CPU_ZERO(&set);
  for (int c : r.requested) CPU_SET(c, &set);

  if (sched_setaffinity(pid, sizeof(set), &set) != 0) {
    r.error = std::string("sched_setaffinity: ") + std::strerror(errno);
    return r;
  }
  r.applied = true;

  // The crucial part: read it back from the kernel and compare.
  std::string err;
  r.actual = read_proc_cpus_allowed(pid, err);
  if (!err.empty()) {
    r.error = err;
    return r;
  }
  r.verified = (r.actual == r.requested);
  if (!r.verified)
    r.error = "readback mismatch: requested != actual";
  return r;
}

}  // namespace hypercore::core
