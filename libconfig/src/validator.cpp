// Semantic validation over a typed Config. See validator.hpp.
//
// Every check appends to `diags` and continues — the operator gets the full
// list, not just the first failure. Errors reject; warnings flag-and-accept.

#include "hypercore/config/validator.hpp"

#include <cctype>
#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace hypercore::config {

namespace {

bool valid_name(std::string_view n) {
  // ^[a-z0-9][a-z0-9-]*$
  if (n.empty()) return false;
  auto is_lower_alnum = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  };
  if (!is_lower_alnum(n.front())) return false;
  for (char c : n)
    if (!is_lower_alnum(c) && c != '-') return false;
  return true;
}

// E2: a required key was not declared at all. (Bad *values* for declared keys
// are the parser's job and are already reported; we must not double-report.)
void check_required(const VmConfig& vm, const std::string& where,
                    Diagnostics& diags) {
  auto require = [&](const char* key) {
    if (!vm.declared.count(key))
      diags.error(Code::MissingRequired, where,
                  std::string("missing required field '") + key + "'");
  };
  require("image");
  require("disk_type");
  require("cpus");
  require("memory");
  require("network");

  // Declared-but-empty image path is a distinct mistake from absent, but still
  // E2 (there is no usable image either way).
  if (vm.declared.count("image") && vm.image.empty())
    diags.error(Code::MissingRequired, where + ".image",
                "image path is empty");
}

// E6: cpus present but empty, negative, or duplicated.
// E7: a core index is >= host core count.
void check_cpus(const VmConfig& vm, const std::string& where,
                const HostInfo& host, Diagnostics& diags) {
  if (!vm.declared.count("cpus")) return;  // absence is E2, handled elsewhere
  if (vm.cpus.empty()) {
    diags.error(Code::BadCpuList, where + ".cpus",
                "cpu core list must not be empty");
    return;
  }
  std::set<std::int64_t> seen;
  for (std::int64_t core : vm.cpus) {
    if (core < 0) {
      diags.error(Code::BadCpuList, where + ".cpus",
                  "negative core index " + std::to_string(core));
      continue;
    }
    if (!seen.insert(core).second) {
      diags.error(Code::BadCpuList, where + ".cpus",
                  "duplicate core index " + std::to_string(core));
      continue;
    }
    if (static_cast<std::uint64_t>(core) >= host.cpu_count)
      diags.error(Code::CpuOutOfRange, where + ".cpus",
                  "core " + std::to_string(core) + " does not exist on host (" +
                      std::to_string(host.cpu_count) + " cores)");
  }
}

// E9: virtiofs requires a share with both tag and host_path.
void check_network_share(const VmConfig& vm, const std::string& where,
                         Diagnostics& diags) {
  if (vm.network != Network::Virtiofs) return;
  if (!vm.share) {
    diags.error(Code::MissingShare, where,
                "network = \"virtiofs\" requires a [" + where +
                    ".share] table");
    return;
  }
  if (vm.share->tag.empty())
    diags.error(Code::MissingShare, where + ".share",
                "share requires a non-empty 'tag'");
  if (vm.share->host_path.empty())
    diags.error(Code::MissingShare, where + ".share",
                "share requires a non-empty 'host_path'");
}

}  // namespace

Diagnostics validate(const Config& cfg, const HostInfo& host) {
  Diagnostics diags;

  // W3: nothing to run.
  if (cfg.vms.empty())
    diags.warn(Code::NoVms, "<config>",
               "no [vm.*] tables defined; the daemon will idle");

  std::set<std::string> names;                     // for E4
  std::map<std::int64_t, std::string> core_owner;  // for W1
  std::uint64_t total_mem = 0;                     // for W2

  for (const auto& vm : cfg.vms) {
    const std::string where = "vm." + vm.name;

    // E3 / E4: name shape and uniqueness.
    if (!valid_name(vm.name))
      diags.error(Code::BadName, where,
                  "guest name must match ^[a-z0-9][a-z0-9-]*$");
    if (!names.insert(vm.name).second)
      diags.error(Code::DuplicateName, where,
                  "duplicate guest name '" + vm.name + "'");

    check_required(vm, where, diags);
    check_cpus(vm, where, host, diags);
    check_network_share(vm, where, diags);

    // W4: restart policy without a guest agent to health-check against.
    if (vm.restart != RestartPolicy::Never && !vm.guest_agent)
      diags.warn(Code::AgentlessRestart, where,
                 "restart policy '" + std::string(to_string(vm.restart)) +
                     "' set but no guest_agent; health checks degrade to "
                     "process liveness only");

    // W1: cross-VM core overlap. Only meaningful for valid (non-negative,
    // in-range) cores; report the first VM that claimed each core.
    for (std::int64_t core : vm.cpus) {
      if (core < 0) continue;
      auto [it, inserted] = core_owner.try_emplace(core, vm.name);
      if (!inserted && it->second != vm.name)
        diags.warn(Code::CpuOverlap, where + ".cpus",
                   "core " + std::to_string(core) + " also pinned by '" +
                       it->second + "'");
    }

    total_mem += vm.memory_bytes;
  }

  // W2: RAM overcommit (only if we actually know host RAM).
  if (host.ram_bytes > 0 && total_mem > host.ram_bytes)
    diags.warn(Code::RamOvercommit, "<config>",
               "sum of guest memory (" + format_bytes(total_mem) +
                   ") exceeds host RAM (" + format_bytes(host.ram_bytes) + ")");

  return diags;
}

}  // namespace hypercore::config
