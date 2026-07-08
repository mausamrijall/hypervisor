#include "hypercore/core/pidfile.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

namespace hypercore::core {

std::string pidfile_path(const std::string& runtime_dir, const std::string& vm) {
  return runtime_dir + "/" + vm + ".pid";
}

std::string guest_marker(const std::string& vm) {
  return "hypercore-" + vm;
}

bool write_pidfile(const std::string& runtime_dir, const std::string& vm,
                   int pid, std::string& err) {
  std::string path = pidfile_path(runtime_dir, vm);
  std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) {
      err = "cannot open " + tmp + " for write";
      return false;
    }
    f << pid << "\n";
    if (!f) {
      err = "write failed to " + tmp;
      return false;
    }
  }
  // Atomic replace so a reader never sees a half-written file.
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    err = "rename " + tmp + " -> " + path + " failed";
    return false;
  }
  return true;
}

std::optional<int> read_pidfile(const std::string& runtime_dir,
                                const std::string& vm) {
  std::ifstream f(pidfile_path(runtime_dir, vm));
  if (!f) return std::nullopt;
  int pid = 0;
  if (!(f >> pid) || pid <= 0) return std::nullopt;
  return pid;
}

void remove_pidfile(const std::string& runtime_dir, const std::string& vm) {
  std::remove(pidfile_path(runtime_dir, vm).c_str());
}

namespace {
std::string meta_path(const std::string& runtime_dir, const std::string& vm) {
  return runtime_dir + "/" + vm + ".meta";
}
}  // namespace

bool write_meta(const std::string& runtime_dir, const std::string& vm,
                const std::string& fingerprint, std::string& err) {
  std::string path = meta_path(runtime_dir, vm);
  std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) { err = "cannot open " + tmp; return false; }
    f << fingerprint << "\n";
    if (!f) { err = "write failed to " + tmp; return false; }
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    err = "rename meta failed";
    return false;
  }
  return true;
}

std::optional<std::string> read_meta(const std::string& runtime_dir,
                                     const std::string& vm) {
  std::ifstream f(meta_path(runtime_dir, vm));
  if (!f) return std::nullopt;
  std::string line;
  std::getline(f, line);
  if (line.empty()) return std::nullopt;
  return line;
}

void remove_meta(const std::string& runtime_dir, const std::string& vm) {
  std::remove(meta_path(runtime_dir, vm).c_str());
}

bool pid_alive(int pid) {
  if (pid <= 0) return false;
  // kill(pid, 0): no signal sent, just an existence/permission probe.
  if (kill(pid, 0) != 0) return errno == EPERM;  // ESRCH => gone

  // The process table entry exists — but a zombie (exited child not yet
  // reaped) must count as DEAD, or shutdown/health logic would wait forever on
  // a process that has actually terminated. Read its state from /proc.
  std::ostringstream path;
  path << "/proc/" << pid << "/stat";
  std::ifstream f(path.str());
  if (!f) return true;  // no /proc entry to contradict; assume alive
  std::string data((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
  // Format: "pid (comm) STATE ...". comm may contain spaces/parens, so scan
  // from the last ')' and take the next non-space char as the state.
  auto rp = data.rfind(')');
  if (rp != std::string::npos && rp + 2 < data.size()) {
    char state = data[rp + 2];
    if (state == 'Z') return false;  // zombie => effectively dead
  }
  return true;
}

void reap_children() {
  // Reap any of OUR exited children so they don't linger as zombies (which
  // would otherwise defeat the zombie check above). Non-child pids (adopted
  // guests from a previous daemon life) yield ECHILD and are simply ignored.
  int status = 0;
  while (waitpid(-1, &status, WNOHANG) > 0) {
  }
}

bool pid_cmdline_contains(int pid, const std::string& marker) {
  std::ostringstream path;
  path << "/proc/" << pid << "/cmdline";
  std::ifstream f(path.str(), std::ios::binary);
  if (!f) return false;
  // cmdline is NUL-separated; read it all and scan.
  std::string data((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
  for (char& c : data)
    if (c == '\0') c = ' ';
  return data.find(marker) != std::string::npos;
}

}  // namespace hypercore::core
