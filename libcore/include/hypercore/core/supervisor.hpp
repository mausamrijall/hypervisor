// The supervisor: the daemon's stateful runtime engine. Owns per-VM state and
// implements adoption (#3d), launch+pin (#3a/#3b), health checks + restart
// policy (#4), reconciliation (#5), and graceful stop (#3c).
//
// A SpecProvider indirection lets integration tests drive real guests: the
// production provider builds a disk-boot LaunchSpec from VmConfig, while tests
// substitute a direct-kernel spec that boots the throwaway busybox guest
// through the identical launch/pin/track/stop code.
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "hypercore/config/types.hpp"
#include "hypercore/core/proc_stats.hpp"
#include "hypercore/core/qemu.hpp"
#include "hypercore/core/qmp.hpp"
#include "hypercore/core/reconcile.hpp"

namespace hypercore::core {

enum class VmState { Stopped, Starting, Running, Unhealthy, Stopping, Failed, HealthPanic };
const char* to_string(VmState);

enum class Health { Unknown, Healthy, Unhealthy };
const char* to_string(Health);

struct VmRuntime {
  std::string name;
  VmState state = VmState::Stopped;
  Health health = Health::Unknown;
  int pid = 0;
  bool cpus_verified = false;
  std::vector<std::int64_t> cpus;
  std::uint64_t memory_bytes = 0;
  config::Network network = config::Network::User;
  config::RestartPolicy restart = config::RestartPolicy::OnFailure;
  std::string agent_socket;
  std::string qmp_socket;
  std::string fingerprint;
  std::chrono::steady_clock::time_point started_at{};
  int restarts = 0;
  int consecutive_health_failures = 0;
  bool adopted = false;  // discovered via pid file on startup

  // Phase 4: fields the CLI/dashboard consume.
  std::string serial_log;         // console capture path for `logs`
  int ssh_port = 0;               // user-net hostfwd port (0 => none)
  std::string ip;                 // guest IP learned via agent (bridge); may be ""
  double cpu_percent = 0.0;       // host-side CPU% of the QEMU process
  std::uint64_t rss_bytes = 0;    // host-side resident memory of QEMU
  ProcSampler sampler;            // rolling CPU baseline (not serialized)
};

// Provides the LaunchSpec for a VM. Production default lives in the .cpp.
using SpecProvider = std::function<LaunchSpec(const config::VmConfig&)>;

struct SupervisorOptions {
  std::string runtime_dir = "/run/hypercore";
  int health_failure_threshold = 3;  // N consecutive misses => unhealthy (#4)
  std::chrono::milliseconds health_timeout{1500};
  StopParams stop = {};
  SpecProvider spec_provider = nullptr;  // null => production disk-boot provider

  // Privilege separation: uid/gid each QEMU child drops to before exec. 0 means
  // "do not drop" (the daemon is already unprivileged, e.g. dev/test). The
  // daemon populates this from --qemu-user when running as root. See qemu.cpp.
  unsigned qemu_uid = 0;
  unsigned qemu_gid = 0;
  // Supplementary group (kvm) the dropped QEMU keeps so it can open /dev/kvm.
  unsigned kvm_gid = 0;
};

class Supervisor {
 public:
  Supervisor(config::Config cfg, SupervisorOptions opts);

  // Discover guests still running from a previous daemon life via their pid
  // files, and adopt them instead of relaunching (#3d). Safe to call once at
  // startup.
  void adopt_existing();

  // Pure snapshot of desired-vs-actual, for --reconcile --dry-run (#5).
  ReconcilePlan plan() const;

  // Apply a plan: start/stop/restart as needed. Per-VM failures are recorded
  // on the VmRuntime and logged; they never abort the whole apply.
  void apply(const ReconcilePlan& p);

  // Individual operations (also used by the control socket).
  bool start(const std::string& name, std::string& err);
  StopResult stop(const std::string& name);

  // One health-check pass over running guests: detect process death, ping the
  // guest agent, and apply restart policy on failure (#4). Call periodically.
  void health_tick();

  const std::map<std::string, VmRuntime>& runtimes() const { return rt_; }
  const config::Config& config() const { return cfg_; }
  bool has_vm(const std::string& name) const;

 private:
  LaunchSpec spec_for(const config::VmConfig& vm) const;
  const config::VmConfig* find_cfg(const std::string& name) const;
  void handle_failure(VmRuntime& rt, const char* reason);
  std::map<std::string, ActualState> snapshot_actual() const;

  config::Config cfg_;
  SupervisorOptions opts_;
  std::map<std::string, VmRuntime> rt_;
  int health_sync_counter_ = 0;
};

}  // namespace hypercore::core
