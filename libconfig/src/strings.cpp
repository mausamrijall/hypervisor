// String <-> enum conversions, memory-size parsing, and diagnostic code names.
// Kept together because they are the pure, dependency-free "vocabulary" of the
// config module, shared by the parser and validator.

#include <array>
#include <cctype>
#include <cstdint>
#include <limits>

#include "hypercore/config/diagnostics.hpp"
#include "hypercore/config/types.hpp"

namespace hypercore::config {

// --- Enum <-> string ---------------------------------------------------------

std::string_view to_string(DiskType v) {
  switch (v) {
    case DiskType::Raw: return "raw";
    case DiskType::Qcow2: return "qcow2";
  }
  return "?";
}
std::string_view to_string(Network v) {
  switch (v) {
    case Network::Bridge: return "bridge";
    case Network::User: return "user";
    case Network::Virtiofs: return "virtiofs";
  }
  return "?";
}
std::string_view to_string(RestartPolicy v) {
  switch (v) {
    case RestartPolicy::Never: return "never";
    case RestartPolicy::OnFailure: return "on-failure";
    case RestartPolicy::Always: return "always";
  }
  return "?";
}

std::optional<DiskType> parse_disk_type(std::string_view s) {
  if (s == "raw") return DiskType::Raw;
  if (s == "qcow2") return DiskType::Qcow2;
  return std::nullopt;
}
std::optional<Network> parse_network(std::string_view s) {
  if (s == "bridge") return Network::Bridge;
  if (s == "user") return Network::User;
  if (s == "virtiofs") return Network::Virtiofs;
  return std::nullopt;
}
std::optional<RestartPolicy> parse_restart_policy(std::string_view s) {
  if (s == "never") return RestartPolicy::Never;
  if (s == "on-failure") return RestartPolicy::OnFailure;
  if (s == "always") return RestartPolicy::Always;
  return std::nullopt;
}

// --- Memory sizes ------------------------------------------------------------
// Grammar: ^[0-9]+[KMGT]?$  (binary units). Overflow-safe.

std::optional<std::uint64_t> parse_memory(std::string_view s) {
  if (s.empty()) return std::nullopt;

  // Split trailing unit suffix (single char, case-insensitive).
  std::uint64_t multiplier = 1;
  char last = s.back();
  if (!std::isdigit(static_cast<unsigned char>(last))) {
    switch (std::toupper(static_cast<unsigned char>(last))) {
      case 'K': multiplier = 1ull << 10; break;
      case 'M': multiplier = 1ull << 20; break;
      case 'G': multiplier = 1ull << 30; break;
      case 'T': multiplier = 1ull << 40; break;
      default: return std::nullopt;  // unknown suffix
    }
    s.remove_suffix(1);
    if (s.empty()) return std::nullopt;  // suffix with no number
  }

  // Parse the (now suffix-free) digit run, checking for overflow at each step.
  std::uint64_t value = 0;
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (kMax - digit) / 10) return std::nullopt;  // would overflow
    value = value * 10 + digit;
  }
  if (multiplier != 1 && value > kMax / multiplier) return std::nullopt;
  return value * multiplier;
}

std::string format_bytes(std::uint64_t bytes) {
  // Pick the largest binary unit that divides evenly; otherwise show bytes.
  struct Unit { const char* suffix; std::uint64_t scale; };
  constexpr std::array<Unit, 4> units{{
      {"T", 1ull << 40}, {"G", 1ull << 30},
      {"M", 1ull << 20}, {"K", 1ull << 10}}};
  for (const auto& u : units) {
    if (bytes >= u.scale && bytes % u.scale == 0)
      return std::to_string(bytes / u.scale) + u.suffix;
  }
  return std::to_string(bytes) + "B";
}

// --- Diagnostic code names ---------------------------------------------------

std::string_view code_str(Code c) {
  switch (c) {
    case Code::SyntaxOrType: return "E1";
    case Code::MissingRequired: return "E2";
    case Code::BadName: return "E3";
    case Code::DuplicateName: return "E4";
    case Code::BadEnum: return "E5";
    case Code::BadCpuList: return "E6";
    case Code::CpuOutOfRange: return "E7";
    case Code::BadMemory: return "E8";
    case Code::MissingShare: return "E9";
    case Code::UnknownKey: return "E10";
    case Code::CpuOverlap: return "W1";
    case Code::RamOvercommit: return "W2";
    case Code::NoVms: return "W3";
    case Code::AgentlessRestart: return "W4";
  }
  return "?";
}

}  // namespace hypercore::config
