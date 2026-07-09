#include "client.hpp"

#include <chrono>
#include <string>

#include "hypercore/core/usock.hpp"

namespace hypercore::cli {

Reply Client::request(const std::string& command, const std::string& arg) {
  Reply r;
  hypercore::core::UnixClient conn;
  std::string err;
  if (!conn.connect(socket_path_, std::chrono::milliseconds(2000), err)) {
    r.transport_error =
        "cannot connect to daemon at " + socket_path_ + ": " + err +
        "\n(is hypercored running? try --socket <path>)";
    return r;
  }

  std::string line = std::to_string(kProtoVersion) + " " + command;
  if (!arg.empty()) line += " " + arg;
  line += "\n";
  if (!conn.write_all(line, err)) {
    r.transport_error = "write failed: " + err;
    return r;
  }

  auto resp = conn.read_line(std::chrono::milliseconds(5000), err);
  if (!resp) {
    r.transport_error = err.empty() ? "no response (timeout)" : err;
    return r;
  }

  try {
    r.json = nlohmann::json::parse(*resp);
  } catch (const std::exception& e) {
    r.transport_error = std::string("malformed response: ") + e.what();
    return r;
  }
  r.transport_ok = true;
  return r;
}

}  // namespace hypercore::cli
