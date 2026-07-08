// hypercored — the hypercore hypervisor daemon.
//
// Phase 1: this is a runnable skeleton. It handles global CLI flags, sets up
// structured logging, and exits cleanly. It does NOT yet parse a config,
// reconcile VM state, or open the control socket — those arrive in Phases 2-3
// and slot in where marked below. Keeping it runnable now means every later
// change is an incremental diff against a working binary.

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "hypercore/config/parser.hpp"
#include "hypercore/config/validator.hpp"
#include "hypercore/log.hpp"
#include "hypercore/version.hpp"

namespace {

namespace log = hypercore::log;

constexpr const char* kDefaultConfig = "/etc/hypercore/hypervisor.cfg";
constexpr const char* kDefaultSocket = "/run/hypercore.sock";

struct Options {
  std::string config_path = kDefaultConfig;
  std::string socket_path = kDefaultSocket;
  log::Level level = log::Level::Info;
  log::Format format = log::Format::Logfmt;
  bool dry_run = false;  // parse + reconcile-plan only, launch nothing
};

void print_usage(const char* argv0) {
  std::printf(
      "hypercored %s — hypercore hypervisor daemon\n"
      "\n"
      "Usage: %s [options]\n"
      "\n"
      "Options:\n"
      "  -c, --config PATH   Config file (default: %s)\n"
      "  -s, --socket PATH   Control socket path (default: %s)\n"
      "      --log-level L   trace|debug|info|warn|error (default: info)\n"
      "      --log-format F  logfmt|json (default: logfmt)\n"
      "      --dry-run       Parse config and print the reconcile plan, then\n"
      "                      exit without launching any guests\n"
      "  -v, --version       Print version and exit\n"
      "  -h, --help          Print this help and exit\n",
      HYPERCORE_VERSION_STRING, argv0, kDefaultConfig, kDefaultSocket);
}

// Minimal, dependency-free flag parsing. Returns false if the caller should
// exit; `exit_code` carries the status and `handled` marks help/version so we
// exit 0 rather than falling through to run().
bool parse_args(int argc, char** argv, Options& opt, int& exit_code,
                bool& handled) {
  auto needs_value = [&](int& i, std::string_view flag) -> const char* {
    if (i + 1 >= argc) {
      log::error("missing value for flag", {log::field("flag", flag)});
      exit_code = 2;
      return nullptr;
    }
    return argv[++i];
  };

  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    if (a == "-h" || a == "--help") {
      print_usage(argv[0]);
      handled = true;
      exit_code = 0;
      return false;
    }
    if (a == "-v" || a == "--version") {
      std::printf("%s\n", HYPERCORE_VERSION_STRING);
      handled = true;
      exit_code = 0;
      return false;
    }
    if (a == "-c" || a == "--config") {
      const char* v = needs_value(i, a);
      if (!v) return false;
      opt.config_path = v;
    } else if (a == "-s" || a == "--socket") {
      const char* v = needs_value(i, a);
      if (!v) return false;
      opt.socket_path = v;
    } else if (a == "--log-level") {
      const char* v = needs_value(i, a);
      if (!v) return false;
      std::string_view lv = v;
      if (lv == "trace") opt.level = log::Level::Trace;
      else if (lv == "debug") opt.level = log::Level::Debug;
      else if (lv == "info") opt.level = log::Level::Info;
      else if (lv == "warn") opt.level = log::Level::Warn;
      else if (lv == "error") opt.level = log::Level::Error;
      else {
        log::error("invalid log level", {log::field("value", lv)});
        exit_code = 2;
        return false;
      }
    } else if (a == "--log-format") {
      const char* v = needs_value(i, a);
      if (!v) return false;
      std::string_view fv = v;
      if (fv == "logfmt") opt.format = log::Format::Logfmt;
      else if (fv == "json") opt.format = log::Format::Json;
      else {
        log::error("invalid log format", {log::field("value", fv)});
        exit_code = 2;
        return false;
      }
    } else if (a == "--dry-run") {
      opt.dry_run = true;
    } else {
      log::error("unknown argument", {log::field("arg", a)});
      print_usage(argv[0]);
      exit_code = 2;
      return false;
    }
  }
  return true;
}

int run(const Options& opt) {
  log::info("hypercored starting",
            {log::field("version", HYPERCORE_VERSION_STRING),
             log::field("config", opt.config_path),
             log::field("socket", opt.socket_path),
             log::field("dry_run", opt.dry_run)});

  // ---------------------------------------------------------------------
  // Phase 2: load + validate the TOML config. Parsing and validation are two
  // separate passes; we surface EVERY diagnostic (errors and warnings), not
  // just the first. Errors mean we refuse to proceed; warnings are logged and
  // tolerated.
  // Phase 3 hook: reconcile desired vs actual VM state, launch QEMU with CPU
  //               pinning, open the control socket, run the health-check loop.
  // ---------------------------------------------------------------------
  namespace cfg = hypercore::config;

  cfg::ParseResult parsed = cfg::parse_file(opt.config_path);
  cfg::Diagnostics diags = parsed.diagnostics;
  if (parsed.config) {
    diags.append(cfg::validate(*parsed.config, cfg::HostInfo::detect()));
  }

  for (const auto& d : diags.items()) {
    auto fields = std::vector<log::Field>{
        log::field("code", cfg::code_str(d.code)),
        log::field("at", d.where), log::field("detail", d.message)};
    if (d.severity == cfg::Severity::Error)
      log::error("config problem", fields);
    else
      log::warn("config advisory", fields);
  }

  if (!parsed.config || diags.has_errors()) {
    log::error("config rejected",
               {log::field("errors", diags.error_count()),
                log::field("warnings", diags.warning_count())});
    return 1;
  }

  log::info("config loaded",
            {log::field("vms", parsed.config->vms.size()),
             log::field("warnings", diags.warning_count())});

  if (opt.dry_run) {
    // Reconcile plan preview: with no actual state yet (Phase 3), the plan is
    // simply "every configured guest would be started".
    for (const auto& vm : parsed.config->vms) {
      log::info("plan: would start guest",
                {log::field("name", vm.name),
                 log::field("cpus", vm.cpus.size()),
                 log::field("memory", cfg::format_bytes(vm.memory_bytes)),
                 log::field("network",
                            vm.network ? cfg::to_string(*vm.network) : "?")});
    }
    log::info("dry-run complete; not launching (Phase 3)", {});
    return 0;
  }

  log::warn(
      "config is valid but the runtime engine is not implemented yet "
      "(reconcile, QEMU launch, and the control socket arrive in Phase 3)",
      {});
  log::info("hypercored exiting cleanly", {});
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  int exit_code = 0;
  bool handled = false;

  if (!parse_args(argc, argv, opt, exit_code, handled)) {
    return exit_code;
  }

  // Apply logging options as early as possible so run() honors them.
  log::default_logger().set_level(opt.level);
  log::default_logger().set_format(opt.format);

  return run(opt);
}
