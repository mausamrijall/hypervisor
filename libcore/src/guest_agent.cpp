#include "hypercore/core/guest_agent.hpp"

#include <string>

#include "hypercore/core/usock.hpp"

namespace hypercore::core {

namespace {
// We do not pull in a JSON parser for the daemon's read path (same reasoning as
// the control protocol: minimize parser surface). The agent's replies are tiny
// and fixed-shape, so a substring check is sufficient and safe here.
bool contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}
}  // namespace

AgentPing agent_ping(const std::string& socket_path, int sync_id,
                     std::chrono::milliseconds timeout) {
  AgentPing r;
  UnixClient c;
  std::string err;
  if (!c.connect(socket_path, timeout, err)) {
    r.detail = "connect: " + err;
    return r;
  }
  // guest-sync-delimited first: the agent echoes our id so we know we're in
  // sync and not reading a stale buffered reply.
  std::string sync = "{\"execute\":\"guest-sync\",\"arguments\":{\"id\":" +
                     std::to_string(sync_id) + "}}\n";
  if (!c.write_all(sync, err)) {
    r.detail = "write sync: " + err;
    return r;
  }
  auto line = c.read_line(timeout, err);
  if (!line) {
    r.detail = err.empty() ? "sync timeout" : ("sync read: " + err);
    return r;
  }
  if (!contains(*line, std::to_string(sync_id))) {
    r.detail = "sync id mismatch: " + *line;
    return r;
  }
  // Now the actual ping.
  if (!c.write_all("{\"execute\":\"guest-ping\"}\n", err)) {
    r.detail = "write ping: " + err;
    return r;
  }
  line = c.read_line(timeout, err);
  if (!line) {
    r.detail = err.empty() ? "ping timeout" : ("ping read: " + err);
    return r;
  }
  if (!contains(*line, "return")) {
    r.detail = "ping bad reply: " + *line;
    return r;
  }
  r.alive = true;
  r.detail = "ok";
  return r;
}

bool agent_request_shutdown(const std::string& socket_path,
                            std::chrono::milliseconds timeout,
                            std::string& err) {
  UnixClient c;
  if (!c.connect(socket_path, timeout, err)) return false;
  // Best-effort sync first (ignore result), then request shutdown.
  c.write_all("{\"execute\":\"guest-sync\",\"arguments\":{\"id\":1}}\n", err);
  std::string ignore;
  c.read_line(timeout, ignore);
  if (!c.write_all(
          "{\"execute\":\"guest-shutdown\",\"arguments\":{\"mode\":\"powerdown\"}}\n",
          err))
    return false;
  auto line = c.read_line(timeout, err);
  if (!line) {
    if (err.empty()) err = "shutdown ack timeout";
    return false;
  }
  // Agent returns {"return":{}} on accept.
  if (line->find("return") == std::string::npos) {
    err = "shutdown not accepted: " + *line;
    return false;
  }
  return true;
}

}  // namespace hypercore::core
