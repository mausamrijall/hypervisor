// Strongly-typed representation of hypervisor.cfg.
//
// This is deliberately NOT a generic key/value map: every field a guest can
// have is a named, typed member. The parser (parser.hpp) fills these in from
// TOML; the validator (validator.hpp) checks them for semantic sanity. See
// docs/schema.md for the normative field list.
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace hypercore::config {

// --- Enumerated fields -------------------------------------------------------
// Kept as enums (not strings) so the rest of the daemon can switch over them
// exhaustively. Parsing string -> enum happens in the parser; an unrecognized
// value is reported as a diagnostic rather than silently mapped.

enum class DiskType { Raw, Qcow2 };
enum class Network { Bridge, User, Virtiofs };
enum class RestartPolicy { Never, OnFailure, Always };

// String <-> enum helpers (defined in parser.cpp). to_string is total; the
// parse_* variants return nullopt on an unknown token.
std::string_view to_string(DiskType);
std::string_view to_string(Network);
std::string_view to_string(RestartPolicy);
std::optional<DiskType> parse_disk_type(std::string_view);
std::optional<Network> parse_network(std::string_view);
std::optional<RestartPolicy> parse_restart_policy(std::string_view);

// Parse a memory size like "2G", "512M", "1048576" (bytes if no suffix).
// Suffixes are binary (K=1024, M=1024^2, G=1024^3, T=1024^4). Returns nullopt
// on malformed input or overflow. Zero is parsed successfully as 0 (the
// validator rejects a zero allocation, since "0" vs "absent" are different
// mistakes and get different diagnostics).
std::optional<std::uint64_t> parse_memory(std::string_view);

// Render bytes back to a human string (for diagnostics/logging).
std::string format_bytes(std::uint64_t bytes);

// --- Config structs ----------------------------------------------------------

struct ShareConfig {
  std::string tag;
  std::string host_path;
  bool readonly = false;
};

struct VmConfig {
  std::string name;  // the <name> in [vm.<name>]

  // Required fields. They have no natural "unset" value, so presence is tracked
  // separately in `declared` (below) and enforced by the validator — that's how
  // "missing required field" stays a validation concern, not a parse throw.
  std::string image;
  std::optional<DiskType> disk_type;
  std::vector<std::int64_t> cpus;  // raw host core indices, range-checked later
  std::uint64_t memory_bytes = 0;
  std::optional<Network> network;

  // Optional fields with defaults.
  RestartPolicy restart = RestartPolicy::OnFailure;
  std::optional<std::string> guest_agent;
  std::optional<ShareConfig> share;

  // Parse provenance: the set of keys that actually appeared in this VM's TOML
  // table. The validator uses it to distinguish "field missing" (E2) from
  // "field present but semantically invalid" (E5/E6/E8), so the two never
  // double-report. Not part of the guest's runtime identity.
  std::set<std::string> declared;
};

struct GlobalConfig {
  std::string socket = "/run/hypercore.sock";
  std::string state_dir = "/var/lib/hypercore";
  std::string log_level = "info";
  std::string log_format = "logfmt";
};

struct Config {
  GlobalConfig global;
  std::vector<VmConfig> vms;
};

}  // namespace hypercore::config
