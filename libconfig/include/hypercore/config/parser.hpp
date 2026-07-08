// Parser: TOML text -> typed Config.
//
// Responsibilities (and ONLY these):
//   - Turn TOML into the typed structs in types.hpp.
//   - Report TOML syntax errors and type mismatches (E1).
//   - Report unknown keys in any known table (E10) — strict by design.
//   - Convert enum-valued strings; an unknown token is E5.
//   - Convert memory strings; a malformed value is E8.
//   - Record which keys were declared, so the validator can tell "missing"
//     apart from "present but invalid".
//
// It does NOT check semantics that need cross-VM or host context (missing
// required fields, CPU ranges, RAM sums, name uniqueness). That is the
// validator's job, run as a separate pass. This separation is a deliberate
// requirement, not an accident.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "hypercore/config/diagnostics.hpp"
#include "hypercore/config/types.hpp"

namespace hypercore::config {

struct ParseResult {
  // Present whenever TOML was syntactically parseable — even if `diagnostics`
  // holds errors from later semantic-shape problems (unknown keys, bad enums).
  // Absent only when TOML itself failed to parse (hard E1). The validator can
  // still be run on a partial Config to surface additional issues in one go.
  std::optional<Config> config;
  Diagnostics diagnostics;

  bool ok() const { return config.has_value() && !diagnostics.has_errors(); }
};

// Parse from an in-memory string (used everywhere, and directly by tests).
// `source_name` is used only in diagnostic locations.
ParseResult parse_string(std::string_view toml,
                         std::string_view source_name = "<string>");

// Parse from a file path. A missing/unreadable file is reported as an E1-class
// diagnostic with no config.
ParseResult parse_file(const std::string& path);

}  // namespace hypercore::config
