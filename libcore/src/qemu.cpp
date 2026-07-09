#include "hypercore/core/qemu.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "hypercore/core/pidfile.hpp"

namespace hypercore::core {

namespace {

bool path_readable(const std::string& p) {
  return !p.empty() && access(p.c_str(), R_OK) == 0;
}

std::string mem_mib_arg(std::uint64_t bytes) {
  // QEMU -m takes MiB by default; round up to at least 1 MiB.
  std::uint64_t mib = bytes / (1024 * 1024);
  if (mib == 0) mib = 1;
  return std::to_string(mib);
}

}  // namespace

LaunchSpec spec_from_vm(const config::VmConfig& vm,
                        const std::string& runtime_dir) {
  LaunchSpec s;
  s.name = vm.name;
  s.cpus = vm.cpus;
  s.memory_bytes = vm.memory_bytes;
  if (vm.network) s.network = *vm.network;

  LaunchSpec::DiskBoot disk;
  disk.image = vm.image;
  if (vm.disk_type) disk.type = *vm.disk_type;
  s.disk = disk;

  if (vm.share) s.share = *vm.share;

  s.qmp_socket = runtime_dir + "/" + vm.name + ".qmp.sock";
  s.guest_agent_socket =
      vm.guest_agent ? *vm.guest_agent
                     : runtime_dir + "/" + vm.name + ".agent.sock";
  s.pidfile = pidfile_path(runtime_dir, vm.name);
  // Persist the console so `hypercore logs <name>` can read it.
  s.serial_log = runtime_dir + "/" + vm.name + ".console.log";

  // For user-mode networking, derive a stable SSH host-forward port from the
  // guest name so it survives daemon restarts / adoption (the same guest always
  // maps to the same 127.0.0.1:PORT). Range 20000-29999 keeps clear of the
  // ephemeral range and common services. Collisions between two guests hashing
  // equal are unlikely and, if they occur, the second QEMU simply fails to bind
  // that VM (reported as a launch failure) rather than silently misrouting.
  if (s.network == config::Network::User) {
    std::uint32_t h = 2166136261u;  // FNV-1a
    for (char c : vm.name) { h ^= static_cast<unsigned char>(c); h *= 16777619u; }
    s.ssh_hostfwd_port = 20000 + static_cast<int>(h % 10000);
  }
  return s;
}

std::string check_paths(const LaunchSpec& spec) {
  if (spec.disk) {
    if (!path_readable(spec.disk->image))
      return "disk image not found or unreadable: " + spec.disk->image;
  }
  if (spec.direct) {
    if (!path_readable(spec.direct->kernel))
      return "kernel not found or unreadable: " + spec.direct->kernel;
    if (!path_readable(spec.direct->initramfs))
      return "initramfs not found or unreadable: " + spec.direct->initramfs;
  }
  if (!spec.disk && !spec.direct)
    return "launch spec has no boot source (neither disk nor direct kernel)";
  if (spec.share) {
    struct stat st{};
    if (spec.share->host_path.empty() ||
        stat(spec.share->host_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
      return "virtiofs host_path missing or not a directory: " +
             spec.share->host_path;
  }
  return "";
}

std::vector<std::string> build_argv(const LaunchSpec& spec) {
  std::vector<std::string> a;
  auto add = [&](std::string s) { a.push_back(std::move(s)); };

  add(spec.qemu_binary);
  // Identify our process so PID adoption can confirm ownership via cmdline.
  add("-name");
  add(guest_marker(spec.name));

  if (spec.enable_kvm) {
    add("-enable-kvm");
    add("-cpu");
    add("host");
  }
  add("-m");
  add(mem_mib_arg(spec.memory_bytes));
  add("-smp");
  add(std::to_string(spec.cpus.empty() ? 1 : spec.cpus.size()));

  add("-display");
  add("none");
  add("-no-reboot");

  // Serial: to a file for tests, else a null device (headless daemon).
  add("-serial");
  add(spec.serial_log.empty() ? "null" : ("file:" + spec.serial_log));

  // Boot source.
  if (spec.direct) {
    add("-kernel");
    add(spec.direct->kernel);
    add("-initrd");
    add(spec.direct->initramfs);
    add("-append");
    add(spec.direct->cmdline);
  } else if (spec.disk) {
    add("-drive");
    std::string fmt =
        spec.disk->type == config::DiskType::Raw ? "raw" : "qcow2";
    add("file=" + spec.disk->image + ",format=" + fmt + ",if=virtio");
  }

  // Networking.
  switch (spec.network) {
    case config::Network::User: {
      // SLIRP user networking + an SSH host-forward so `hypercore ssh <name>`
      // can reach the guest at 127.0.0.1:<port> (guest port 22).
      std::string netdev = "user,id=n0";
      if (spec.ssh_hostfwd_port > 0)
        netdev += ",hostfwd=tcp:127.0.0.1:" +
                  std::to_string(spec.ssh_hostfwd_port) + "-:22";
      add("-netdev"); add(netdev);
      add("-device"); add("virtio-net-pci,netdev=n0");
      break;
    }
    case config::Network::Bridge:
      add("-netdev"); add("bridge,id=n0,br=br0");
      add("-device"); add("virtio-net-pci,netdev=n0");
      break;
    case config::Network::Virtiofs:
      // virtiofs needs a vhost-user-fs device + shared memory; the share is
      // wired here. (Networking, if also needed, is layered by config.)
      if (spec.share) {
        add("-chardev");
        add("socket,id=vfs0,path=" + spec.qmp_socket + ".vfs");
        add("-device");
        add("vhost-user-fs-pci,chardev=vfs0,tag=" + spec.share->tag);
      }
      break;
  }

  // QMP control channel (server; daemon connects) — requirement #3c.
  add("-qmp");
  add("unix:" + spec.qmp_socket + ",server=on,wait=off");

  // Guest-agent virtio-serial channel — requirement #4.
  if (!spec.guest_agent_socket.empty()) {
    add("-chardev");
    add("socket,id=ga0,path=" + spec.guest_agent_socket +
        ",server=on,wait=off");
    add("-device");
    add("virtio-serial");
    add("-device");
    add("virtserialport,chardev=ga0,name=org.qemu.guest_agent.0");
  }
  return a;
}

LaunchResult launch(const LaunchSpec& spec, const std::string& runtime_dir) {
  LaunchResult r;
  r.argv = build_argv(spec);

  // #3e: verify paths BEFORE spawning anything.
  std::string path_err = check_paths(spec);
  if (!path_err.empty()) {
    r.error = path_err;
    return r;
  }

  // Build argv as C strings.
  std::vector<char*> cargv;
  cargv.reserve(r.argv.size() + 1);
  for (auto& s : r.argv) cargv.push_back(const_cast<char*>(s.c_str()));
  cargv.push_back(nullptr);

  pid_t pid = fork();
  if (pid < 0) {
    r.error = std::string("fork: ") + std::strerror(errno);
    return r;
  }
  if (pid == 0) {
    // --- child ---
    // New session so the guest isn't killed by the daemon's controlling TTY.
    setsid();
    // Silence stdio (serial already routed via -serial).
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      // Keep stderr for early QEMU errors only if no serial_log; else quiet.
      dup2(devnull, STDOUT_FILENO);
      if (!spec.serial_log.empty()) dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO) close(devnull);
    }
    execvp(cargv[0], cargv.data());
    // If exec returns, it failed. Use _exit to avoid running atexit handlers.
    _exit(127);
  }

  // --- parent ---
  // fork() returned the pid immediately, but the child has not yet finished
  // exec'ing QEMU. Confirm the exec before reporting success: poll until the
  // process cmdline shows our QEMU (identified by the -name marker), or the
  // child exits early (exec failure). This closes a race where callers would
  // otherwise inspect /proc/<pid>/cmdline (e.g. adoption identity) before QEMU
  // replaced the forked image.
  const std::string marker = guest_marker(spec.name);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool exec_ok = false;
  while (std::chrono::steady_clock::now() < deadline) {
    int status = 0;
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) {
      // Child exited before we saw QEMU: exec failed (127) or immediate crash.
      r.error = "QEMU exited immediately after spawn (exec failed?)";
      return r;
    }
    if (pid_cmdline_contains(pid, marker)) {
      exec_ok = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!exec_ok) {
    r.error = "timed out waiting for QEMU to start (pid " +
              std::to_string(pid) + ")";
    return r;
  }

  std::string err;
  if (!write_pidfile(runtime_dir, spec.name, pid, err)) {
    // We spawned but couldn't record it; report so the caller can decide.
    r.error = "spawned pid " + std::to_string(pid) +
              " but failed to write pidfile: " + err;
    r.pid = pid;
    return r;
  }
  r.ok = true;
  r.pid = pid;
  return r;
}

}  // namespace hypercore::core
