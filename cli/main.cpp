// hypercore — the CLI client for hypercored.
//
// Talks to the daemon over the control socket (docs/protocol.md). Holds no VM
// state of its own: every command is an RPC, and rendering is derived purely
// from the daemon's responses. `ssh`/`logs` resolve their targets from daemon
// state too (no hardcoded connection details).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <arpa/inet.h>
#include <unistd.h>

#include "client.hpp"
#include "dashboard.hpp"
#include "hypercore/version.hpp"

namespace {

using hypercore::cli::Client;
using hypercore::cli::Reply;

constexpr const char* kDefaultSocket = "/run/hypercore.sock";

void print_usage(const char* argv0) {
  std::printf(
      "hypercore %s — CLI for the hypercore hypervisor\n"
      "\n"
      "Usage: %s [--socket PATH] <command> [args]\n"
      "\n"
      "Commands:\n"
      "  list                 List configured guests and their state\n"
      "  status <name>        Show detailed guest status\n"
      "  start <name|all>     Start a guest (or all)\n"
      "  stop  <name|all>     Stop a guest (or all)\n"
      "  reload               Ask the daemon to re-read config and reconcile\n"
      "  logs  <name>         Show a guest's console log\n"
      "  ssh   <name>         SSH into a guest (endpoint resolved from daemon)\n"
      "  dashboard            Live TTY dashboard (uptime, CPU/RAM, IP/SSH)\n"
      "\n"
      "Options:\n"
      "  --socket PATH        Daemon control socket (default: %s)\n"
      "  -v, --version        Print version and exit\n"
      "  -h, --help           Print this help and exit\n",
      HYPERCORE_VERSION_STRING, argv0, kDefaultSocket);
}

// Print a daemon-side error nicely and return an exit code.
int fail(const Reply& r) {
  std::fprintf(stderr, "hypercore: %s", r.error_message().c_str());
  std::string code = r.error_code();
  if (!code.empty() && code != "transport")
    std::fprintf(stderr, " [%s]", code.c_str());
  std::fprintf(stderr, "\n");
  return r.error_code() == "transport" ? 4 : 5;
}

std::string human_bytes(std::uint64_t b) {
  const char* u[] = {"B", "K", "M", "G", "T"};
  double v = static_cast<double>(b);
  int i = 0;
  while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
  char buf[32];
  std::snprintf(buf, sizeof(buf), v < 10 && i > 0 ? "%.1f%s" : "%.0f%s", v, u[i]);
  return buf;
}

std::string human_uptime(long secs) {
  if (secs <= 0) return "-";
  long d = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60;
  char buf[32];
  if (d) std::snprintf(buf, sizeof(buf), "%ldd%ldh", d, h);
  else if (h) std::snprintf(buf, sizeof(buf), "%ldh%ldm", h, m);
  else std::snprintf(buf, sizeof(buf), "%ldm", m ? m : 1);
  return buf;
}

// Build the SSH endpoint string from a status object, or "" if unknown.
std::string ssh_endpoint(const nlohmann::json& vm) {
  std::string ip = vm.value("ip", "");
  int port = vm.value("ssh_port", 0);
  if (ip.empty()) return "";
  // user-net: forwarded loopback port; bridge: guest IP on :22.
  int p = port > 0 ? port : 22;
  return ip + ":" + std::to_string(p);
}

int cmd_list(Client& c) {
  Reply r = c.request("list");
  if (!r.ok()) return fail(r);
  const auto& vms = r.json["data"]["vms"];
  std::printf("%-14s %-9s %-9s %-8s %6s %8s  %s\n", "NAME", "STATE", "HEALTH",
              "PID", "CPU%", "MEM", "SSH");
  for (const auto& vm : vms) {
    std::string ep = ssh_endpoint(vm);
    char pid[16];
    if (vm.value("pid", nlohmann::json()).is_number())
      std::snprintf(pid, sizeof(pid), "%d", vm.value("pid", 0));
    else
      std::snprintf(pid, sizeof(pid), "-");
    std::printf("%-14s %-9s %-9s %-8s %5.0f%% %8s  %s\n",
                vm.value("name", "?").c_str(), vm.value("state", "?").c_str(),
                vm.value("health", "?").c_str(), pid,
                vm.value("cpu_percent", 0.0),
                human_bytes(vm.value("rss_bytes", 0ull)).c_str(),
                ep.empty() ? "-" : ep.c_str());
  }
  return 0;
}

int cmd_status(Client& c, const std::string& name) {
  Reply r = c.request("status", name);
  if (!r.ok()) return fail(r);
  const auto& vm = r.json["data"];
  auto p = [](const char* k, const std::string& v) {
    std::printf("  %-14s %s\n", k, v.c_str());
  };
  std::printf("guest %s\n", vm.value("name", "?").c_str());
  p("state", vm.value("state", "?"));
  p("health", vm.value("health", "?"));
  p("pid", vm.value("pid", nlohmann::json()).is_number()
               ? std::to_string(vm.value("pid", 0)) : "-");
  {
    std::string cpus;
    for (const auto& c2 : vm.value("cpus", nlohmann::json::array())) {
      if (!cpus.empty()) cpus += ",";
      cpus += std::to_string(c2.get<int>());
    }
    p("pinned cpus", cpus + (vm.value("cpus_verified", false)
                                 ? " (verified)" : " (UNVERIFIED)"));
  }
  p("cpu%", std::to_string(vm.value("cpu_percent", 0.0)));
  p("memory (rss)", human_bytes(vm.value("rss_bytes", 0ull)) + " / " +
                        human_bytes(vm.value("memory_bytes", 0ull)));
  p("network", vm.value("network", "?"));
  {
    std::string ep = ssh_endpoint(vm);
    p("ssh", ep.empty() ? "(unknown — needs guest agent / user-net)" : ep);
  }
  p("restart policy", vm.value("restart", "?"));
  p("uptime", human_uptime(vm.value("uptime_secs", 0)));
  p("restarts", std::to_string(vm.value("restarts", 0)));
  return 0;
}

int cmd_start_stop(Client& c, const std::string& verb, const std::string& arg) {
  if (arg.empty()) {
    std::fprintf(stderr, "hypercore: %s needs a guest name or 'all'\n",
                 verb.c_str());
    return 2;
  }
  Reply r = c.request(verb, arg);
  if (!r.ok()) return fail(r);
  for (const auto& a : r.json["data"]["actions"]) {
    std::printf("%-14s %s", a.value("name", "?").c_str(),
                a.value("result", "?").c_str());
    if (a.contains("pid")) std::printf(" (pid %d)", a.value("pid", 0));
    if (a.contains("error")) std::printf(" — %s", a.value("error", "").c_str());
    std::printf("\n");
  }
  return 0;
}

int cmd_reload(Client& c) {
  Reply r = c.request("reload");
  if (!r.ok()) return fail(r);
  const auto& d = r.json["data"]["diff"];
  auto show = [&](const char* label, const char* key) {
    const auto& arr = d.value(key, nlohmann::json::array());
    if (arr.empty()) return;
    std::printf("%s:", label);
    for (const auto& n : arr) std::printf(" %s", n.get<std::string>().c_str());
    std::printf("\n");
  };
  show("start", "start");
  show("stop", "stop");
  show("restart", "restart");
  show("unchanged", "unchanged");
  return 0;
}

int cmd_logs(Client& c, const std::string& name) {
  // Resolve the console log path from the daemon, then stream the file. The CLI
  // does not know the runtime-dir layout; it asks the daemon (single source of
  // truth). We surface the path via status.serial_log-equivalent: the daemon
  // does not currently expose the path, so we ask for status and read the
  // conventional location it reports. For now, print guidance if unavailable.
  Reply r = c.request("status", name);
  if (!r.ok()) return fail(r);
  const std::string log = r.json["data"].value("console_log", "");
  if (log.empty()) {
    std::fprintf(stderr,
                 "hypercore: daemon did not report a console log path for '%s'\n",
                 name.c_str());
    return 5;
  }
  FILE* f = std::fopen(log.c_str(), "r");
  if (!f) {
    std::fprintf(stderr, "hypercore: cannot open %s\n", log.c_str());
    return 5;
  }
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) std::fwrite(buf, 1, n, stdout);
  std::fclose(f);
  return 0;
}

int cmd_ssh(Client& c, const std::string& name, char** envp) {
  Reply r = c.request("status", name);
  if (!r.ok()) return fail(r);
  const auto& vm = r.json["data"];
  if (vm.value("state", "") != "running") {
    std::fprintf(stderr, "hypercore: guest '%s' is not running (%s)\n",
                 name.c_str(), vm.value("state", "?").c_str());
    return 5;
  }
  std::string ip = vm.value("ip", "");
  int port = vm.value("ssh_port", 0);
  if (ip.empty()) {
    std::fprintf(stderr,
                 "hypercore: no SSH endpoint known for '%s'.\n"
                 "  For bridge networking the guest agent must report an IP;\n"
                 "  for user networking a forwarded port is assigned at launch.\n",
                 name.c_str());
    return 5;
  }
  // Defense in depth (HC-2026-001): the `ip` originates from the guest agent
  // (untrusted). The daemon already restricts it to a valid IPv4 literal, but
  // the CLI must not trust that blindly — re-validate here, and even then never
  // let ssh interpret it as an option. A value beginning with '-' or anything
  // that is not a dotted-quad is refused outright.
  auto is_ipv4 = [](const std::string& s) {
    if (s.empty() || s.size() > 15) return false;
    struct in_addr a{};
    return inet_pton(AF_INET, s.c_str(), &a) == 1;
  };
  if (!is_ipv4(ip)) {
    std::fprintf(stderr,
                 "hypercore: refusing to ssh to '%s' — endpoint '%s' is not a "
                 "valid IPv4 address (guest may be reporting a malicious value)\n",
                 name.c_str(), ip.c_str());
    return 5;
  }
  int p = port > 0 ? port : 22;
  std::string portstr = std::to_string(p);
  std::fprintf(stderr, "hypercore: ssh -> %s:%s\n", ip.c_str(), portstr.c_str());
  // exec ssh, replacing this process (so the user gets a normal ssh session).
  // Harden the invocation: disable ProxyCommand/LocalCommand so even a bug that
  // let a hostile string through cannot spawn a local command, and pass "--" so
  // the destination can never be parsed as an option.
  std::vector<char*> args;
  std::string ssh = "ssh";
  std::string popt = "-p";
  std::string no_proxy = "-oProxyCommand=none";
  std::string no_local = "-oPermitLocalCommand=no";
  std::string dashdash = "--";
  args.push_back(ssh.data());
  args.push_back(popt.data());
  args.push_back(portstr.data());
  args.push_back(no_proxy.data());
  args.push_back(no_local.data());
  args.push_back(dashdash.data());
  args.push_back(ip.data());
  args.push_back(nullptr);
  execvpe("ssh", args.data(), envp);
  std::perror("hypercore: exec ssh failed");
  return 127;
}

}  // namespace

int main(int argc, char** argv, char** envp) {
  std::string socket_path = kDefaultSocket;

  int i = 1;
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
    break;
  }

  if (i >= argc) { print_usage(argv[0]); return 2; }

  std::string cmd = argv[i];
  std::string arg = (i + 1 < argc) ? argv[i + 1] : "";
  Client client(socket_path);

  if (cmd == "list") return cmd_list(client);
  if (cmd == "status") {
    if (arg.empty()) { std::fprintf(stderr, "hypercore: status needs a name\n"); return 2; }
    return cmd_status(client, arg);
  }
  if (cmd == "start" || cmd == "stop") return cmd_start_stop(client, cmd, arg);
  if (cmd == "reload") return cmd_reload(client);
  if (cmd == "logs") {
    if (arg.empty()) { std::fprintf(stderr, "hypercore: logs needs a name\n"); return 2; }
    return cmd_logs(client, arg);
  }
  if (cmd == "ssh") {
    if (arg.empty()) { std::fprintf(stderr, "hypercore: ssh needs a name\n"); return 2; }
    return cmd_ssh(client, arg, envp);
  }
  if (cmd == "dashboard") return hypercore::cli::run_dashboard(client);

  std::fprintf(stderr, "hypercore: unknown command '%s'\n", cmd.c_str());
  print_usage(argv[0]);
  return 2;
}
