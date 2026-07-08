// PID files: the persistent record that lets a restarted daemon find guests it
// launched in a previous life (requirement #3d, adoption). Format is one line:
//   <pid>
// The daemon also stamps each QEMU with `-name hypercore-<vm>` so adoption can
// confirm a live PID really is *our* guest and not a recycled PID belonging to
// some unrelated process.
#pragma once

#include <optional>
#include <string>

namespace hypercore::core {

// Path helpers keep the runtime-dir layout in one place.
std::string pidfile_path(const std::string& runtime_dir, const std::string& vm);

// Write <pid> to the VM's pid file (atomically via temp + rename).
bool write_pidfile(const std::string& runtime_dir, const std::string& vm,
                   int pid, std::string& err);

// Read a pid from the VM's pid file. nullopt if absent/unparseable.
std::optional<int> read_pidfile(const std::string& runtime_dir,
                                const std::string& vm);

void remove_pidfile(const std::string& runtime_dir, const std::string& vm);

// Sidecar "meta" file storing the launch-time fingerprint of a running guest.
// This lets a *separate* process (e.g. `hypercored --reconcile --dry-run`)
// adopt a guest and still detect config drift — without it, adoption has no
// record of what config the guest was actually started with, so reconcile
// could never report a needed restart across a fresh process.
bool write_meta(const std::string& runtime_dir, const std::string& vm,
                const std::string& fingerprint, std::string& err);
std::optional<std::string> read_meta(const std::string& runtime_dir,
                                     const std::string& vm);
void remove_meta(const std::string& runtime_dir, const std::string& vm);

// Is `pid` currently alive (kill(pid,0))? Zombies count as dead.
bool pid_alive(int pid);

// Reap any exited child processes (WNOHANG) so they don't linger as zombies.
// Call periodically (e.g. each health tick) and after stopping a guest.
void reap_children();

// Does /proc/<pid>/cmdline contain `marker`? Used to confirm an adopted PID is
// really our QEMU for this VM (guards against PID reuse).
bool pid_cmdline_contains(int pid, const std::string& marker);

// The marker we embed in QEMU's -name and look for on adoption.
std::string guest_marker(const std::string& vm);

}  // namespace hypercore::core
