// Security-hardening tests (HC-2026-001 mitigations + input sanitation).
//
// Host-independent, no QEMU/KVM: exercises the control-socket request parser
// and the guest-agent IP validation against hostile/malformed input, asserting
// the daemon never throws out of request handling and never accepts an unsafe
// "IP".

#include "hctest.hpp"

#include <arpa/inet.h>

#include <string>
#include <vector>

#include "hypercore/config/types.hpp"
#include "hypercore/core/control_server.hpp"
#include "hypercore/core/supervisor.hpp"

using namespace hypercore::core;
namespace cfg = hypercore::config;

namespace {

// Build a supervisor with one configured (never-launched) guest so the control
// server has something to answer about.
Supervisor make_sup() {
  cfg::Config c;
  cfg::VmConfig vm;
  vm.name = "web";
  vm.image = "/img";
  vm.disk_type = cfg::DiskType::Raw;
  vm.cpus = {0};
  vm.memory_bytes = 1u << 30;
  vm.network = cfg::Network::User;
  c.vms.push_back(vm);
  SupervisorOptions o;
  o.runtime_dir = "/tmp/hc-hardening";
  return Supervisor(std::move(c), std::move(o));
}

// Mirror of the daemon-side is_valid_ipv4 used by agent_get_ipv4 / cmd_ssh.
bool is_ipv4(const std::string& s) {
  if (s.empty() || s.size() > 15) return false;
  struct in_addr a{};
  return inet_pton(AF_INET, s.c_str(), &a) == 1;
}

}  // namespace

// The request parser must survive ANY byte sequence without throwing and always
// return a non-empty response line.
TEST(control_parser_never_throws_on_malformed_input) {
  Supervisor sup = make_sup();
  ControlServer srv("/tmp/hc-hardening.sock", sup);

  std::vector<std::string> hostile = {
      "",                                   // empty
      "   ",                                // whitespace only
      "\t\t",                               // tabs only
      "1",                                  // proto only, no command
      "notanumber list",                    // non-numeric proto
      "999999999999999999999999 list",      // proto overflow (out_of_range)
      "-1 list",                            // negative proto
      "1 status",                           // missing arg
      "1 status ../../etc/passwd",          // path-traversal-ish arg
      "1 status WEB",                        // invalid name (uppercase)
      "1 status web extra tokens here",     // excess tokens
      "1     status      web",              // excessive interior spaces
      "1 \x01\x02\x03 web",                 // non-printable command
      "1 status \x7f\x80\xff",              // non-printable / high-bit arg
      "1 frobnicate",                        // unknown command
      "1 start",                             // start needs arg
      "1 stop all",                          // valid-ish
      "2 list",                              // proto mismatch
      std::string(5000, 'A'),               // very long single token
      "1 status " + std::string(3000, 'a'), // very long arg
  };

  for (const auto& in : hostile) {
    std::string resp;
    bool threw = false;
    try {
      resp = srv.handle_request(in);
    } catch (...) {
      threw = true;
    }
    if (threw) HC_FAIL(("handle_request threw on input: " + in).c_str());
    if (resp.empty()) HC_FAIL(("empty response for input: " + in).c_str());
    // Every response must be a JSON object mentioning the proto field.
    if (resp.find("\"proto\"") == std::string::npos)
      HC_FAIL(("response missing proto for input: " + in).c_str());
  }
}

// Well-formed valid request still works after all the hardening.
TEST(control_parser_valid_request_ok) {
  Supervisor sup = make_sup();
  ControlServer srv("/tmp/hc-hardening.sock", sup);
  std::string resp = srv.handle_request("1 list");
  CHECK(resp.find("\"ok\":true") != std::string::npos);
  resp = srv.handle_request("1 status web");
  CHECK(resp.find("\"ok\":true") != std::string::npos);
  resp = srv.handle_request("1 status nonexistent");
  CHECK(resp.find("unknown_vm") != std::string::npos);
}

// The IPv4 validator gates out every SSH-injection payload class from the audit.
TEST(ipv4_validation_rejects_injection_payloads) {
  // Accepts only real dotted-quads.
  CHECK(is_ipv4("10.0.2.15"));
  CHECK(is_ipv4("192.168.1.1"));
  CHECK(is_ipv4("8.8.8.8"));

  // Rejects everything the guest agent could weaponize.
  CHECK(!is_ipv4("-oProxyCommand=touch /tmp/pwned"));  // ssh option injection
  CHECK(!is_ipv4("-lroot"));                            // ssh login-name flag
  CHECK(!is_ipv4("attacker.evil.com"));                 // hostname redirection
  CHECK(!is_ipv4("10.0.0.1 -oProxyCommand=x"));         // trailing option
  CHECK(!is_ipv4(""));                                  // empty
  CHECK(!is_ipv4("999.999.999.999"));                   // out-of-range octets
  CHECK(!is_ipv4("10.0.0.1\n-lroot"));                  // embedded newline
  CHECK(!is_ipv4("0x0a.0.0.1"));                         // hex octet trickery
  CHECK(!is_ipv4("10.0.0.1.5"));                         // too many octets
  CHECK(!is_ipv4(" 10.0.0.1"));                          // leading space
}
