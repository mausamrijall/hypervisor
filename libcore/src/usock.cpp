#include "hypercore/core/usock.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <thread>

namespace hypercore::core {

using clock = std::chrono::steady_clock;

UnixClient::~UnixClient() { close(); }

UnixClient::UnixClient(UnixClient&& o) noexcept
    : fd_(o.fd_), buf_(std::move(o.buf_)) {
  o.fd_ = -1;
}
UnixClient& UnixClient::operator=(UnixClient&& o) noexcept {
  if (this != &o) {
    close();
    fd_ = o.fd_;
    buf_ = std::move(o.buf_);
    o.fd_ = -1;
  }
  return *this;
}

void UnixClient::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  buf_.clear();
}

bool UnixClient::connect(const std::string& path,
                         std::chrono::milliseconds timeout, std::string& err) {
  if (path.size() >= sizeof(sockaddr_un::sun_path)) {
    err = "socket path too long: " + path;
    return false;
  }
  auto deadline = clock::now() + timeout;
  for (;;) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      err = std::string("socket: ") + std::strerror(errno);
      return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      fd_ = fd;
      return true;
    }
    ::close(fd);
    if (clock::now() >= deadline) {
      err = "connect timeout to " + path + " (" + std::strerror(errno) + ")";
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
}

bool UnixClient::write_all(const std::string& data, std::string& err) {
  if (fd_ < 0) {
    err = "not connected";
    return false;
  }
  std::size_t off = 0;
  while (off < data.size()) {
    ssize_t n = ::write(fd_, data.data() + off, data.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      err = std::string("write: ") + std::strerror(errno);
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

std::optional<std::string> UnixClient::read_line(
    std::chrono::milliseconds timeout, std::string& err) {
  err.clear();
  if (fd_ < 0) {
    err = "not connected";
    return std::nullopt;
  }
  auto deadline = clock::now() + timeout;
  for (;;) {
    // Serve a complete line already sitting in the buffer.
    auto nl = buf_.find('\n');
    if (nl != std::string::npos) {
      std::string line = buf_.substr(0, nl);
      buf_.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      return line;
    }
    auto now = clock::now();
    if (now >= deadline) return std::nullopt;  // timeout, err stays empty
    int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count());
    pollfd pfd{fd_, POLLIN, 0};
    int pr = ::poll(&pfd, 1, ms);
    if (pr < 0) {
      if (errno == EINTR) continue;
      err = std::string("poll: ") + std::strerror(errno);
      return std::nullopt;
    }
    if (pr == 0) return std::nullopt;  // timeout
    char chunk[1024];
    ssize_t n = ::read(fd_, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) continue;
      err = std::string("read: ") + std::strerror(errno);
      return std::nullopt;
    }
    if (n == 0) {
      err = "eof";
      return std::nullopt;
    }
    buf_.append(chunk, static_cast<std::size_t>(n));
  }
}

}  // namespace hypercore::core
