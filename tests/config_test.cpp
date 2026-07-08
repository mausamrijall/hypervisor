// Unit tests for the config parser + validator.
//
// Covers the Phase 2 required cases and a few edges:
//   - valid config round-trips into typed structs
//   - missing required field  -> E2 (not a throw)
//   - malformed TOML syntax   -> E1, no config
//   - CPU pin overlap         -> W1 (warning, config still valid)
//   - RAM overcommit          -> W2 (warning, config still valid)
//   - empty config file       -> parses, W3 (no VMs), no errors
//   plus: unknown key (E10), bad enum (E5), bad memory (E8), CPU out of range
//   (E7), duplicate names, virtiofs-needs-share (E9), memory-unit parsing,
//   validate-returns-ALL-errors (not first-only).

#include "hctest.hpp"

#include "hypercore/config/host_info.hpp"
#include "hypercore/config/parser.hpp"
#include "hypercore/config/types.hpp"
#include "hypercore/config/validator.hpp"

using namespace hypercore::config;

namespace {

// A deterministic synthetic host: 4 cores, 8 GiB. Lets CPU-range (E7) and
// RAM-overcommit (W2) tests assert fixed outcomes regardless of CI hardware.
HostInfo synthetic_host() {
  HostInfo h;
  h.cpu_count = 4;
  h.ram_bytes = 8ull << 30;  // 8 GiB
  return h;
}

// Convenience: parse then validate against the synthetic host, merging both
// passes' diagnostics the way the daemon will.
Diagnostics run(std::string_view toml, std::optional<Config>* out = nullptr) {
  ParseResult pr = parse_string(toml);
  Diagnostics all = pr.diagnostics;
  if (pr.config) {
    all.append(validate(*pr.config, synthetic_host()));
    if (out) *out = pr.config;
  }
  return all;
}

}  // namespace

// --- memory-size unit parsing (pure) ----------------------------------------
TEST(memory_parsing_units) {
  CHECK_EQ(parse_memory("1024"), std::optional<std::uint64_t>(1024));
  CHECK_EQ(parse_memory("1K"), std::optional<std::uint64_t>(1024));
  CHECK_EQ(parse_memory("2M"), std::optional<std::uint64_t>(2ull << 20));
  CHECK_EQ(parse_memory("2G"), std::optional<std::uint64_t>(2ull << 30));
  CHECK_EQ(parse_memory("1T"), std::optional<std::uint64_t>(1ull << 40));
  CHECK_EQ(parse_memory("512m"), std::optional<std::uint64_t>(512ull << 20));
  // Malformed / edge:
  CHECK(!parse_memory("").has_value());
  CHECK(!parse_memory("G").has_value());
  CHECK(!parse_memory("2X").has_value());
  CHECK(!parse_memory("1.5G").has_value());
  CHECK(!parse_memory("0x10").has_value());
  // Overflow guard: 99999...T must not wrap.
  CHECK(!parse_memory("99999999999999999999T").has_value());
}

// --- required case 1: a fully valid config ----------------------------------
TEST(valid_config_parses_into_typed_structs) {
  const char* cfg = R"(
    [hypercore]
    socket = "/tmp/hc.sock"
    log_level = "debug"

    [vm.web]
    image = "/img/web.qcow2"
    disk_type = "qcow2"
    cpus = [0, 1]
    memory = "2G"
    network = "bridge"
    restart = "always"
    guest_agent = "/run/web.agent"

    [vm.build]
    image = "/img/build.raw"
    disk_type = "raw"
    cpus = [2, 3]
    memory = "4G"
    network = "virtiofs"

    [vm.build.share]
    tag = "src"
    host_path = "/home/dev"
    readonly = true
  )";
  std::optional<Config> cfgout;
  Diagnostics d = run(cfg, &cfgout);

  CHECK(!d.has_errors());
  CHECK(cfgout.has_value());
  if (!cfgout) return;

  CHECK_EQ(cfgout->global.socket, std::string("/tmp/hc.sock"));
  CHECK_EQ(cfgout->global.log_level, std::string("debug"));
  CHECK_EQ(cfgout->vms.size(), std::size_t(2));

  // Structs are strongly typed, not a KV map — assert typed members directly.
  // (Table iteration order is deterministic per key in toml++.)
  const VmConfig* web = nullptr;
  const VmConfig* build = nullptr;
  for (const auto& vm : cfgout->vms) {
    if (vm.name == "web") web = &vm;
    if (vm.name == "build") build = &vm;
  }
  CHECK(web != nullptr);
  CHECK(build != nullptr);
  if (web) {
    CHECK_EQ(web->image, std::string("/img/web.qcow2"));
    CHECK(web->disk_type == DiskType::Qcow2);
    CHECK(web->network == Network::Bridge);
    CHECK(web->restart == RestartPolicy::Always);
    CHECK_EQ(web->cpus.size(), std::size_t(2));
    CHECK_EQ(web->memory_bytes, std::uint64_t(2ull << 30));
    CHECK(web->guest_agent.has_value());
  }
  if (build) {
    CHECK(build->disk_type == DiskType::Raw);
    CHECK(build->network == Network::Virtiofs);
    CHECK(build->restart == RestartPolicy::OnFailure);  // defaulted
    CHECK(build->share.has_value());
    if (build->share) {
      CHECK_EQ(build->share->tag, std::string("src"));
      CHECK(build->share->readonly == true);
    }
  }
}

// --- required case 2: missing required field -> E2, not a throw -------------
TEST(missing_required_field_is_error_not_throw) {
  // 'image' omitted.
  const char* cfg = R"(
    [vm.web]
    disk_type = "qcow2"
    cpus = [0]
    memory = "1G"
    network = "user"
  )";
  Diagnostics d = run(cfg);
  CHECK(d.has_errors());
  CHECK(d.contains(Code::MissingRequired));  // E2
  // And nothing about the fields that WERE provided.
  CHECK(!d.contains(Code::BadEnum));
  CHECK(!d.contains(Code::BadMemory));
}

// --- required case 3: malformed TOML syntax -> E1, no config ----------------
TEST(malformed_toml_is_syntax_error) {
  const char* cfg = "[vm.web\nimage = ";  // unterminated table + dangling '='
  ParseResult pr = parse_string(cfg);
  CHECK(!pr.config.has_value());  // TOML itself unusable
  CHECK(pr.diagnostics.has_errors());
  CHECK(pr.diagnostics.contains(Code::SyntaxOrType));  // E1
}

// --- required case 4: CPU pin overlap between two VMs -> W1 (warning) -------
TEST(cpu_overlap_is_warning_not_error) {
  const char* cfg = R"(
    [vm.a]
    image = "/a"
    disk_type = "raw"
    cpus = [0, 1]
    memory = "1G"
    network = "user"

    [vm.b]
    image = "/b"
    disk_type = "raw"
    cpus = [1, 2]
    memory = "1G"
    network = "user"
  )";
  Diagnostics d = run(cfg);
  CHECK(d.contains(Code::CpuOverlap));  // W1
  CHECK(!d.has_errors());               // overlap alone must not reject
  CHECK_EQ(d.warning_count() >= 1, true);
}

// --- required case 5: RAM overcommit -> W2 (warning, not hard error) --------
TEST(ram_overcommit_is_warning) {
  // Host has 8 GiB; two 6 GiB guests sum to 12 GiB.
  const char* cfg = R"(
    [vm.a]
    image = "/a"
    disk_type = "raw"
    cpus = [0]
    memory = "6G"
    network = "user"

    [vm.b]
    image = "/b"
    disk_type = "raw"
    cpus = [1]
    memory = "6G"
    network = "user"
  )";
  Diagnostics d = run(cfg);
  CHECK(d.contains(Code::RamOvercommit));  // W2
  CHECK(!d.has_errors());                  // overcommit is allowed
}

// --- required case 6: empty config file -------------------------------------
TEST(empty_config_parses_with_no_vms_warning) {
  ParseResult pr = parse_string("");
  CHECK(pr.config.has_value());          // empty TOML is valid TOML
  CHECK(!pr.diagnostics.has_errors());   // parsing an empty doc is fine
  Diagnostics d = validate(*pr.config, synthetic_host());
  CHECK(d.contains(Code::NoVms));        // W3
  CHECK(!d.has_errors());                // idling is not an error
  // Globals fall back to documented defaults.
  CHECK_EQ(pr.config->global.socket, std::string("/run/hypercore.sock"));
  CHECK_EQ(pr.config->global.log_level, std::string("info"));
}

// --- extra: CPU index >= host core count -> E7 ------------------------------
TEST(cpu_out_of_range_is_error) {
  // Synthetic host has 4 cores (valid indices 0..3); pin to 4.
  const char* cfg = R"(
    [vm.a]
    image = "/a"
    disk_type = "raw"
    cpus = [0, 4]
    memory = "1G"
    network = "user"
  )";
  Diagnostics d = run(cfg);
  CHECK(d.contains(Code::CpuOutOfRange));  // E7
  CHECK(d.has_errors());
}

// --- extra: unknown key -> E10 (typo strictness) ----------------------------
TEST(unknown_key_is_error) {
  const char* cfg = R"(
    [vm.a]
    image = "/a"
    disk_type = "raw"
    cpus = [0]
    memory = "1G"
    network = "user"
    memry = "typo"
  )";
  Diagnostics d = run(cfg);
  CHECK(d.contains(Code::UnknownKey));  // E10
}

// --- extra: bad enum token -> E5 --------------------------------------------
TEST(bad_enum_value_is_error) {
  const char* cfg = R"(
    [vm.a]
    image = "/a"
    disk_type = "vmdk"
    cpus = [0]
    memory = "1G"
    network = "user"
  )";
  Diagnostics d = run(cfg);
  CHECK(d.contains(Code::BadEnum));  // E5
}

// --- extra: bad / zero memory -> E8 -----------------------------------------
TEST(bad_memory_value_is_error) {
  const char* cfg = R"(
    [vm.a]
    image = "/a"
    disk_type = "raw"
    cpus = [0]
    memory = "2X"
    network = "user"
  )";
  CHECK(run(cfg).contains(Code::BadMemory));  // E8

  const char* zero = R"(
    [vm.a]
    image = "/a"
    disk_type = "raw"
    cpus = [0]
    memory = "0"
    network = "user"
  )";
  CHECK(run(zero).contains(Code::BadMemory));  // zero rejected
}

// --- extra: virtiofs without a share -> E9 ----------------------------------
TEST(virtiofs_requires_share) {
  const char* cfg = R"(
    [vm.a]
    image = "/a"
    disk_type = "raw"
    cpus = [0]
    memory = "1G"
    network = "virtiofs"
  )";
  Diagnostics d = run(cfg);
  CHECK(d.contains(Code::MissingShare));  // E9
}

// --- extra: duplicate CPU indices within one VM -> E6 -----------------------
TEST(duplicate_cpu_in_one_vm_is_error) {
  const char* cfg = R"(
    [vm.a]
    image = "/a"
    disk_type = "raw"
    cpus = [1, 1]
    memory = "1G"
    network = "user"
  )";
  CHECK(run(cfg).contains(Code::BadCpuList));  // E6
}

// --- extra: validator returns ALL errors, not just the first ----------------
TEST(validator_accumulates_all_errors) {
  // Three independent problems in one VM: bad name (E3), out-of-range cpu (E7),
  // and a missing required field (network omitted -> E2).
  const char* cfg = R"(
    [vm.Bad_Name]
    image = "/a"
    disk_type = "raw"
    cpus = [9]
    memory = "1G"
  )";
  Diagnostics d = run(cfg);
  CHECK(d.contains(Code::BadName));        // E3
  CHECK(d.contains(Code::CpuOutOfRange));  // E7
  CHECK(d.contains(Code::MissingRequired));// E2 (network)
  CHECK(d.error_count() >= 3);             // proves no stop-at-first
}
