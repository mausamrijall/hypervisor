// hostcaps — prints detected host capabilities as a single JSON line.
//
// Two uses:
//   1. The integration-test harness runs this and gates real-QEMU tests on
//      `can_run_guests` (so the gate and the daemon share ONE detector).
//   2. Humans debugging a box: `hostcaps` tells you at a glance whether KVM and
//      QEMU are usable and, if not, why.
//
// Exit code: 0 if guests can run (kvm && qemu), 1 otherwise — so shell/CTest
// gating can branch on the exit status without parsing the JSON.

#include <cstdio>

#include "hypercore/core/capabilities.hpp"

int main() {
  hypercore::core::Capabilities c = hypercore::core::detect_capabilities();

  auto b = [](bool v) { return v ? "true" : "false"; };
  // Hand-rolled JSON: this is a leaf tool with a fixed, tiny shape.
  std::printf(
      "{\"kvm\":%s,\"qemu\":%s,\"can_run_guests\":%s,"
      "\"qemu_path\":\"%s\",\"qemu_version\":\"%s\",\"kvm_reason\":\"%s\"}\n",
      b(c.kvm), b(c.qemu), b(c.can_run_guests()), c.qemu_path.c_str(),
      c.qemu_version.c_str(), c.kvm_reason.c_str());

  return c.can_run_guests() ? 0 : 1;
}
