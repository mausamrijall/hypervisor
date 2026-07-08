// TOML -> typed Config. See parser.hpp for the responsibility boundary.
//
// Division of labour (deliberate, per the Phase 2 brief):
//   parser    reports malformed *values* — E1 (syntax/type), E5 (bad enum
//             token), E8 (bad/zero memory), E10 (unknown key).
//   validator reports incoherent *sets of values* — E2, E3, E4, E6, E7, E9,
//             and all warnings.
// The two never double-report the same field: the parser records which keys
// were `declared`, and the validator keys its "missing required" logic off
// that set rather than off default-constructed values.

#include "hypercore/config/parser.hpp"

#include <fstream>
#include <sstream>
#include <string>

#include "toml.hpp"

namespace hypercore::config {

namespace {

// Format a toml++ source position as "line:col" for diagnostic locations.
std::string at(const toml::source_region& src) {
  std::ostringstream os;
  os << src.begin.line << ":" << src.begin.column;
  return os.str();
}

// Allowed key sets. An unknown key in any table is E10 (strict-by-design so a
// typo like `memry` is caught, not silently ignored).
bool is_global_key(std::string_view k) {
  return k == "socket" || k == "state_dir" || k == "log_level" ||
         k == "log_format";
}
bool is_vm_key(std::string_view k) {
  return k == "image" || k == "disk_type" || k == "cpus" || k == "memory" ||
         k == "network" || k == "restart" || k == "guest_agent" ||
         k == "share";
}
bool is_share_key(std::string_view k) {
  return k == "tag" || k == "host_path" || k == "readonly";
}

// Pull a string value; on a present-but-wrong-type node, emit E1 and return
// nullopt so the caller leaves the field at its default.
std::optional<std::string> want_string(const toml::node& n, std::string_view where,
                                        std::string_view key, Diagnostics& diags) {
  if (auto v = n.value<std::string>()) return *v;
  diags.error(Code::SyntaxOrType, std::string(where) + "." + std::string(key),
              "expected a string");
  return std::nullopt;
}

void parse_share(const toml::table& tbl, std::string_view where,
                 VmConfig& vm, Diagnostics& diags) {
  ShareConfig share;
  for (auto&& [key, node] : tbl) {
    std::string_view k = key.str();
    if (!is_share_key(k)) {
      diags.error(Code::UnknownKey, std::string(where) + "." + std::string(k),
                  "unknown key in share table");
      continue;
    }
    if (k == "tag") {
      if (auto v = want_string(node, where, k, diags)) share.tag = *v;
    } else if (k == "host_path") {
      if (auto v = want_string(node, where, k, diags)) share.host_path = *v;
    } else if (k == "readonly") {
      if (auto v = node.value<bool>()) {
        share.readonly = *v;
      } else {
        diags.error(Code::SyntaxOrType, std::string(where) + ".readonly",
                    "expected a boolean");
      }
    }
  }
  vm.share = std::move(share);
}

void parse_vm(std::string name, const toml::table& tbl, VmConfig& vm,
              Diagnostics& diags) {
  vm.name = std::move(name);
  const std::string where = "vm." + vm.name;

  for (auto&& [key, node] : tbl) {
    std::string_view k = key.str();
    if (!is_vm_key(k)) {
      diags.error(Code::UnknownKey, where + "." + std::string(k),
                  "unknown key in vm table");
      continue;
    }
    vm.declared.insert(std::string(k));

    if (k == "image") {
      if (auto v = want_string(node, where, k, diags)) vm.image = *v;
    } else if (k == "disk_type") {
      if (auto v = want_string(node, where, k, diags)) {
        if (auto d = parse_disk_type(*v)) vm.disk_type = *d;
        else diags.error(Code::BadEnum, where + ".disk_type",
                         "must be one of: raw, qcow2 (got \"" + *v + "\")");
      }
    } else if (k == "network") {
      if (auto v = want_string(node, where, k, diags)) {
        if (auto nw = parse_network(*v)) vm.network = *nw;
        else diags.error(Code::BadEnum, where + ".network",
                         "must be one of: bridge, user, virtiofs (got \"" +
                             *v + "\")");
      }
    } else if (k == "restart") {
      if (auto v = want_string(node, where, k, diags)) {
        if (auto r = parse_restart_policy(*v)) vm.restart = *r;
        else diags.error(Code::BadEnum, where + ".restart",
                         "must be one of: never, on-failure, always (got \"" +
                             *v + "\")");
      }
    } else if (k == "guest_agent") {
      if (auto v = want_string(node, where, k, diags)) vm.guest_agent = *v;
    } else if (k == "memory") {
      if (auto v = want_string(node, where, k, diags)) {
        auto bytes = parse_memory(*v);
        if (!bytes || *bytes == 0)
          diags.error(Code::BadMemory, where + ".memory",
                      "invalid memory size \"" + *v +
                          "\" (want e.g. 512M, 2G; must be > 0)");
        else
          vm.memory_bytes = *bytes;
      }
    } else if (k == "cpus") {
      const toml::array* arr = node.as_array();
      if (!arr) {
        diags.error(Code::SyntaxOrType, where + ".cpus",
                    "expected an array of integers");
      } else {
        for (const toml::node& elem : *arr) {
          if (auto iv = elem.value<std::int64_t>()) {
            vm.cpus.push_back(*iv);
          } else {
            diags.error(Code::SyntaxOrType, where + ".cpus",
                        "cpu core list must contain integers only");
          }
        }
      }
    } else if (k == "share") {
      const toml::table* stbl = node.as_table();
      if (!stbl)
        diags.error(Code::SyntaxOrType, where + ".share",
                    "expected a [vm." + vm.name + ".share] table");
      else
        parse_share(*stbl, where + ".share", vm, diags);
    }
  }
}

void parse_global(const toml::table& tbl, GlobalConfig& g, Diagnostics& diags) {
  for (auto&& [key, node] : tbl) {
    std::string_view k = key.str();
    if (!is_global_key(k)) {
      diags.error(Code::UnknownKey, std::string("hypercore.") + std::string(k),
                  "unknown key in [hypercore] table");
      continue;
    }
    auto v = want_string(node, "hypercore", k, diags);
    if (!v) continue;
    if (k == "socket") g.socket = *v;
    else if (k == "state_dir") g.state_dir = *v;
    else if (k == "log_level") g.log_level = *v;
    else if (k == "log_format") g.log_format = *v;
  }
}

Config build_config(const toml::table& root, Diagnostics& diags) {
  Config cfg;
  for (auto&& [key, node] : root) {
    std::string_view k = key.str();
    if (k == "hypercore") {
      if (const toml::table* t = node.as_table())
        parse_global(*t, cfg.global, diags);
      else
        diags.error(Code::SyntaxOrType, "hypercore",
                    "[hypercore] must be a table");
    } else if (k == "vm") {
      const toml::table* vms = node.as_table();
      if (!vms) {
        diags.error(Code::SyntaxOrType, "vm", "[vm] must be a table of guests");
        continue;
      }
      for (auto&& [vm_name, vm_node] : *vms) {
        const toml::table* vt = vm_node.as_table();
        if (!vt) {
          diags.error(Code::SyntaxOrType,
                      std::string("vm.") + std::string(vm_name.str()),
                      "each [vm.<name>] must be a table");
          continue;
        }
        VmConfig vm;
        parse_vm(std::string(vm_name.str()), *vt, vm, diags);
        cfg.vms.push_back(std::move(vm));
      }
    } else {
      diags.error(Code::UnknownKey, std::string(k),
                  "unknown top-level key (expected [hypercore] or [vm.*])");
    }
  }
  return cfg;
}

}  // namespace

ParseResult parse_string(std::string_view toml_text, std::string_view source_name) {
  ParseResult result;
  toml::table root;
  try {
    root = toml::parse(toml_text, source_name);
  } catch (const toml::parse_error& err) {
    result.diagnostics.error(Code::SyntaxOrType, at(err.source()),
                             std::string("TOML syntax error: ") +
                                 std::string(err.description()));
    return result;  // no config: TOML itself is unusable
  }
  result.config = build_config(root, result.diagnostics);
  return result;
}

ParseResult parse_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    ParseResult result;
    result.diagnostics.error(Code::SyntaxOrType, path,
                             "cannot open config file");
    return result;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return parse_string(ss.str(), path);
}

}  // namespace hypercore::config
