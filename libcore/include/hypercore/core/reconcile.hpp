// Reconciliation: desired (config) vs actual (running) state (requirement #5).
//
// The diff itself is PURE and host-independent so it can be unit-tested without
// touching any process: given the set of desired VMs and a snapshot of actual
// state, it classifies each guest into start / stop / restart / unchanged. The
// supervisor supplies real ActualState (from PID files + liveness); tests
// supply synthetic snapshots.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "hypercore/config/types.hpp"

namespace hypercore::core {

// A minimal fingerprint of what a guest is actually running as, so we can
// detect config changes that require a restart (not just presence/absence).
struct ActualState {
  bool running = false;
  int pid = 0;
  // Fingerprint of the running guest's launch-relevant config, captured when
  // it was started. Compared against the desired fingerprint to decide
  // restart. Empty when unknown (e.g. adopted without provenance).
  std::string fingerprint;
};

// Stable fingerprint of the launch-relevant fields of a VmConfig. Two configs
// with the same fingerprint can share a running process; a change means the
// guest must be restarted to take effect.
std::string fingerprint(const config::VmConfig& vm);

struct ReconcilePlan {
  std::vector<std::string> to_start;      // desired, not running
  std::vector<std::string> to_stop;       // running, no longer desired
  std::vector<std::string> to_restart;    // running but config changed
  std::vector<std::string> unchanged;     // running and matching

  bool empty() const {
    return to_start.empty() && to_stop.empty() && to_restart.empty();
  }
};

// Compute the plan. `desired` is the config's VMs; `actual` maps vm name ->
// ActualState for every guest we currently know about (running or with a
// lingering pid file). Pure: no side effects.
ReconcilePlan reconcile(const std::vector<config::VmConfig>& desired,
                        const std::map<std::string, ActualState>& actual);

}  // namespace hypercore::core
