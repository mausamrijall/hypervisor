// Integration tests for the Phase 3 runtime engine, run against a REAL QEMU
// guest (the throwaway busybox initramfs) — no mocks. Covers requirements
// #3a-#3e and #4 end to end.
//
// GATING: if the host lacks /dev/kvm or qemu-system, this program exits 77
// (the CTest SKIP_RETURN_CODE we configure), so CI runners without KVM SKIP
// these cleanly rather than failing. The reconcile diff logic has its own
// host-independent unit test (reconcile_test) that always runs.
//
// This test drives libcore directly (launch/pin/stop/health/adopt), using a
// direct-kernel LaunchSpec so the busybox guest exercises the exact same code
// path a real disk-backed guest would.

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "hypercore/config/types.hpp"
#include "hypercore/core/affinity.hpp"
#include "hypercore/core/capabilities.hpp"
#include "hypercore/core/guest_agent.hpp"
#include "hypercore/core/pidfile.hpp"
#include "hypercore/core/qemu.hpp"
#include "hypercore/core/qmp.hpp"
#include "hypercore/core/supervisor.hpp"

namespace core = hypercore::core;
namespace cfg = hypercore::config;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;
#define REQUIRE(cond, msg)                                            \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::fprintf(stderr, "  FAIL: %s (%s:%d)\n", msg, __FILE__,    \
                   __LINE__);                                        \
      ++g_failures;                                                  \
    } else {                                                        \
      std::fprintf(stderr, "  ok:   %s\n", msg);                    \
    }                                                               \
  } while (0)

constexpr int kSkip = 77;

std::string g_kernel;
std::string g_initramfs;
std::string g_runtime;  // per-run scratch dir

// Build a direct-kernel spec for the busybox guest pinned to `cores`.
core::LaunchSpec test_spec(const std::string& name,
                           std::vector<std::int64_t> cores,
                           bool with_agent = true) {
  core::LaunchSpec s;
  s.name = name;
  s.cpus = std::move(cores);
  s.memory_bytes = 128ull << 20;  // 128 MiB
  s.network = cfg::Network::User;
  core::LaunchSpec::DirectKernelBoot dk;
  dk.kernel = g_kernel;
  dk.initramfs = g_initramfs;
  s.direct = dk;
  s.qmp_socket = g_runtime + "/" + name + ".qmp.sock";
  if (with_agent) s.guest_agent_socket = g_runtime + "/" + name + ".ga.sock";
  s.pidfile = core::pidfile_path(g_runtime, name);
  s.serial_log = g_runtime + "/" + name + ".console.log";
  return s;
}

// Wait until the guest agent answers, or timeout.
bool wait_agent_ready(const std::string& sock, std::chrono::seconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  int id = 1;
  while (std::chrono::steady_clock::now() < deadline) {
    if (core::agent_ping(sock, id++, 500ms).alive) return true;
    std::this_thread::sleep_for(200ms);
  }
  return false;
}

// ---- 3a: launch tracked via PID file ---------------------------------------
void test_launch_and_pidfile() {
  std::fprintf(stderr, "[3a] launch + pid file\n");
  auto spec = test_spec("t3a", {0});
  core::LaunchResult lr = core::launch(spec, g_runtime);
  REQUIRE(lr.ok, "launch succeeded");
  REQUIRE(lr.pid > 0, "got a pid");
  REQUIRE(core::pid_alive(lr.pid), "process is alive");
  auto pf = core::read_pidfile(g_runtime, "t3a");
  REQUIRE(pf.has_value() && *pf == lr.pid, "pid file records the pid");
  REQUIRE(core::pid_cmdline_contains(lr.pid, core::guest_marker("t3a")),
          "cmdline carries our -name marker");
  core::StopParams sp;
  sp.agent_socket = spec.guest_agent_socket;
  sp.qmp_socket = spec.qmp_socket;
  core::stop_guest(lr.pid, sp);
  core::remove_pidfile(g_runtime, "t3a");
}

// ---- 3b: pin post-spawn, verify via /proc readback -------------------------
void test_pin_and_readback() {
  std::fprintf(stderr, "[3b] cpu pinning with /proc readback\n");
  std::vector<std::int64_t> cores = {1, 2};  // host has >=3 (checked in main)
  auto spec = test_spec("t3b", cores);
  core::LaunchResult lr = core::launch(spec, g_runtime);
  REQUIRE(lr.ok, "launch succeeded");

  core::AffinityResult a = core::pin_and_verify(lr.pid, cores);
  REQUIRE(a.applied, "sched_setaffinity applied");
  REQUIRE(a.verified, "readback verified against requested set");
  REQUIRE((a.actual == std::set<int>{1, 2}), "actual cpus == {1,2}");

  // Independent confirmation straight from /proc.
  std::string err;
  auto proc = core::read_proc_cpus_allowed(lr.pid, err);
  REQUIRE((proc == std::set<int>{1, 2}),
          "/proc/<pid>/status Cpus_allowed_list == {1,2}");

  core::StopParams sp;
  sp.agent_socket = spec.guest_agent_socket;
  core::stop_guest(lr.pid, sp);
  core::remove_pidfile(g_runtime, "t3b");
}

// ---- 4 (part 1): guest-agent health probe against the real guest -----------
void test_health_probe() {
  std::fprintf(stderr, "[4] guest-agent health probe\n");
  auto spec = test_spec("t4h", {0});
  core::LaunchResult lr = core::launch(spec, g_runtime);
  REQUIRE(lr.ok, "launch succeeded");
  REQUIRE(wait_agent_ready(spec.guest_agent_socket, 15s),
          "guest agent became responsive");
  // Probe with the same tolerance the supervisor uses: a healthy guest answers
  // within a few attempts (the N-consecutive-failure threshold absorbs the
  // occasional reopen-race miss). A single mandatory shot would be stricter
  // than production semantics.
  bool alive = false;
  for (int i = 0; i < 3 && !alive; ++i) {
    if (core::agent_ping(spec.guest_agent_socket, 100 + i, 2s).alive)
      alive = true;
    else
      std::this_thread::sleep_for(1s);
  }
  REQUIRE(alive, "guest-ping returned alive within threshold attempts");
  core::StopParams sp;
  sp.agent_socket = spec.guest_agent_socket;
  core::stop_guest(lr.pid, sp);
  core::remove_pidfile(g_runtime, "t4h");
}

// ---- 3c (clean): guest-agent shutdown -> graceful, no SIGKILL --------------
void test_graceful_clean() {
  std::fprintf(stderr, "[3c] clean shutdown via guest agent\n");
  auto spec = test_spec("t3cc", {0});
  core::LaunchResult lr = core::launch(spec, g_runtime);
  REQUIRE(lr.ok, "launch succeeded");
  REQUIRE(wait_agent_ready(spec.guest_agent_socket, 15s), "agent ready");

  core::StopParams sp;
  sp.agent_socket = spec.guest_agent_socket;
  sp.qmp_socket = spec.qmp_socket;
  sp.graceful_timeout = 10s;
  core::StopResult r = core::stop_guest(lr.pid, sp);
  REQUIRE(r.outcome == core::StopOutcome::GracefulAgent,
          "stopped via graceful guest-agent path (no SIGKILL)");
  REQUIRE(!core::pid_alive(lr.pid), "process is gone");
  core::remove_pidfile(g_runtime, "t3cc");
}

// ---- 3c (forced): graceful times out -> SIGTERM/SIGKILL fallback -----------
void test_graceful_forced_timeout() {
  std::fprintf(stderr, "[3c] forced shutdown on graceful timeout\n");
  // No agent socket, and QMP system_powerdown is ignored by the busybox guest
  // (no acpid), so graceful cannot succeed -> escalation fires.
  auto spec = test_spec("t3cf", {0}, /*with_agent=*/false);
  core::LaunchResult lr = core::launch(spec, g_runtime);
  REQUIRE(lr.ok, "launch succeeded");
  std::this_thread::sleep_for(2s);  // let it come up

  core::StopParams sp;
  sp.qmp_socket = spec.qmp_socket;      // powerdown will be ignored by guest
  sp.agent_socket.clear();              // no clean path available
  sp.graceful_timeout = 1500ms;         // force the timeout quickly
  sp.sigterm_timeout = 3s;
  core::StopResult r = core::stop_guest(lr.pid, sp);
  REQUIRE(r.outcome == core::StopOutcome::Sigterm ||
              r.outcome == core::StopOutcome::Sigkill,
          "escalated to SIGTERM/SIGKILL after graceful timeout");
  REQUIRE(!core::pid_alive(lr.pid), "process is gone after forced stop");
  core::remove_pidfile(g_runtime, "t3cf");
}

// ---- 3e: missing image/path fails THIS launch, no process, no crash --------
void test_path_check() {
  std::fprintf(stderr, "[3e] launch-time path existence check\n");
  core::LaunchSpec spec;
  spec.name = "t3e";
  spec.cpus = {0};
  spec.memory_bytes = 64ull << 20;
  core::LaunchSpec::DiskBoot disk;
  disk.image = "/nonexistent/does-not-exist.qcow2";
  disk.type = cfg::DiskType::Qcow2;
  spec.disk = disk;
  spec.qmp_socket = g_runtime + "/t3e.qmp.sock";
  spec.pidfile = core::pidfile_path(g_runtime, "t3e");

  std::string pe = core::check_paths(spec);
  REQUIRE(!pe.empty(), "check_paths reports the missing image");

  core::LaunchResult lr = core::launch(spec, g_runtime);
  REQUIRE(!lr.ok, "launch refused (ok=false)");
  REQUIRE(lr.pid == 0, "no process was spawned");
  REQUIRE(!core::read_pidfile(g_runtime, "t3e").has_value(),
          "no pid file written");
}

// ---- 3d: adoption on daemon restart (no double-launch) ---------------------
void test_adoption() {
  std::fprintf(stderr, "[3d] pid-file adoption across daemon restart\n");
  // Simulate the daemon by building a Supervisor whose spec_provider returns
  // the busybox direct-kernel spec.
  cfg::Config c;
  cfg::VmConfig vm;
  vm.name = "t3d";
  vm.image = "/unused";  // provider overrides boot source
  vm.disk_type = cfg::DiskType::Raw;
  vm.cpus = {0};
  vm.memory_bytes = 128ull << 20;
  vm.network = cfg::Network::User;
  vm.restart = cfg::RestartPolicy::Never;
  c.vms.push_back(vm);

  auto provider = [](const cfg::VmConfig& v) { return test_spec(v.name, v.cpus); };

  core::SupervisorOptions o1;
  o1.runtime_dir = g_runtime;
  o1.spec_provider = provider;
  core::Supervisor sup1(c, o1);
  std::string err;
  REQUIRE(sup1.start("t3d", err), "first supervisor started the guest");
  int pid1 = sup1.runtimes().at("t3d").pid;
  REQUIRE(core::pid_alive(pid1), "guest running");

  // "Restart the daemon": a brand-new Supervisor over the same runtime dir.
  core::SupervisorOptions o2;
  o2.runtime_dir = g_runtime;
  o2.spec_provider = provider;
  core::Supervisor sup2(c, o2);
  sup2.adopt_existing();
  const auto& rt = sup2.runtimes().at("t3d");
  REQUIRE(rt.state == core::VmState::Running, "adopted as running");
  REQUIRE(rt.pid == pid1, "adopted the SAME pid (no double-launch)");
  REQUIRE(rt.adopted, "flagged as adopted");

  // A reconcile on the new supervisor must NOT plan to start it again.
  core::ReconcilePlan p = sup2.plan();
  bool would_start = false;
  for (auto& n : p.to_start) if (n == "t3d") would_start = true;
  REQUIRE(!would_start, "reconcile does not re-start the adopted guest");

  sup2.stop("t3d");
  REQUIRE(!core::pid_alive(pid1), "stopped after test");
}

// ---- 4 (part 2): restart policy honored for all three values ---------------
void test_restart_policy(cfg::RestartPolicy policy, const char* label,
                         bool expect_restart) {
  std::fprintf(stderr, "[4] restart policy '%s'\n", label);
  cfg::Config c;
  cfg::VmConfig vm;
  vm.name = "t4p";
  vm.image = "/unused";
  vm.disk_type = cfg::DiskType::Raw;
  vm.cpus = {0};
  vm.memory_bytes = 128ull << 20;
  vm.network = cfg::Network::User;
  vm.restart = policy;
  c.vms.push_back(vm);

  core::SupervisorOptions o;
  o.runtime_dir = g_runtime;
  o.health_failure_threshold = 1;  // react fast in the test
  o.spec_provider = [](const cfg::VmConfig& v) {
    return test_spec(v.name, v.cpus);
  };
  core::Supervisor sup(c, o);
  std::string err;
  REQUIRE(sup.start("t4p", err), "guest started");
  int pid1 = sup.runtimes().at("t4p").pid;

  // Deliberately KILL the guest out from under the supervisor.
  ::kill(pid1, SIGKILL);
  std::this_thread::sleep_for(500ms);

  // One health tick: should detect death and apply policy.
  sup.health_tick();
  std::this_thread::sleep_for(1s);
  sup.health_tick();  // second tick lets a restarted guest settle

  const auto& rt = sup.runtimes().at("t4p");
  if (expect_restart) {
    REQUIRE(rt.state == core::VmState::Running || rt.restarts >= 1,
            "guest was restarted per policy");
    REQUIRE(rt.pid != 0 && rt.pid != pid1 && core::pid_alive(rt.pid),
            "new process with a different pid is running");
    sup.stop("t4p");
  } else {
    REQUIRE(rt.state == core::VmState::Failed ||
            rt.state == core::VmState::HealthPanic,
            "guest left failed (policy 'never' did not restart)");
    REQUIRE(rt.restarts == 0, "no restart happened");
  }
  core::remove_pidfile(g_runtime, "t4p");
}

}  // namespace

int main(int argc, char** argv) {
  core::Capabilities caps = core::detect_capabilities();
  if (!caps.can_run_guests()) {
    std::fprintf(stderr,
                 "SKIP: real-QEMU integration tests require /dev/kvm + "
                 "qemu-system (kvm=%d qemu=%d: %s)\n",
                 caps.kvm, caps.qemu, caps.kvm_reason.c_str());
    return kSkip;
  }
  if (std::thread::hardware_concurrency() < 3) {
    std::fprintf(stderr, "SKIP: need >=3 host cores for the pinning test\n");
    return kSkip;
  }

  // Locate the test guest (built by the CTest fixture or passed on argv).
  std::string guest_dir = argc > 1 ? argv[1] : "build/testguest";
  g_kernel = guest_dir + "/vmlinuz";
  g_initramfs = guest_dir + "/initramfs.cpio.gz";
  if (!fs::exists(g_kernel) || !fs::exists(g_initramfs)) {
    std::fprintf(stderr, "SKIP: test guest not found in %s (run "
                         "iso-builder/testguest/build-testguest.sh)\n",
                 guest_dir.c_str());
    return kSkip;
  }

  g_runtime = (fs::temp_directory_path() / "hypercore-itest").string();
  fs::remove_all(g_runtime);
  fs::create_directories(g_runtime);
  std::fprintf(stderr, "runtime dir: %s\n", g_runtime.c_str());

  test_launch_and_pidfile();
  test_pin_and_readback();
  test_health_probe();
  test_graceful_clean();
  test_graceful_forced_timeout();
  test_path_check();
  test_adoption();
  test_restart_policy(cfg::RestartPolicy::Never, "never", false);
  test_restart_policy(cfg::RestartPolicy::OnFailure, "on-failure", true);
  test_restart_policy(cfg::RestartPolicy::Always, "always", true);

  fs::remove_all(g_runtime);
  std::fprintf(stderr, "\nintegration: %s (%d failures)\n",
               g_failures == 0 ? "PASS" : "FAIL", g_failures);
  return g_failures == 0 ? 0 : 1;
}
