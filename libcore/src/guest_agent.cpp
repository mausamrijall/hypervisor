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

std::string agent_get_ipv4(const std::string& socket_path,
                           std::chrono::milliseconds timeout) {
  UnixClient c;
  std::string err;
  if (!c.connect(socket_path, timeout, err)) return "";
  // Sync, then query interfaces.
  c.write_all("{\"execute\":\"guest-sync\",\"arguments\":{\"id\":7}}\n", err);
  std::string ignore;
  c.read_line(timeout, ignore);
  if (!c.write_all("{\"execute\":\"guest-network-get-interfaces\"}\n", err))
    return "";
  auto line = c.read_line(timeout, err);
  if (!line) return "";

  // Hand-scan for the first non-loopback IPv4. We deliberately avoid a JSON
  // parser on the daemon's privileged read path; the shape we need
  // ("ip-address":"<v4>") is flat enough to extract safely. IPv6 values
  // contain ':' and are skipped; 127.* is skipped.
  const std::string key = "\"ip-address\":";
  std::size_t pos = 0;
  while ((pos = line->find(key, pos)) != std::string::npos) {
    pos += key.size();
    // Skip spaces and the opening quote.
    while (pos < line->size() && (*line)[pos] == ' ') ++pos;
    if (pos >= line->size() || (*line)[pos] != '"') continue;
    ++pos;
    std::size_t end = line->find('"', pos);
    if (end == std::string::npos) break;
    std::string addr = line->substr(pos, end - pos);
    pos = end + 1;
    if (addr.find(':') != std::string::npos) continue;   // IPv6
    if (addr.rfind("127.", 0) == 0) continue;            // loopback
    if (addr.empty()) continue;
    return addr;
  }
  return "";
}

}  // namespace hypercore::core
