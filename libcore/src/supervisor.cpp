#include "hypercore/core/supervisor.hpp"

#include <utility>

#include "hypercore/core/affinity.hpp"
#include "hypercore/core/guest_agent.hpp"
#include "hypercore/core/pidfile.hpp"
#include "hypercore/log.hpp"

namespace hypercore::core {

namespace log = hypercore::log;

const char* to_string(VmState s) {
  switch (s) {
    case VmState::Stopped: return "stopped";
    case VmState::Starting: return "starting";
    case VmState::Running: return "running";
    case VmState::Unhealthy: return "unhealthy";
    case VmState::Stopping: return "stopping";
    case VmState::Failed: return "failed";
  }
  return "?";
}
const char* to_string(Health h) {
  switch (h) {
    case Health::Unknown: return "unknown";
    case Health::Healthy: return "healthy";
    case Health::Unhealthy: return "unhealthy";
  }
  return "?";
}

Supervisor::Supervisor(config::Config cfg, SupervisorOptions opts)
    : cfg_(std::move(cfg)), opts_(std::move(opts)) {
  // Seed runtime entries from config so status is complete even before start.
  for (const auto& vm : cfg_.vms) {
    VmRuntime rt;
    rt.name = vm.name;
    rt.cpus = vm.cpus;
    rt.memory_bytes = vm.memory_bytes;
    if (vm.network) rt.network = *vm.network;
    rt.restart = vm.restart;
    rt.fingerprint = fingerprint(vm);
    LaunchSpec s = spec_for(vm);
    rt.agent_socket = s.guest_agent_socket;
    rt.qmp_socket = s.qmp_socket;
    rt_[vm.name] = std::move(rt);
  }
}

const config::VmConfig* Supervisor::find_cfg(const std::string& name) const {
  for (const auto& vm : cfg_.vms)
    if (vm.name == name) return &vm;
  return nullptr;
}

bool Supervisor::has_vm(const std::string& name) const {
  return rt_.find(name) != rt_.end();
}

LaunchSpec Supervisor::spec_for(const config::VmConfig& vm) const {
  if (opts_.spec_provider) return opts_.spec_provider(vm);
  return spec_from_vm(vm, opts_.runtime_dir);
}

void Supervisor::adopt_existing() {
  for (auto& [name, rt] : rt_) {
    auto pid = read_pidfile(opts_.runtime_dir, name);
    if (!pid) continue;
    // Confirm the PID is alive AND is really our guest (guard PID reuse #3d).
    if (pid_alive(*pid) && pid_cmdline_contains(*pid, guest_marker(name))) {
      rt.pid = *pid;
      rt.state = VmState::Running;
      rt.adopted = true;
      rt.started_at = std::chrono::steady_clock::now();
      // Load the fingerprint the guest was actually LAUNCHED with (persisted in
      // its meta file). Without this, a fresh process could not tell that the
      // config changed since launch and would never plan a restart.
      if (auto meta = read_meta(opts_.runtime_dir, name)) rt.fingerprint = *meta;
      // Re-verify pinning against the adopted process.
      const config::VmConfig* vm = find_cfg(name);
      if (vm) {
        AffinityResult a = pin_and_verify(*pid, vm->cpus);
        rt.cpus_verified = a.verified;
      }
      log::info("adopted running guest",
                {log::field("vm", name), log::field("pid", *pid),
                 log::field("cpus_verified", rt.cpus_verified)});
    } else {
      // Stale pid file: process gone or not ours. Clean it up.
      remove_pidfile(opts_.runtime_dir, name);
      remove_meta(opts_.runtime_dir, name);
      log::info("removed stale pid file",
                {log::field("vm", name), log::field("pid", *pid)});
    }
  }
}

std::map<std::string, ActualState> Supervisor::snapshot_actual() const {
  std::map<std::string, ActualState> actual;
  for (const auto& [name, rt] : rt_) {
    ActualState st;
    st.running = (rt.state == VmState::Running || rt.state == VmState::Unhealthy);
    st.pid = rt.pid;
    st.fingerprint = rt.fingerprint;
    actual[name] = st;
  }
  return actual;
}

ReconcilePlan Supervisor::plan() const {
  return reconcile(cfg_.vms, snapshot_actual());
}

void Supervisor::apply(const ReconcilePlan& p) {
  for (const auto& name : p.to_stop) stop(name);
  for (const auto& name : p.to_restart) {
    stop(name);
    std::string err;
    if (!start(name, err))
      log::error("restart: start failed",
                 {log::field("vm", name), log::field("error", err)});
  }
  for (const auto& name : p.to_start) {
    std::string err;
    if (!start(name, err))
      log::error("start failed",
                 {log::field("vm", name), log::field("error", err)});
  }
}

bool Supervisor::start(const std::string& name, std::string& err) {
  auto rit = rt_.find(name);
  if (rit == rt_.end()) { err = "unknown vm"; return false; }
  VmRuntime& rt = rit->second;
  const config::VmConfig* vm = find_cfg(name);
  if (!vm) { err = "no config for vm"; return false; }

  if (rt.state == VmState::Running) { err = "already running"; return false; }

  rt.state = VmState::Starting;
  LaunchSpec spec = spec_for(*vm);

  // #3e: path checks happen inside launch(), before any spawn.
  LaunchResult lr = launch(spec, opts_.runtime_dir);
  if (!lr.ok) {
    rt.state = VmState::Failed;
    err = lr.error;
    log::error("launch failed",
               {log::field("vm", name), log::field("error", err)});
    return false;
  }
  rt.pid = lr.pid;

  // #3b: apply CPU pinning and VERIFY via /proc readback.
  AffinityResult aff = pin_and_verify(lr.pid, vm->cpus);
  rt.cpus_verified = aff.verified;
  if (!aff.verified)
    log::warn("cpu pinning not verified",
              {log::field("vm", name), log::field("detail", aff.error)});
  else
    log::info("cpu pinning verified",
              {log::field("vm", name), log::field("cpus", aff.actual.size())});

  rt.state = VmState::Running;
  rt.health = Health::Unknown;
  rt.consecutive_health_failures = 0;
  rt.started_at = std::chrono::steady_clock::now();
  rt.adopted = false;
  // Persist the launch fingerprint so a separate process can detect drift.
  rt.fingerprint = fingerprint(*vm);
  std::string merr;
  write_meta(opts_.runtime_dir, name, rt.fingerprint, merr);
  log::info("guest started",
            {log::field("vm", name), log::field("pid", lr.pid),
             log::field("cpus_verified", rt.cpus_verified)});
  return true;
}

StopResult Supervisor::stop(const std::string& name) {
  auto rit = rt_.find(name);
  if (rit == rt_.end()) return {StopOutcome::Error, "unknown vm"};
  VmRuntime& rt = rit->second;
  if (rt.pid <= 0 || !pid_alive(rt.pid)) {
    rt.state = VmState::Stopped;
    rt.pid = 0;
    remove_pidfile(opts_.runtime_dir, name);
    return {StopOutcome::AlreadyDead, "not running"};
  }
  rt.state = VmState::Stopping;
  StopParams sp = opts_.stop;
  sp.qmp_socket = rt.qmp_socket;
  sp.agent_socket = rt.agent_socket;
  StopResult res = stop_guest(rt.pid, sp);
  rt.state = VmState::Stopped;
  rt.pid = 0;
  rt.health = Health::Unknown;
  remove_pidfile(opts_.runtime_dir, name);
  remove_meta(opts_.runtime_dir, name);
  log::info("guest stopped",
            {log::field("vm", name), log::field("outcome", to_string(res.outcome))});
  return res;
}

void Supervisor::handle_failure(VmRuntime& rt, const char* reason) {
  // Apply restart policy (#4). Never hardcode always-restart.
  switch (rt.restart) {
    case config::RestartPolicy::Never:
      rt.state = VmState::Failed;
      log::warn("guest failed; restart policy 'never' -> leaving stopped",
                {log::field("vm", rt.name), log::field("reason", reason)});
      break;
    case config::RestartPolicy::OnFailure:
    case config::RestartPolicy::Always: {
      log::warn("guest failed; restart policy applies -> restarting",
                {log::field("vm", rt.name), log::field("reason", reason),
                 log::field("policy", config::to_string(rt.restart))});
      // Ensure the old process is gone, then relaunch.
      if (rt.pid > 0 && pid_alive(rt.pid)) stop(rt.name);
      rt.pid = 0;
      rt.state = VmState::Stopped;
      std::string err;
      if (start(rt.name, err)) {
        rt.restarts++;
      } else {
        rt.state = VmState::Failed;
        log::error("restart failed",
                   {log::field("vm", rt.name), log::field("error", err)});
      }
      break;
    }
  }
}

void Supervisor::health_tick() {
  // Reap any exited children first so pid_alive() sees the true state and does
  // not treat a just-exited guest as still running.
  reap_children();
  for (auto& [name, rt] : rt_) {
    if (rt.state != VmState::Running && rt.state != VmState::Unhealthy)
      continue;

    // 1. Process liveness: a dead QEMU is an immediate failure.
    if (rt.pid <= 0 || !pid_alive(rt.pid)) {
      rt.health = Health::Unhealthy;
      handle_failure(rt, "process exited");
      continue;
    }

    // 2. Guest-agent probe (only if the guest has an agent channel).
    if (rt.agent_socket.empty()) {
      rt.health = Health::Unknown;  // no agent => liveness-only (documented)
      continue;
    }
    AgentPing p =
        agent_ping(rt.agent_socket, ++health_sync_counter_, opts_.health_timeout);
    if (p.alive) {
      rt.health = Health::Healthy;
      rt.consecutive_health_failures = 0;
      if (rt.state == VmState::Unhealthy) rt.state = VmState::Running;
    } else {
      rt.consecutive_health_failures++;
      log::debug("health probe failed",
                 {log::field("vm", name),
                  log::field("fails", rt.consecutive_health_failures),
                  log::field("detail", p.detail)});
      if (rt.consecutive_health_failures >= opts_.health_failure_threshold) {
        rt.health = Health::Unhealthy;
        rt.state = VmState::Unhealthy;
        handle_failure(rt, "guest agent unresponsive");
      }
    }
  }
}

}  // namespace hypercore::core
