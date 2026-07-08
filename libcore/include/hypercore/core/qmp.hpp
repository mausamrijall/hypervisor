// QMP client + the graceful-shutdown escalation ladder (requirement #3c).
#pragma once

#include <chrono>
#include <memory>
#include <string>

namespace hypercore::core {

// A thin QMP client: connect, perform the capabilities handshake, and issue
// fire-and-ack commands. We only need a couple of verbs, so this is not a full
// QMP implementation.
class QmpClient {
 public:
  bool connect(const std::string& socket_path,
               std::chrono::milliseconds timeout, std::string& err);
  // Issue system_powerdown (ACPI). Works for guests running acpid/systemd.
  bool system_powerdown(std::string& err);
  // Issue quit: QEMU exits immediately (not guest-graceful). Last-resort
  // in-band stop before we fall back to signals.
  bool quit(std::string& err);

 private:
  bool handshake(std::string& err);
  // Connection held via type-erased pimpl to keep usock.hpp out of this header.
  std::shared_ptr<void> conn_;
};

// How a stop ultimately completed — reported so callers/tests can assert the
// path taken.
enum class StopOutcome {
  AlreadyDead,     // process wasn't running
  GracefulAgent,   // guest agent shutdown -> guest powered off in time
  GracefulQmp,     // QMP system_powerdown -> guest powered off in time
  Sigterm,         // graceful timed out; SIGTERM to QEMU stopped it
  Sigkill,         // SIGTERM timed out; SIGKILL forced it
  Error,
};

const char* to_string(StopOutcome);

struct StopParams {
  // Prefer guest-agent shutdown (clean for agent-equipped guests), then fall
  // back to QMP powerdown. Either may be empty to skip that mechanism.
  std::string qmp_socket;
  std::string agent_socket;
  std::chrono::milliseconds graceful_timeout{8000};  // wait for guest to leave
  std::chrono::milliseconds sigterm_timeout{4000};   // wait after SIGTERM
};

struct StopResult {
  StopOutcome outcome = StopOutcome::Error;
  std::string detail;
};

// Stop the QEMU process `pid` following the escalation ladder:
//   1. graceful: guest-agent guest-shutdown (if agent_socket set), else QMP
//      system_powerdown (if qmp_socket set); wait graceful_timeout for exit.
//   2. SIGTERM the QEMU process; wait sigterm_timeout for exit.
//   3. SIGKILL.
// A process is considered stopped when kill(pid,0) reports it gone.
StopResult stop_guest(int pid, const StopParams& params);

}  // namespace hypercore::core
