// Control socket server — the ndjson line-request API from docs/protocol.md.
//
// Requests are one flat whitespace-delimited line: "<proto> <command> [arg]".
// Responses are one JSON object per line. The daemon never runs a recursive
// parser on request input (deliberate: minimal attack surface on the privileged
// control path); it only WRITES JSON, which is trivial and safe.
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

#include "hypercore/core/supervisor.hpp"

namespace hypercore::core {

// Bumped on any breaking change to request grammar or response fields.
inline constexpr int kProtoVersion = 1;

class ControlServer {
 public:
  ControlServer(std::string socket_path, Supervisor& sup);
  ~ControlServer();

  // Create, bind (0600), and listen on the socket. False + err on failure
  // (e.g. path in use). Removes a stale socket file first if no one answers.
  bool listen(std::string& err);

  // Accept/serve loop. Between connections (every `tick` interval) invokes
  // `on_tick` so the daemon can run health checks on the same thread — no
  // locking needed because request handling and ticks never overlap. Runs
  // until *stop becomes true.
  void run(const std::atomic<bool>& stop, std::chrono::milliseconds tick,
           const std::function<void()>& on_tick);

  // Handle a single request line and produce a response line (no trailing
  // newline). Exposed for unit testing without a real socket.
  std::string handle_request(const std::string& line);

  int fd() const { return listen_fd_; }

 private:
  std::string status_json(const VmRuntime& rt) const;

  std::string socket_path_;
  Supervisor& sup_;
  int listen_fd_ = -1;
};

}  // namespace hypercore::core
