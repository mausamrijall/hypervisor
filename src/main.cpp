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
  // Phase 2 hook: load + validate the TOML config at opt.config_path.
  // Phase 3 hook: reconcile desired vs actual VM state, launch QEMU with
  //               CPU pinning, open the control socket at opt.socket_path,
  //               install signal handlers, run the health-check loop.
  // For Phase 1 we simply report readiness and exit so the binary is a
  // known-good baseline.
  // ---------------------------------------------------------------------

  log::warn(
      "no runtime engine yet: this is the Phase 1 skeleton — parsing, "
      "reconcile, and the control socket are not implemented",
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
