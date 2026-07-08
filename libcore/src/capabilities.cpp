// Host capability detection. See capabilities.hpp.

#include "hypercore/core/capabilities.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <unistd.h>

namespace hypercore::core {

namespace {

// Resolve a program name against $PATH, or accept an absolute path as-is.
// Returns the usable absolute path or "" if not found/executable.
std::string resolve_executable(const std::string& name_or_path) {
  auto is_exec = [](const std::string& p) {
    return access(p.c_str(), X_OK) == 0;
  };
  if (name_or_path.find('/') != std::string::npos)
    return is_exec(name_or_path) ? name_or_path : "";

  const char* path_env = std::getenv("PATH");
  if (!path_env) return "";
  std::string paths = path_env;
  std::size_t start = 0;
  while (start <= paths.size()) {
    std::size_t colon = paths.find(':', start);
    std::string dir = paths.substr(
        start, colon == std::string::npos ? std::string::npos : colon - start);
    if (!dir.empty()) {
      std::string cand = dir + "/" + name_or_path;
      if (is_exec(cand)) return cand;
    }
    if (colon == std::string::npos) break;
    start = colon + 1;
  }
  return "";
}

// Run `<path> --version` and capture the first line. Best-effort; empty on
// failure. Uses popen with a fixed argv-free command built from a resolved
// absolute path (not user input), so shell injection is not a concern here.
std::string probe_version(const std::string& path) {
  std::string cmd = "'" + path + "' --version 2>/dev/null";
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) return "";
  std::array<char, 256> buf{};
  std::string line;
  if (std::fgets(buf.data(), static_cast<int>(buf.size()), p))
    line = buf.data();
  pclose(p);
  if (!line.empty() && line.back() == '\n') line.pop_back();
  return line;
}

}  // namespace

Capabilities detect_capabilities(const std::string& qemu_binary) {
  Capabilities caps;

  // --- QEMU (allow env override of the binary to probe) ---
  const char* qemu_env = std::getenv("HYPERCORE_QEMU");
  std::string want = qemu_env ? std::string(qemu_env) : qemu_binary;
  caps.qemu_path = resolve_executable(want);
  caps.qemu = !caps.qemu_path.empty();
  if (caps.qemu) caps.qemu_version = probe_version(caps.qemu_path);

  // --- KVM ---
  if (std::getenv("HYPERCORE_FORCE_NO_KVM")) {
    caps.kvm = false;
    caps.kvm_reason = "forced off via HYPERCORE_FORCE_NO_KVM";
  } else if (access("/dev/kvm", F_OK) != 0) {
    caps.kvm = false;
    caps.kvm_reason = "/dev/kvm does not exist";
  } else if (access("/dev/kvm", R_OK | W_OK) != 0) {
    caps.kvm = false;
    caps.kvm_reason = "/dev/kvm exists but is not read+writable by this user";
  } else {
    caps.kvm = true;
    caps.kvm_reason = "ok";
  }

  return caps;
}

}  // namespace hypercore::core
