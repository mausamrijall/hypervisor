// QEMU process launch (requirement #3a) + launch-time path checks (#3e).
//
// The daemon builds a LaunchSpec from a Phase-2 VmConfig and hands it here.
// LaunchSpec also supports a direct kernel+initramfs boot source that no real
// VmConfig uses — it exists so the integration tests can boot the throwaway
// busybox guest through the exact same launch/pin/track code path as a real
// disk-backed guest, without adding kernel/initramfs fields to the user-facing
// schema.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hypercore/config/types.hpp"

namespace hypercore::core {

struct LaunchSpec {
  std::string name;
  std::vector<std::int64_t> cpus;
  std::uint64_t memory_bytes = 0;
  config::Network network = config::Network::User;

  // --- boot source: exactly one of these ---
  struct DiskBoot {
    std::string image;
    config::DiskType type = config::DiskType::Qcow2;
  };
  struct DirectKernelBoot {  // test-only path
    std::string kernel;
    std::string initramfs;
    std::string cmdline = "console=ttyS0 quiet panic=-1";
  };
  std::optional<DiskBoot> disk;
  std::optional<DirectKernelBoot> direct;

  // Optional virtiofs share (host_path existence is checked at launch).
  std::optional<config::ShareConfig> share;

  // --- control/runtime endpoints (absolute paths) ---
  std::string qmp_socket;          // QEMU creates this (server); daemon dials it
  std::string guest_agent_socket;  // empty => no agent channel
  std::string pidfile;
  std::string serial_log;          // empty => inherit; tests point at a file

  // For user-mode networking, the host port forwarded to the guest's SSH port
  // (22). `ssh <name>` connects to 127.0.0.1:ssh_hostfwd_port. 0 => none.
  // Chosen per-guest by the daemon (see spec_from_vm). Ignored for bridge,
  // where the guest gets a routable IP the daemon learns via the guest agent.
  int ssh_hostfwd_port = 0;

  std::string qemu_binary = "qemu-system-x86_64";
  bool enable_kvm = true;

  // Privilege separation: when the daemon runs as root, each QEMU child drops
  // to this uid/gid (via setgroups/setgid/setuid) in the forked child BEFORE
  // execvp, so a guest/QEMU compromise does not hand an attacker root over host
  // RAM. 0 means "do not drop" — used only when the daemon itself is already
  // unprivileged (dev/test), where dropping is impossible and unnecessary.
  // Populated by spec_from_vm from SupervisorOptions.
  unsigned run_as_uid = 0;
  unsigned run_as_gid = 0;
  // Supplementary group to retain after the drop (typically the `kvm` group, so
  // the unprivileged QEMU can still open /dev/kvm). 0 => keep none.
  unsigned keep_gid = 0;
};

// Build a LaunchSpec for a real guest from its validated VmConfig. Endpoints
// are placed under runtime_dir (e.g. /run/hypercore).
LaunchSpec spec_from_vm(const config::VmConfig& vm,
                        const std::string& runtime_dir);

struct LaunchResult {
  bool ok = false;
  int pid = 0;
  std::string error;         // human-readable; set when ok == false
  std::vector<std::string> argv;  // the exact argv used (for logging/tests)
};

// Check that every path the spec references (disk image, kernel, initramfs,
// virtiofs host_path) exists and is readable. Returns "" if all good, else a
// message naming the first missing path. Requirement #3e — called before spawn
// so a missing image fails THIS guest cleanly without spawning anything.
std::string check_paths(const LaunchSpec& spec);

// Construct the QEMU argv from a spec (no spawning). Pure and unit-testable.
std::vector<std::string> build_argv(const LaunchSpec& spec);

// Launch QEMU: path-check, fork/exec, write the pid file. On any pre-spawn
// failure (bad paths, fork error) returns ok=false with an error and spawns
// nothing. The daemon treats that as a single-VM launch failure, not fatal.
LaunchResult launch(const LaunchSpec& spec, const std::string& runtime_dir);

}  // namespace hypercore::core
