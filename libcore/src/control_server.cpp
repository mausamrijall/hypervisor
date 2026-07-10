#include "hypercore/core/control_server.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sstream>
#include <vector>

#include "hypercore/core/pidfile.hpp"
#include "hypercore/log.hpp"

namespace hypercore::core {

namespace log = hypercore::log;

namespace {

// Minimal JSON string escaping for values we emit.
std::string jesc(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      default: o += c;
    }
  }
  return o;
}

std::vector<std::string> tokenize(const std::string& line) {
  std::vector<std::string> out;
  std::istringstream ss(line);
  std::string tok;
  while (ss >> tok) out.push_back(tok);
  return out;
}

bool valid_name(const std::string& n) {
  if (n.empty()) return false;
  auto ok = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
  };
  if (!((n[0] >= 'a' && n[0] <= 'z') || (n[0] >= '0' && n[0] <= '9')))
    return false;
  for (char c : n)
    if (!ok(c)) return false;
  return true;
}

std::string err_response(const std::string& cmd, const std::string& code,
                         const std::string& msg) {
  std::ostringstream o;
  o << "{\"proto\":" << kProtoVersion << ",\"ok\":false,\"command\":\""
    << jesc(cmd) << "\",\"error\":{\"code\":\"" << jesc(code)
    << "\",\"message\":\"" << jesc(msg) << "\"}}";
  return o.str();
}

}  // namespace

ControlServer::ControlServer(std::string socket_path, Supervisor& sup)
    : socket_path_(std::move(socket_path)), sup_(sup) {}

ControlServer::~ControlServer() {
  if (listen_fd_ >= 0) ::close(listen_fd_);
  if (!socket_path_.empty()) ::unlink(socket_path_.c_str());
}

bool ControlServer::listen(std::string& err) {
  if (socket_path_.size() >= sizeof(sockaddr_un::sun_path)) {
    err = "socket path too long";
    return false;
  }
  // If a stale socket file exists and nobody is listening, remove it.
  ::unlink(socket_path_.c_str());

  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    err = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  // Restrict the socket to the owner (protocol.md: 0600, no auth beyond perms).
  mode_t old = ::umask(0177);
  int rc = ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  ::umask(old);
  if (rc != 0) {
    err = std::string("bind: ") + std::strerror(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (::listen(listen_fd_, 16) != 0) {
    err = std::string("listen: ") + std::strerror(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  return true;
}

std::string ControlServer::status_json(const VmRuntime& rt) const {
  std::ostringstream o;
  auto secs = rt.pid > 0
                  ? std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - rt.started_at)
                        .count()
                  : 0;
  o << "{\"name\":\"" << jesc(rt.name) << "\""
    << ",\"state\":\"" << to_string(rt.state) << "\""
    << ",\"health\":\"" << to_string(rt.health) << "\""
    << ",\"pid\":" << (rt.pid > 0 ? std::to_string(rt.pid) : "null")
    << ",\"cpus\":[";
  for (std::size_t i = 0; i < rt.cpus.size(); ++i) {
    if (i) o << ",";
    o << rt.cpus[i];
  }
  o << "]"
    << ",\"cpus_verified\":" << (rt.cpus_verified ? "true" : "false")
    << ",\"memory_bytes\":" << rt.memory_bytes
    << ",\"rss_bytes\":" << rt.rss_bytes
    << ",\"cpu_percent\":" << static_cast<long long>(rt.cpu_percent * 10) / 10.0
    << ",\"network\":\"" << config::to_string(rt.network) << "\""
    << ",\"ip\":\"" << jesc(rt.ip) << "\""
    << ",\"ssh_port\":" << rt.ssh_port
    << ",\"console_log\":\"" << jesc(rt.serial_log) << "\""
    << ",\"restart\":\"" << config::to_string(rt.restart) << "\""
    << ",\"uptime_secs\":" << secs
    << ",\"restarts\":" << rt.restarts
    << ",\"adopted\":" << (rt.adopted ? "true" : "false") << "}";
  return o.str();
}

std::string ControlServer::handle_request(const std::string& line) {
  auto toks = tokenize(line);
  if (toks.empty()) return err_response("?", "bad_request", "empty request");

  // Parse protocol version.
  int proto = -1;
  try {
    proto = std::stoi(toks[0]);
  } catch (...) {
    return err_response("?", "bad_request", "leading token must be proto int");
  }
  if (proto != kProtoVersion) {
    return err_response("?", "proto_mismatch",
                        "daemon speaks proto " + std::to_string(kProtoVersion));
  }
  if (toks.size() < 2)
    return err_response("?", "bad_request", "missing command");

  const std::string& cmd = toks[1];
  auto arg = toks.size() >= 3 ? toks[2] : std::string();

  // Helper to open a success envelope.
  auto ok_prefix = [&](const std::string& c) {
    std::ostringstream o;
    o << "{\"proto\":" << kProtoVersion << ",\"ok\":true,\"command\":\"" << c
      << "\",\"data\":";
    return o.str();
  };

  if (cmd == "list") {
    if (toks.size() != 2)
      return err_response(cmd, "bad_request", "list takes no argument");
    std::ostringstream o;
    o << ok_prefix("list") << "{\"vms\":[";
    bool first = true;
    for (const auto& [name, rt] : sup_.runtimes()) {
      if (!first) o << ",";
      first = false;
      o << status_json(rt);
    }
    o << "]}}";
    return o.str();
  }

  if (cmd == "status") {
    if (!valid_name(arg))
      return err_response(cmd, "bad_request", "status needs a valid vm name");
    if (!sup_.has_vm(arg))
      return err_response(cmd, "unknown_vm", "no such vm: " + arg);
    return ok_prefix("status") + status_json(sup_.runtimes().at(arg)) + "}";
  }

  if (cmd == "start" || cmd == "stop") {
    const bool is_start = (cmd == "start");
    if (arg != "all" && !valid_name(arg))
      return err_response(cmd, "bad_request", cmd + " needs a vm name or 'all'");
    std::vector<std::string> targets;
    if (arg == "all") {
      for (const auto& [name, rt] : sup_.runtimes()) targets.push_back(name);
    } else {
      if (!sup_.has_vm(arg))
        return err_response(cmd, "unknown_vm", "no such vm: " + arg);
      targets.push_back(arg);
    }
    std::ostringstream o;
    o << ok_prefix(cmd) << "{\"actions\":[";
    for (std::size_t i = 0; i < targets.size(); ++i) {
      if (i) o << ",";
      const std::string& name = targets[i];
      if (is_start) {
        std::string err;
        bool already = sup_.runtimes().at(name).state == VmState::Running;
        bool ok = already ? false : sup_.start(name, err);
        const char* result = already ? "already_running"
                                      : (ok ? "started" : "launch_failed");
        o << "{\"name\":\"" << jesc(name) << "\",\"result\":\"" << result
          << "\"";
        if (ok) o << ",\"pid\":" << sup_.runtimes().at(name).pid;
        if (!ok && !already) o << ",\"error\":\"" << jesc(err) << "\"";
        o << "}";
      } else {
        StopResult sr = sup_.stop(name);
        const char* result =
            sr.outcome == StopOutcome::AlreadyDead ? "already_stopped"
            : sr.outcome == StopOutcome::Sigkill   ? "killed"
                                                   : "stopped";
        o << "{\"name\":\"" << jesc(name) << "\",\"result\":\"" << result
          << "\",\"outcome\":\"" << to_string(sr.outcome) << "\"}";
      }
    }
    o << "]}}";
    return o.str();
  }

  if (cmd == "reload") {
    if (toks.size() != 2)
      return err_response(cmd, "bad_request", "reload takes no argument");
    // Recompute plan against current running state and apply it.
    ReconcilePlan p = sup_.plan();
    sup_.apply(p);
    std::ostringstream o;
    auto arr = [&](const std::vector<std::string>& v) {
      std::string s = "[";
      for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += ",";
        s += "\"" + jesc(v[i]) + "\"";
      }
      return s + "]";
    };
    o << ok_prefix("reload") << "{\"diff\":{\"start\":" << arr(p.to_start)
      << ",\"stop\":" << arr(p.to_stop) << ",\"restart\":" << arr(p.to_restart)
      << ",\"unchanged\":" << arr(p.unchanged) << "}}}";
    return o.str();
  }

  return err_response(cmd, "unknown_command", "unknown command: " + cmd);
}

void ControlServer::run(const std::atomic<bool>& stop,
                        std::chrono::milliseconds tick,
                        const std::function<void()>& on_tick) {
  while (!stop.load()) {
    pollfd pfd{listen_fd_, POLLIN, 0};
    int pr = ::poll(&pfd, 1, static_cast<int>(tick.count()));
    if (pr < 0) {
      if (errno == EINTR) continue;
      log::error("control poll error",
                 {log::field("err", std::strerror(errno))});
      break;
    }
    if (pr == 0) {
      if (on_tick) on_tick();  // health checks between connections
      continue;
    }
    int cfd = ::accept(listen_fd_, nullptr, nullptr);
    if (cfd < 0) continue;

    // Read one line (bounded), respond, close. Connection-per-request.
    std::string req;
    char buf[512];
    bool got_line = false;
    for (int i = 0; i < 64 && !got_line; ++i) {  // bound total reads
      ssize_t n = ::read(cfd, buf, sizeof(buf));
      if (n <= 0) break;
      req.append(buf, static_cast<std::size_t>(n));
      if (req.find('\n') != std::string::npos) got_line = true;
      if (req.size() > 8192) break;  // guard against unbounded input
    }
    auto nl = req.find('\n');
    std::string line = nl == std::string::npos ? req : req.substr(0, nl);
    // Defense in depth: handle_request parses untrusted socket input. Guarantee
    // that NO exception (a missed map::at, std::bad_alloc, stoi edge case, or
    // any future code path) can ever unwind out of the accept loop and take the
    // daemon down. A malformed request must, at worst, get an error reply.
    std::string resp;
    try {
      resp = handle_request(line);
    } catch (const std::exception& e) {
      resp = err_response("?", "internal",
                          std::string("request handling error: ") + e.what());
    } catch (...) {
      resp = err_response("?", "internal", "unknown request handling error");
    }
    resp.push_back('\n');
    ssize_t w = ::write(cfd, resp.data(), resp.size());
    (void)w;
    ::close(cfd);
  }
}

}  // namespace hypercore::core
