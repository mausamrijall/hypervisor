// hypercore — the thin CLI client for hypercored.
//
// Phase 1: recognizes the subcommand surface and prints help/version so the
// UX shape is visible and testable. The actual work — connecting to the
// daemon's Unix socket and issuing list/start/stop/logs/ssh, plus the live TTY
// dashboard — lands in Phase 4. Subcommands currently report "not implemented"
// with a clear exit code rather than pretending to succeed.

#include <cstdio>
#include <string>
#include <string_view>

#include "hypercore/version.hpp"

namespace {

constexpr const char* kDefaultSocket = "/run/hypercore.sock";

void print_usage(const char* argv0) {
  std::printf(
      "hypercore %s — CLI for the hypercore hypervisor\n"
      "\n"
      "Usage: %s [--socket PATH] <command> [args]\n"
      "\n"
      "Commands:\n"
      "  list                 List configured guests and their state\n"
      "  start <name|all>     Start a guest\n"
      "  stop  <name|all>     Stop a guest\n"
      "  status <name>        Show detailed guest status\n"
      "  logs  <name>         Stream a guest's logs\n"
      "  ssh   <name>         Open an SSH session to a guest\n"
      "  dashboard            Live TTY dashboard (uptime, CPU/RAM, IP/SSH)\n"
      "  reload               Ask the daemon to re-read its config\n"
      "\n"
      "Options:\n"
      "  --socket PATH        Daemon control socket (default: %s)\n"
      "  -v, --version        Print version and exit\n"
      "  -h, --help           Print this help and exit\n",
      HYPERCORE_VERSION_STRING, argv0, kDefaultSocket);
}

int not_implemented(std::string_view cmd) {
  std::fprintf(stderr,
               "hypercore: '%.*s' is not implemented yet (arrives in Phase 4)\n",
               static_cast<int>(cmd.size()), cmd.data());
  return 3;  // distinct from usage(2); "recognized but unavailable"
}

}  // namespace

int main(int argc, char** argv) {
  std::string socket_path = kDefaultSocket;

  int i = 1;
  // Global options may precede the subcommand.
  for (; i < argc; ++i) {
    std::string_view a = argv[i];
    if (a == "-h" || a == "--help") { print_usage(argv[0]); return 0; }
    if (a == "-v" || a == "--version") {
      std::printf("%s\n", HYPERCORE_VERSION_STRING);
      return 0;
    }
    if (a == "--socket") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "hypercore: --socket requires a value\n");
        return 2;
      }
      socket_path = argv[++i];
      continue;
    }
    break;  // first non-option token is the subcommand
  }

  if (i >= argc) {
    print_usage(argv[0]);
    return 2;
  }

  std::string_view cmd = argv[i];
  if (cmd == "list" || cmd == "start" || cmd == "stop" || cmd == "status" ||
      cmd == "logs" || cmd == "ssh" || cmd == "dashboard" || cmd == "reload") {
    return not_implemented(cmd);
  }

  std::fprintf(stderr, "hypercore: unknown command '%.*s'\n",
               static_cast<int>(cmd.size()), cmd.data());
  print_usage(argv[0]);
  return 2;
}
