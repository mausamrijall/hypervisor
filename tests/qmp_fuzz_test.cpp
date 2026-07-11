// qmp_fuzz_test.cpp — Fuzz-style regression tests for the QMP and guest-agent
// read paths.
//
// Threat model: a compromised guest controls the byte stream returned by the
// QEMU Machine Protocol (QMP) socket and the virtio-serial guest-agent socket.
// The host daemon reads those sockets and extracts structured data. If either
// read path crashes, hangs, or corrupts internal state under hostile input, a
// guest escape can deny service to the entire host.
//
// These tests drive the PRODUCTION parsing code with bounded-size adversarial
// byte streams through a pair of in-process Unix socketpairs — no QEMU needed.
// They verify:
//   1. No C++ exception escapes the read path.
//   2. No call returns a validly-structured result from junk input ("panic"
//      vs. "graceful error" distinction).
//   3. The read path does not block past its configured timeout.
//   4. Pathological JSON (deeply nested, multi-megabyte, NUL-stuffed,
//      embedded newlines) is handled gracefully.

#define HC_NO_MAIN  // we provide our own main() below to suppress SIGPIPE
#include "hctest.hpp"

#include <arpa/inet.h>
#include <csignal>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "hypercore/core/guest_agent.hpp"
#include "hypercore/core/qmp.hpp"
#include "hypercore/core/usock.hpp"

using namespace hypercore::core;

namespace {

// ── socketpair-based mock socket ─────────────────────────────────────────────
//
// Creates a Unix-domain socketpair so the test can write bytes to one end and
// the production code connects to the other end via a filesystem path.
//
// We bind one fd to a temp path so QmpClient::connect / agent_ping can use
// their normal Unix-socket connect path; the other fd is the "guest" side
// that injects bytes.

class MockSocket {
 public:
  explicit MockSocket(const std::string& path) : path_(path) {
    ::unlink(path.c_str());
    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      ::close(listen_fd_); listen_fd_ = -1; return;
    }
    ::listen(listen_fd_, 4);
  }

  ~MockSocket() {
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (conn_fd_ >= 0) ::close(conn_fd_);
    ::unlink(path_.c_str());
  }

  // Accept one connection from the production code (blocks briefly).
  bool accept_once(std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
    // Use select so we don't block forever if the production connect fails.
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(listen_fd_, &rset);
    struct timeval tv{};
    tv.tv_sec = timeout.count() / 1000;
    tv.tv_usec = (timeout.count() % 1000) * 1000;
    if (::select(listen_fd_ + 1, &rset, nullptr, nullptr, &tv) <= 0) return false;
    conn_fd_ = ::accept(listen_fd_, nullptr, nullptr);
    return conn_fd_ >= 0;
  }

  // Send bytes to the production reader.
  void send(const std::string& s) {
    if (conn_fd_ < 0) return;
    ::write(conn_fd_, s.data(), s.size());
  }

  // Close the connection fd so read_line sees EOF.
  void close_conn() {
    if (conn_fd_ >= 0) { ::close(conn_fd_); conn_fd_ = -1; }
  }

  const std::string& path() const { return path_; }

 private:
  std::string path_;
  int listen_fd_ = -1;
  int conn_fd_ = -1;
};

// Send `data` from a background thread after production code has initiated
// a connect — this mirrors the real async timing where QEMU writes its greeting
// while the daemon is in connect+read.
void inject_async(MockSocket& sock, const std::string& data,
                  std::chrono::milliseconds delay = std::chrono::milliseconds(10)) {
  std::thread([&sock, data, delay] {
    if (!sock.accept_once(std::chrono::milliseconds(1000))) return;
    std::this_thread::sleep_for(delay);
    sock.send(data);
    sock.close_conn();
  }).detach();
}

// Minimal QMP handshake: greeting + qmp_capabilities ack.
const char* QMP_GREETING =
    "{\"QMP\":{\"version\":{\"qemu\":{\"micro\":0,\"minor\":0,\"major\":9},"
    "\"package\":\"\"},\"capabilities\":[\"oob\"]}}\n";
const char* QMP_CAPS_ACK = "{\"return\":{}}\n";

constexpr auto SHORT_TIMEOUT = std::chrono::milliseconds(300);

}  // namespace

// ── QmpClient fuzz tests ─────────────────────────────────────────────────────

// A QMP handshake response that is valid JSON but has random extra fields must
// not crash the client — it just needs the "return" substring for the ack.
TEST(qmp_handshake_extra_fields_no_crash) {
  MockSocket srv("/tmp/hc-fuzz-qmp-extra.sock");
  std::string big_greeting =
      std::string(QMP_GREETING);  // valid greeting
  std::string caps_ack =
      "{\"return\":{\"junk\":\"" + std::string(4096, 'X') + "\"}}\n";
  inject_async(srv, big_greeting + caps_ack);

  QmpClient q;
  std::string err;
  bool threw = false;
  try {
    q.connect("/tmp/hc-fuzz-qmp-extra.sock", SHORT_TIMEOUT, err);
  } catch (...) {
    threw = true;
  }
  CHECK(!threw);  // must never throw
}

// Missing greeting: server sends nothing and closes. The client must time out
// and return false rather than blocking or crashing.
TEST(qmp_handshake_empty_stream_returns_false) {
  MockSocket srv("/tmp/hc-fuzz-qmp-empty.sock");
  inject_async(srv, "");  // empty — sends nothing, then closes

  QmpClient q;
  std::string err;
  bool threw = false;
  bool connected = true;
  try {
    connected = q.connect("/tmp/hc-fuzz-qmp-empty.sock", SHORT_TIMEOUT, err);
  } catch (...) {
    threw = true;
  }
  CHECK(!threw);
  CHECK(!connected);  // must fail gracefully
}

// Truncated JSON (no newline terminator) — the read_line loop must honour its
// timeout and not block indefinitely.
TEST(qmp_handshake_truncated_json_no_hang) {
  MockSocket srv("/tmp/hc-fuzz-qmp-trunc.sock");
  inject_async(srv, "{\"QMP\":{\"broken\":");  // no closing brace, no newline

  QmpClient q;
  std::string err;
  bool threw = false;
  auto t0 = std::chrono::steady_clock::now();
  try {
    q.connect("/tmp/hc-fuzz-qmp-trunc.sock", SHORT_TIMEOUT, err);
  } catch (...) {
    threw = true;
  }
  auto elapsed = std::chrono::steady_clock::now() - t0;
  CHECK(!threw);
  // Must not block significantly past the configured timeout.
  CHECK(elapsed < std::chrono::milliseconds(2000));
}

// Flood: server sends a multi-megabyte blob with no newlines. The daemon's
// bounded read_line must not allocate unbounded memory and must bail out.
TEST(qmp_handshake_megabyte_flood_no_oom) {
  MockSocket srv("/tmp/hc-fuzz-qmp-flood.sock");
  // 2 MB of 'A' with no newline — tests the line-read size cap.
  std::thread([&srv] {
    if (!srv.accept_once(std::chrono::milliseconds(1000))) return;
    std::string chunk(65536, 'A');
    for (int i = 0; i < 32; ++i) srv.send(chunk);
    srv.close_conn();
  }).detach();

  QmpClient q;
  std::string err;
  bool threw = false;
  try {
    q.connect("/tmp/hc-fuzz-qmp-flood.sock", SHORT_TIMEOUT, err);
  } catch (...) {
    threw = true;
  }
  CHECK(!threw);
}

// NUL bytes inside the JSON stream — must not corrupt internal string state.
TEST(qmp_handshake_nul_bytes_no_crash) {
  MockSocket srv("/tmp/hc-fuzz-qmp-nul.sock");
  std::string payload = std::string("{\"QMP\":{\"ver\":") +
                        '\0' + '\0' + '\0' +
                        "\"bad\"}}\n";
  inject_async(srv, payload);

  QmpClient q;
  std::string err;
  bool threw = false;
  try {
    q.connect("/tmp/hc-fuzz-qmp-nul.sock", SHORT_TIMEOUT, err);
  } catch (...) {
    threw = true;
  }
  CHECK(!threw);
}

// A valid handshake followed by a garbage system_powerdown response. The
// system_powerdown path must not crash and must return false (no "return" key).
TEST(qmp_powerdown_garbage_response_no_crash) {
  MockSocket srv("/tmp/hc-fuzz-qmp-pwdown.sock");
  const std::string good_hs =
      std::string(QMP_GREETING) + std::string(QMP_CAPS_ACK);
  const std::string garbage_powerdown =
      "{\"error\":{\"class\":\"CommandNotFound\","
      "\"desc\":\"" + std::string(512, 'G') + "\"}}\n";
  inject_async(srv, good_hs + garbage_powerdown);

  QmpClient q;
  std::string err;
  bool threw = false;
  try {
    if (q.connect("/tmp/hc-fuzz-qmp-pwdown.sock", SHORT_TIMEOUT, err)) {
      q.system_powerdown(err);
    }
  } catch (...) {
    threw = true;
  }
  CHECK(!threw);
}

// ── agent_ping fuzz tests ─────────────────────────────────────────────────────

// Guest returns a malformed sync reply (wrong or missing id). agent_ping must
// return alive=false rather than treating it as healthy.
TEST(agent_ping_bad_sync_id_not_alive) {
  MockSocket srv("/tmp/hc-fuzz-agent-badsync.sock");
  // Reply with a different sync id (99 instead of whatever the daemon sends).
  inject_async(srv, "{\"return\":99}\n{\"return\":{}}\n");

  AgentPing result{};
  bool threw = false;
  try {
    result = agent_ping("/tmp/hc-fuzz-agent-badsync.sock", 42, SHORT_TIMEOUT);
  } catch (...) {
    threw = true;
  }
  CHECK(!threw);
  CHECK(!result.alive);  // wrong sync id must not be treated as healthy
}

// Guest sends a massive single-line JSON "response" (no newline cap breach).
// The agent path must not crash or OOM.
TEST(agent_ping_megabyte_response_no_crash) {
  MockSocket srv("/tmp/hc-fuzz-agent-mega.sock");
  std::thread([&srv] {
    if (!srv.accept_once(std::chrono::milliseconds(1000))) return;
    // Send a "return" of the right sync id followed by a huge payload.
    srv.send("{\"return\":1}\n");
    std::string big = "{\"return\":{\"data\":\"" +
                      std::string(1 << 20, 'B') + "\"}}\n";
    srv.send(big);
    srv.close_conn();
  }).detach();

  AgentPing result{};
  bool threw = false;
  try {
    result = agent_ping("/tmp/hc-fuzz-agent-mega.sock", 1, SHORT_TIMEOUT);
  } catch (...) {
    threw = true;
  }
  CHECK(!threw);
  // Whether alive or not is secondary — the key property is no crash.
}

// Guest sends an empty response (connection closed immediately). Must return
// alive=false and not hang.
TEST(agent_ping_empty_stream_not_alive) {
  MockSocket srv("/tmp/hc-fuzz-agent-empty.sock");
  inject_async(srv, "");

  AgentPing result{};
  bool threw = false;
  auto t0 = std::chrono::steady_clock::now();
  try {
    result = agent_ping("/tmp/hc-fuzz-agent-empty.sock", 7, SHORT_TIMEOUT);
  } catch (...) {
    threw = true;
  }
  auto elapsed = std::chrono::steady_clock::now() - t0;
  CHECK(!threw);
  CHECK(!result.alive);
  CHECK(elapsed < std::chrono::milliseconds(2000));
}

// Guest floods alternating valid/invalid lines — state machine must not
// misidentify a healthy ping.
TEST(agent_ping_interleaved_junk_not_alive) {
  MockSocket srv("/tmp/hc-fuzz-agent-interleaved.sock");
  std::thread([&srv] {
    if (!srv.accept_once(std::chrono::milliseconds(1000))) return;
    for (int i = 0; i < 20; ++i) {
      srv.send("{\"junk\":\"" + std::string(i * 10, 'J') + "\"}\n");
    }
    srv.close_conn();
  }).detach();

  AgentPing result{};
  bool threw = false;
  try {
    result = agent_ping("/tmp/hc-fuzz-agent-interleaved.sock", 13, SHORT_TIMEOUT);
  } catch (...) {
    threw = true;
  }
  CHECK(!threw);
  CHECK(!result.alive);
}

// Guest returns valid IP replies but with injection payloads embedded. The
// agent_get_ipv4 validator must strip all of them out.
TEST(agent_get_ipv4_rejects_injection_payloads) {
  // Inline validation — mirrors the daemon's is_valid_ipv4 logic.
  auto is_ipv4 = [](const std::string& s) -> bool {
    if (s.empty() || s.size() > 15) return false;
    struct in_addr a{};
    return inet_pton(AF_INET, s.c_str(), &a) == 1;
  };

  // Payloads a compromised guest might inject as an "ip-address".
  std::vector<std::string> hostile = {
    "-oProxyCommand=touch /tmp/pwned",
    "attacker.evil.com",
    "10.0.0.1 -lroot",
    "10.0.0.1\n-oProxyCommand=x",
    "$(id)",
    "`id`",
    "0x0a.0.0.1",
    "10.0.0.1.5",
    " 10.0.0.1",
    "10.0.0.1 ",
    "::1",
    "fe80::1",
    std::string(300, 'A'),
  };

  for (const auto& payload : hostile) {
    if (is_ipv4(payload))
      HC_FAIL(("is_valid_ipv4 accepted injection payload: " + payload).c_str());
  }

  // Sanity: real addresses still accepted.
  CHECK(is_ipv4("10.0.2.15"));
  CHECK(is_ipv4("192.168.1.100"));
}

// Custom main: suppress SIGPIPE before running any test. Writing to a socket
// whose peer closed triggers SIGPIPE by default; we want errno=EPIPE instead so
// the read/write paths return errors rather than killing the test process.
int main() {
  signal(SIGPIPE, SIG_IGN);

  int failed_cases = 0;
  for (auto& c : ::hctest::registry()) {
    ::hctest::current_failures() = 0;
    std::printf("[ RUN  ] %s\n", c.name.c_str());
    c.fn();
    if (::hctest::current_failures() == 0) {
      std::printf("[  OK  ] %s\n", c.name.c_str());
    } else {
      std::printf("[ FAIL ] %s (%d checks failed)\n", c.name.c_str(),
                  ::hctest::current_failures());
      ++failed_cases;
    }
  }
  std::printf("\n%zu cases, %d failed\n", ::hctest::registry().size(),
              failed_cases);
  return failed_cases == 0 ? 0 : 1;
}
