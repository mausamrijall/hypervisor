// Minimal blocking Unix-domain-socket client helpers, shared by the QMP client,
// the guest-agent health probe, and (server side) the control socket. Kept tiny
// and dependency-free: connect, send-all, read-until-newline, with deadlines so
// a wedged guest can never hang the daemon.
#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace hypercore::core {

class UnixClient {
 public:
  UnixClient() = default;
  ~UnixClient();
  UnixClient(const UnixClient&) = delete;
  UnixClient& operator=(const UnixClient&) = delete;
  UnixClient(UnixClient&&) noexcept;
  UnixClient& operator=(UnixClient&&) noexcept;

  // Connect to a Unix socket path. Retries until `deadline` because QEMU may
  // create the socket a beat after spawn. Returns false with `err` set on
  // failure/timeout.
  bool connect(const std::string& path, std::chrono::milliseconds timeout,
               std::string& err);

  bool is_open() const { return fd_ >= 0; }
  void close();

  // Write all bytes. false on error.
  bool write_all(const std::string& data, std::string& err);

  // Read a single '\n'-terminated line (newline stripped). nullopt on
  // timeout/EOF/error; `err` distinguishes timeout ("") from hard error.
  std::optional<std::string> read_line(std::chrono::milliseconds timeout,
                                       std::string& err);

 private:
  int fd_ = -1;
  std::string buf_;  // holds bytes read past a line boundary
};

}  // namespace hypercore::core
