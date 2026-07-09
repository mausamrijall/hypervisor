// Control-socket client for the hypercore CLI.
//
// Speaks the docs/protocol.md wire format: send one line "<proto> <cmd> [arg]",
// read one ndjson response line. Unlike the daemon (which never parses JSON on
// its privileged input path), the CLI DOES parse the daemon's responses — with
// the vendored nlohmann/json — because it must render structured status. This
// is the safe direction: the client trusts the daemon it connected to.
#pragma once

#include <string>

#include "json.hpp"

namespace hypercore::cli {

// Client-side view of the protocol version it speaks. Must match the daemon's
// kProtoVersion or the daemon replies proto_mismatch.
inline constexpr int kProtoVersion = 1;

struct Reply {
  bool transport_ok = false;   // did we connect + read a well-formed line?
  std::string transport_error; // set when transport_ok == false
  nlohmann::json json;         // parsed response (valid iff transport_ok)

  // Convenience accessors over the protocol envelope.
  bool ok() const {
    return transport_ok && json.value("ok", false);
  }
  std::string error_code() const {
    if (!transport_ok) return "transport";
    if (json.contains("error")) return json["error"].value("code", "");
    return "";
  }
  std::string error_message() const {
    if (!transport_ok) return transport_error;
    if (json.contains("error")) return json["error"].value("message", "");
    return "";
  }
};

class Client {
 public:
  explicit Client(std::string socket_path) : socket_path_(std::move(socket_path)) {}

  // Send one request line (without proto prefix / newline; both are added) and
  // return the parsed reply. `command` + optional `arg` form the request.
  Reply request(const std::string& command, const std::string& arg = "");

  const std::string& socket_path() const { return socket_path_; }

 private:
  std::string socket_path_;
};

}  // namespace hypercore::cli
