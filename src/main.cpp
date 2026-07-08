// hypercored — the hypercore hypervisor daemon.
//
// Phase 3: the daemon now has a runtime engine. It parses + validates config,
// reconciles desired vs actual VM state, launches QEMU with verified CPU
// pinning, serves the ndjson control socket, and health-checks guests applying
// their restart policy.
//
// Modes:
//   (default)             run: adopt existing guests, reconcile, serve socket,
//                         health-check until signaled.
//   --reconcile --dry-run print the reconcile diff and exit, touching nothing.
//   --dry-run             (config check) parse+validate, print the start plan,
//                         exit without launching.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hypercore/config/parser.hpp"
#include "hypercore/config/validator.hpp"
#include "hypercore/core/capabilities.hpp"
#include "hypercore/core/control_server.hpp"
#include "hypercore/core/reconcile.hpp"
#include "hypercore/core/supervisor.hpp"
#include "hypercore/log.hpp"
#include "hypercore/version.hpp"

namespace {

namespace log = hypercore::log;
namespace core = hypercore::core;

constexpr const char* kDefaultConfig = "/etc/hypercore/hypervisor.cfg";
constexpr const char* kDefaultSocket = "/run/hypercore.sock";
constexpr const char* kDefaultRuntimeDir = "/run/hypercore";

struct Options {
  std::string config_path = kDefaultConfig;
  std::string socket_path = kDefaultSocket;
  std::string runtime_dir = kDefaultRuntimeDir;
  log::Level level = log::Level::Info;
  log::Format format = log::Format::Logfmt;
  bool dry_run = false;    // don't launch anything
  bool reconcile = false;  // reconcile-mode (with --dry-run: print diff only)
};

// Signal-driven shutdown flag.
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

void print_usage(const char* argv0) {
  std::printf(
      "hypercored %s — hypercore hypervisor daemon\n"
      "\n"
      "Usage: %s [options]\n"
      "\n"
      "Options:\n"
      "  -c, --config PATH   Config file (default: %s)\n"
      "  -s, --socket PATH   Control socket path (default: %s)\n"
      "      --runtime-dir D Runtime state dir for pid/sockets (default: %s)\n"
      "      --reconcile     Reconcile desired vs actual state. With --dry-run,\n"
      "                      print the diff (start/stop/restart) and exit\n"
      "                      WITHOUT touching any process.\n"
      "      --dry-run       Do not launch/stop anything; print the plan.\n"
      "      --log-level L   trace|debug|info|warn|error (default: info)\n"
      "      --log-format F  logfmt|json (default: logfmt)\n"
      "  -v, --version       Print version and exit\n"
      "  -h, --help          Print this help and exit\n",
      HYPERCORE_VERSION_STRING, argv0, kDefaultConfig, kDefaultSocket,
      kDefaultRuntimeDir);
}

bool parse_args(int argc, char** argv, Options& opt, int& exit_code) {
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
      exit_code = 0;
      return false;
    }
    if (a == "-v" || a == "--version") {
      std::printf("%s\n", HYPERCORE_VERSION_STRING);
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
    } else if (a == "--runtime-dir") {
      const char* v = needs_value(i, a);
      if (!v) return false;
      opt.runtime_dir = v;
    } else if (a == "--reconcile") {
      opt.reconcile = true;
    } else if (a == "--dry-run") {
      opt.dry_run = true;
    } else if (a == "--log-level") {
      const char* v = needs_value(i, a);
      if (!v) return false;
      std::string_view lv = v;
      if (lv == "trace") opt.level = log::Level::Trace;
      else if (lv == "debug") opt.level = log::Level::Debug;
      else if (lv == "info") opt.level = log::Level::Info;
      else if (lv == "warn") opt.level = log::Level::Warn;
      else if (lv == "error") opt.level = log::Level::Error;
      else { log::error("invalid log level", {log::field("value", lv)});
             exit_code = 2; return false; }
    } else if (a == "--log-format") {
      const char* v = needs_value(i, a);
      if (!v) return false;
      std::string_view fv = v;
      if (fv == "logfmt") opt.format = log::Format::Logfmt;
      else if (fv == "json") opt.format = log::Format::Json;
      else { log::error("invalid log format", {log::field("value", fv)});
             exit_code = 2; return false; }
    } else {
      log::error("unknown argument", {log::field("arg", a)});
      print_usage(argv[0]);
      exit_code = 2;
      return false;
    }
  }
  return true;
}

// Load + validate config, logging every diagnostic. Returns nullopt on error.
std::optional<hypercore::config::Config> load_config(const Options& opt) {
  namespace cfg = hypercore::config;
  cfg::ParseResult parsed = cfg::parse_file(opt.config_path);
  cfg::Diagnostics diags = parsed.diagnostics;
  if (parsed.config)
    diags.append(cfg::validate(*parsed.config, cfg::HostInfo::detect()));
  for (const auto& d : diags.items()) {
    std::vector<log::Field> f{log::field("code", cfg::code_str(d.code)),
                              log::field("at", d.where),
                              log::field("detail", d.message)};
    if (d.severity == cfg::Severity::Error) log::error("config problem", f);
    else log::warn("config advisory", f);
  }
  if (!parsed.config || diags.has_errors()) {
    log::error("config rejected",
               {log::field("errors", diags.error_count())});
    return std::nullopt;
  }
  return parsed.config;
}

void log_plan(const core::ReconcilePlan& p) {
  auto emit = [](const char* action, const std::vector<std::string>& v) {
    for (const auto& n : v)
      log::info("reconcile plan",
                {log::field("action", action), log::field("vm", n)});
  };
  emit("start", p.to_start);
  emit("stop", p.to_stop);
  emit("restart", p.to_restart);
  emit("unchanged", p.unchanged);
  if (p.empty() && p.unchanged.empty())
    log::info("reconcile plan: nothing to do", {});
}

int run(const Options& opt) {
  log::info("hypercored starting",
            {log::field("version", HYPERCORE_VERSION_STRING),
             log::field("config", opt.config_path),
             log::field("reconcile", opt.reconcile),
             log::field("dry_run", opt.dry_run)});

  auto cfg = load_config(opt);
  if (!cfg) return 1;
  log::info("config loaded", {log::field("vms", cfg->vms.size())});

  // Report host capabilities up front — the operator should know immediately
  // whether hardware acceleration is available.
  core::Capabilities caps = core::detect_capabilities();
  log::info("host capabilities",
            {log::field("kvm", caps.kvm), log::field("qemu", caps.qemu),
             log::field("qemu_path", caps.qemu_path),
             log::field("kvm_reason", caps.kvm_reason)});

  core::SupervisorOptions sopts;
  sopts.runtime_dir = opt.runtime_dir;
  core::Supervisor sup(*cfg, sopts);

  // --reconcile --dry-run: adopt (read-only discovery of running guests), then
  // print the diff against real state and exit WITHOUT touching processes.
  if (opt.reconcile && opt.dry_run) {
    sup.adopt_existing();
    core::ReconcilePlan p = sup.plan();
    log_plan(p);
    log::info("reconcile dry-run complete; no processes touched", {});
    return 0;
  }

  // Plain --dry-run: config check + start plan, launch nothing.
  if (opt.dry_run) {
    core::ReconcilePlan p = sup.plan();
    log_plan(p);
    log::info("dry-run complete; not launching", {});
    return 0;
  }

  // --- live daemon ---
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);  // never die on a client hangup

  sup.adopt_existing();
  sup.apply(sup.plan());

  core::ControlServer server(opt.socket_path, sup);
  std::string err;
  if (!server.listen(err)) {
    log::error("cannot open control socket",
               {log::field("path", opt.socket_path), log::field("error", err)});
    return 1;
  }
  log::info("control socket listening", {log::field("path", opt.socket_path)});

  // Serve requests and run health checks between them on one thread.
  server.run(g_stop, std::chrono::milliseconds(1000),
             [&sup] { sup.health_tick(); });

  log::info("shutting down; stopping guests", {});
  for (const auto& [name, rt] : sup.runtimes()) {
    (void)rt;
    sup.stop(name);
  }
  log::info("hypercored stopped cleanly", {});
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  int exit_code = 0;
  if (!parse_args(argc, argv, opt, exit_code)) return exit_code;
  log::default_logger().set_level(opt.level);
  log::default_logger().set_format(opt.format);
  return run(opt);
}
