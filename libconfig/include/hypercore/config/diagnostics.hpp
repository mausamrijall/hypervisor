// Diagnostics — the common currency of parsing and validation.
//
// Both passes accumulate a list of these instead of throwing on the first
// problem, so an operator fixing a config sees every issue at once. Each
// diagnostic carries a stable code (see docs/schema.md: E1-E10, W1-W4) so
// tests can assert on codes rather than brittle message text.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hypercore::config {

enum class Severity { Error, Warning };

// Stable diagnostic codes. Values match docs/schema.md exactly.
enum class Code {
  // Errors
  SyntaxOrType,      // E1: invalid TOML syntax or wrong value type
  MissingRequired,   // E2: required per-VM field absent
  BadName,           // E3: guest name fails ^[a-z0-9][a-z0-9-]*$
  DuplicateName,     // E4: duplicate guest name
  BadEnum,           // E5: disk_type/network/restart out of range
  BadCpuList,        // E6: cpus empty / negative / duplicated
  CpuOutOfRange,     // E7: pinned core index >= host core count
  BadMemory,         // E8: memory unparseable or zero
  MissingShare,      // E9: virtiofs without a valid share table
  UnknownKey,        // E10: unrecognized key in a table

  // Warnings
  CpuOverlap,        // W1: two VMs pin the same host core
  RamOvercommit,     // W2: sum of VM memory exceeds host physical RAM
  NoVms,             // W3: no [vm.*] tables at all
  AgentlessRestart,  // W4: restart policy set but no guest_agent
};

std::string_view code_str(Code);

struct Diagnostic {
  Severity severity;
  Code code;
  std::string where;    // e.g. "vm.web" or "vm.web.cpus" or "<toml>"
  std::string message;  // human-readable detail
};

// A collection with convenience predicates. Ordered as inserted.
class Diagnostics {
 public:
  void error(Code code, std::string where, std::string message) {
    items_.push_back({Severity::Error, code, std::move(where),
                      std::move(message)});
  }
  void warn(Code code, std::string where, std::string message) {
    items_.push_back({Severity::Warning, code, std::move(where),
                      std::move(message)});
  }

  const std::vector<Diagnostic>& items() const { return items_; }
  bool empty() const { return items_.empty(); }

  bool has_errors() const {
    for (const auto& d : items_)
      if (d.severity == Severity::Error) return true;
    return false;
  }

  std::size_t error_count() const {
    std::size_t n = 0;
    for (const auto& d : items_)
      if (d.severity == Severity::Error) ++n;
    return n;
  }
  std::size_t warning_count() const {
    return items_.size() - error_count();
  }

  // True if any diagnostic with the given code is present (test convenience).
  bool contains(Code code) const {
    for (const auto& d : items_)
      if (d.code == code) return true;
    return false;
  }

  void append(const Diagnostics& other) {
    items_.insert(items_.end(), other.items_.begin(), other.items_.end());
  }

 private:
  std::vector<Diagnostic> items_;
};

}  // namespace hypercore::config
