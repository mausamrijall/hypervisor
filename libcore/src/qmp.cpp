#include "hypercore/core/qmp.hpp"

#include <csignal>
#include <memory>
#include <thread>

#include "hypercore/core/guest_agent.hpp"
#include "hypercore/core/pidfile.hpp"
#include "hypercore/core/usock.hpp"

namespace hypercore::core {

using clock = std::chrono::steady_clock;

const char* to_string(StopOutcome o) {
  switch (o) {
    case StopOutcome::AlreadyDead: return "already_dead";
    case StopOutcome::GracefulAgent: return "graceful_agent";
    case StopOutcome::GracefulQmp: return "graceful_qmp";
    case StopOutcome::Sigterm: return "sigterm";
    case StopOutcome::Sigkill: return "sigkill";
    case StopOutcome::Error: return "error";
  }
  return "?";
}

// --- QmpClient ---------------------------------------------------------------

bool QmpClient::connect(const std::string& socket_path,
                        std::chrono::milliseconds timeout, std::string& err) {
  auto c = std::make_shared<UnixClient>();
  if (!c->connect(socket_path, timeout, err)) return false;
  conn_ = c;
  return handshake(err);
}

bool QmpClient::handshake(std::string& err) {
  auto c = std::static_pointer_cast<UnixClient>(conn_);
  // Read the greeting line ({"QMP":{...}}).
  auto greet = c->read_line(std::chrono::milliseconds(2000), err);
  if (!greet) {
    if (err.empty()) err = "no QMP greeting";
    return false;
  }
  // Enter command mode.
  if (!c->write_all("{\"execute\":\"qmp_capabilities\"}\n", err)) return false;
  auto ack = c->read_line(std::chrono::milliseconds(2000), err);
  if (!ack || ack->find("return") == std::string::npos) {
    if (err.empty()) err = "qmp_capabilities not acked";
    return false;
  }
  return true;
}

bool QmpClient::system_powerdown(std::string& err) {
  auto c = std::static_pointer_cast<UnixClient>(conn_);
  if (!c) { err = "not connected"; return false; }
  if (!c->write_all("{\"execute\":\"system_powerdown\"}\n", err)) return false;
  auto ack = c->read_line(std::chrono::milliseconds(2000), err);
  return ack && ack->find("return") != std::string::npos;
}

bool QmpClient::quit(std::string& err) {
  auto c = std::static_pointer_cast<UnixClient>(conn_);
  if (!c) { err = "not connected"; return false; }
  if (!c->write_all("{\"execute\":\"quit\"}\n", err)) return false;
  // quit may close the socket before/after the ack; treat write success as ok.
  std::string ignore;
  c->read_line(std::chrono::milliseconds(500), ignore);
  return true;
}

// --- stop_guest --------------------------------------------------------------

namespace {

// Poll kill(pid,0) until the process is gone or the deadline passes. Returns
// true if it exited.
bool wait_for_exit(int pid, std::chrono::milliseconds timeout) {
  auto deadline = clock::now() + timeout;
  while (clock::now() < deadline) {
    reap_children();  // clear our zombie so pid_alive reflects true state
    if (!pid_alive(pid)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  reap_children();
  return !pid_alive(pid);
}

}  // namespace

StopResult stop_guest(int pid, const StopParams& params) {
  StopResult r;
  if (!pid_alive(pid)) {
    r.outcome = StopOutcome::AlreadyDead;
    return r;
  }

  // --- Step 1: graceful, in-guest first ---
  // Prefer the guest agent (a bare Linux guest without acpid ignores QMP
  // system_powerdown, so the agent is the reliable clean path when present).
  if (!params.agent_socket.empty()) {
    std::string err;
    if (agent_request_shutdown(params.agent_socket,
                               std::chrono::milliseconds(2000), err)) {
      if (wait_for_exit(pid, params.graceful_timeout)) {
        r.outcome = StopOutcome::GracefulAgent;
        r.detail = "guest agent shutdown";
        return r;
      }
    }
  }
  // Then QMP ACPI powerdown (works for acpid/systemd guests).
  if (!params.qmp_socket.empty()) {
    QmpClient q;
    std::string err;
    if (q.connect(params.qmp_socket, std::chrono::milliseconds(1500), err) &&
        q.system_powerdown(err)) {
      if (wait_for_exit(pid, params.graceful_timeout)) {
        r.outcome = StopOutcome::GracefulQmp;
        r.detail = "qmp system_powerdown";
        return r;
      }
    }
  }

  // --- Step 2: SIGTERM the QEMU process ---
  if (::kill(pid, SIGTERM) == 0) {
    if (wait_for_exit(pid, params.sigterm_timeout)) {
      r.outcome = StopOutcome::Sigterm;
      r.detail = "graceful timed out; SIGTERM stopped it";
      return r;
    }
  }

  // --- Step 3: SIGKILL, the last resort ---
  ::kill(pid, SIGKILL);
  if (wait_for_exit(pid, std::chrono::milliseconds(3000))) {
    r.outcome = StopOutcome::Sigkill;
    r.detail = "SIGTERM timed out; SIGKILL forced it";
    return r;
  }

  r.outcome = StopOutcome::Error;
  r.detail = "process survived SIGKILL (unexpected)";
  return r;
}

}  // namespace hypercore::core
