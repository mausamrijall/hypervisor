// Host capability detection.
//
// Requirement #1 of Phase 3: the test harness (and the daemon) must reliably
// detect whether real hardware-accelerated QEMU is available on this machine,
// and gate the real-QEMU integration tests on that — skipping cleanly (never
// failing) where it is absent, e.g. GitHub Actions runners without /dev/kvm.
//
// One detector, shared by the daemon and the tests, so what CI gates on and
// what the daemon reports can never drift apart.
#pragma once

#include <string>

namespace hypercore::core {

struct Capabilities {
  bool kvm = false;             // /dev/kvm present and read+write to us
  bool qemu = false;            // a usable qemu-system binary was found
  std::string qemu_path;        // resolved absolute path, if found
  std::string qemu_version;     // first line of `qemu-... --version`, if found
  std::string kvm_reason;       // why kvm is unavailable (for diagnostics)

  // True only when BOTH a runnable QEMU and usable KVM are present — the
  // precondition for the real-QEMU integration tests.
  bool can_run_guests() const { return kvm && qemu; }
};

// Detect capabilities. Honors two escape hatches so tests can force the
// "no acceleration" path without touching hardware:
//   - env HYPERCORE_FORCE_NO_KVM=1  -> report kvm=false (reason: forced)
//   - env HYPERCORE_QEMU=<path>     -> probe that binary instead of $PATH
// `qemu_binary` overrides the binary name to search for (default
// "qemu-system-x86_64"); the env var takes precedence over it.
Capabilities detect_capabilities(
    const std::string& qemu_binary = "qemu-system-x86_64");

}  // namespace hypercore::core
