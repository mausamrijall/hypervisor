// QEMU guest-agent interaction (requirement #4).
//
// Two operations the daemon needs:
//   - ping():      liveness probe used by the health checker. Sends
//                  guest-sync then guest-ping over the agent's virtio-serial
//                  Unix socket and expects the matching returns within a
//                  deadline. Any timeout / bad reply => not alive.
//   - shutdown():  request a cooperative guest power-off (the CLEAN stop path,
//                  since a bare Linux guest without acpid ignores QMP
//                  system_powerdown).
#pragma once

#include <chrono>
#include <string>

namespace hypercore::core {

struct AgentPing {
  bool alive = false;
  std::string detail;  // reason when !alive (timeout / eof / bad reply)
};

// Probe the guest agent at `socket_path`. `sync_id` disambiguates the
// guest-sync handshake; callers should vary it per attempt.
AgentPing agent_ping(const std::string& socket_path, int sync_id,
                     std::chrono::milliseconds timeout);

// Ask the guest to shut itself down cleanly. Returns true if the command was
// accepted (a return object came back); the caller then waits for the QEMU
// process to actually exit.
bool agent_request_shutdown(const std::string& socket_path,
                            std::chrono::milliseconds timeout,
                            std::string& err);

// Query the guest's primary non-loopback IPv4 address via
// guest-network-get-interfaces. Returns "" if the agent is unreachable, has no
// such address yet, or the reply can't be understood. Best-effort: used to
// populate the SSH endpoint for bridged guests.
std::string agent_get_ipv4(const std::string& socket_path,
                           std::chrono::milliseconds timeout);

}  // namespace hypercore::core
