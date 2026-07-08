#include "hypercore/core/reconcile.hpp"

#include <algorithm>
#include <set>
#include <sstream>

namespace hypercore::core {

std::string fingerprint(const config::VmConfig& vm) {
  // Include exactly the fields that require a process restart when changed:
  // image, disk type, cpu set, memory, network, and share. Restart *policy*
  // and guest_agent path are runtime concerns, not launch identity, so they
  // are intentionally excluded (changing them doesn't require a reboot).
  std::ostringstream o;
  o << "img=" << vm.image << ';';
  o << "disk=" << (vm.disk_type ? config::to_string(*vm.disk_type) : "?") << ';';
  o << "cpus=";
  {
    std::vector<std::int64_t> c = vm.cpus;
    std::sort(c.begin(), c.end());
    for (auto v : c) o << v << ',';
  }
  o << ';';
  o << "mem=" << vm.memory_bytes << ';';
  o << "net=" << (vm.network ? config::to_string(*vm.network) : "?") << ';';
  if (vm.share)
    o << "share=" << vm.share->tag << ':' << vm.share->host_path << ':'
      << (vm.share->readonly ? "ro" : "rw") << ';';
  return o.str();
}

ReconcilePlan reconcile(const std::vector<config::VmConfig>& desired,
                        const std::map<std::string, ActualState>& actual) {
  ReconcilePlan plan;

  std::set<std::string> desired_names;
  for (const auto& vm : desired) {
    desired_names.insert(vm.name);
    auto it = actual.find(vm.name);
    const bool running = it != actual.end() && it->second.running;
    if (!running) {
      plan.to_start.push_back(vm.name);
      continue;
    }
    // Running: restart only if the launch fingerprint changed. If we have no
    // recorded fingerprint (e.g. adopted process), treat as unchanged rather
    // than forcing a disruptive restart on every reconcile.
    const std::string& have = it->second.fingerprint;
    if (!have.empty() && have != fingerprint(vm))
      plan.to_restart.push_back(vm.name);
    else
      plan.unchanged.push_back(vm.name);
  }

  // Anything actually running that is no longer desired should stop.
  for (const auto& [name, st] : actual) {
    if (st.running && desired_names.find(name) == desired_names.end())
      plan.to_stop.push_back(name);
  }

  // Deterministic ordering for stable output/tests.
  std::sort(plan.to_start.begin(), plan.to_start.end());
  std::sort(plan.to_stop.begin(), plan.to_stop.end());
  std::sort(plan.to_restart.begin(), plan.to_restart.end());
  std::sort(plan.unchanged.begin(), plan.unchanged.end());
  return plan;
}

}  // namespace hypercore::core
