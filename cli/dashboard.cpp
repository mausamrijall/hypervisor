#include "dashboard.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include "client.hpp"

namespace hypercore::cli {

namespace {

std::atomic<bool> g_quit{false};
void on_sigint(int) { g_quit.store(true); }

// ANSI helpers.
constexpr const char* kAltScreenOn = "\033[?1049h";
constexpr const char* kAltScreenOff = "\033[?1049l";
constexpr const char* kClearHome = "\033[2J\033[H";
constexpr const char* kHideCursor = "\033[?25l";
constexpr const char* kShowCursor = "\033[?25h";
constexpr const char* kBold = "\033[1m";
constexpr const char* kDim = "\033[2m";
constexpr const char* kReset = "\033[0m";

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

std::string ssh_endpoint(const nlohmann::json& vm) {
  std::string ip = vm.value("ip", "");
  if (ip.empty()) return "-";
  int port = vm.value("ssh_port", 0);
  return ip + ":" + std::to_string(port > 0 ? port : 22);
}

// Health/state coloring for at-a-glance scanning.
const char* state_color(const std::string& state) {
  if (state == "running") return "\033[32m";    // green
  if (state == "unhealthy") return "\033[33m";   // yellow
  if (state == "failed") return "\033[31m";      // red
  if (state == "health_panic") return "\033[1;31m";  // bold red — killed by
                                                     // health panic; resources
                                                     // reclaimed, needs attention
  if (state == "starting" || state == "stopping") return "\033[36m";  // cyan
  return "\033[2m";                              // dim for stopped
}

void draw(const Reply& r) {
  std::string out = kClearHome;
  out += kBold;
  out += "  hypercore dashboard";
  out += kReset;
  out += kDim;
  out += "   (q or Ctrl-C to quit, refreshes every 1s)\n\n";
  out += kReset;

  if (!r.transport_ok) {
    out += "  \033[31mdaemon unreachable:\033[0m " + r.transport_error + "\n";
    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
    return;
  }
  if (!r.ok()) {
    out += "  \033[31merror:\033[0m " + r.error_message() + "\n";
    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
    return;
  }

  char hdr[256];
  std::snprintf(hdr, sizeof(hdr), "  %s%-14s %-9s %-8s %-7s %7s %9s %-8s %s%s\n",
                kBold, "NAME", "STATE", "HEALTH", "PID", "CPU%", "MEM", "UPTIME",
                "SSH", kReset);
  out += hdr;

  const auto& vms = r.json["data"]["vms"];
  int running = 0;
  for (const auto& vm : vms) {
    std::string state = vm.value("state", "?");
    if (state == "running") ++running;
    char pid[16];
    if (vm.value("pid", nlohmann::json()).is_number())
      std::snprintf(pid, sizeof(pid), "%d", vm.value("pid", 0));
    else
      std::snprintf(pid, sizeof(pid), "-");

    char row[512];
    std::snprintf(
        row, sizeof(row),
        "  %-14s %s%-9s%s %-8s %-7s %6.0f%% %9s %-8s %s\n",
        vm.value("name", "?").c_str(), state_color(state), state.c_str(), kReset,
        vm.value("health", "?").c_str(), pid, vm.value("cpu_percent", 0.0),
        human_bytes(vm.value("rss_bytes", 0ull)).c_str(),
        human_uptime(vm.value("uptime_secs", 0)).c_str(),
        ssh_endpoint(vm).c_str());
    out += row;
  }

  char footer[128];
  std::snprintf(footer, sizeof(footer), "\n  %s%d guest(s), %d running%s\n",
                kDim, static_cast<int>(vms.size()), running, kReset);
  out += footer;

  std::fputs(out.c_str(), stdout);
  std::fflush(stdout);
}

// Non-blocking check for a 'q' keypress on stdin (raw mode).
bool q_pressed() {
  pollfd pfd{STDIN_FILENO, POLLIN, 0};
  if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
    char c;
    if (read(STDIN_FILENO, &c, 1) == 1 && (c == 'q' || c == 'Q')) return true;
  }
  return false;
}

}  // namespace

int run_dashboard(Client& client) {
  const bool tty = isatty(STDOUT_FILENO);
  std::signal(SIGINT, on_sigint);
  std::signal(SIGTERM, on_sigint);

  // Put the terminal in raw-ish mode so 'q' is read without Enter.
  termios orig{};
  bool raw = false;
  if (tty && tcgetattr(STDIN_FILENO, &orig) == 0) {
    termios t = orig;
    t.c_lflag &= ~(ICANON | ECHO);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) raw = true;
  }
  if (tty) { std::fputs(kAltScreenOn, stdout); std::fputs(kHideCursor, stdout); }

  int rc = 0;
  while (!g_quit.load()) {
    Reply r = client.request("list");
    draw(r);
    // Sleep ~1s in small slices so 'q' and Ctrl-C feel responsive.
    for (int i = 0; i < 20 && !g_quit.load(); ++i) {
      if (raw && q_pressed()) { g_quit.store(true); break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  if (tty) { std::fputs(kShowCursor, stdout); std::fputs(kAltScreenOff, stdout); std::fflush(stdout); }
  if (raw) tcsetattr(STDIN_FILENO, TCSANOW, &orig);
  return rc;
}

}  // namespace hypercore::cli
