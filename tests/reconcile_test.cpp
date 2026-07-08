// Unit tests for reconciliation diff logic and CPU-list parsing.
//
// These are deliberately HOST-INDEPENDENT and process-free (requirement #5:
// "keep unit tests for reconciliation diff logic mockable/host-independent").
// No QEMU, no /dev/kvm, no spawning — they run everywhere, including CI runners
// without KVM.

#include "hctest.hpp"

#include <map>
#include <string>
#include <vector>

#include "hypercore/config/types.hpp"
#include "hypercore/core/affinity.hpp"
#include "hypercore/core/reconcile.hpp"

using namespace hypercore::core;
namespace cfg = hypercore::config;

namespace {

cfg::VmConfig make_vm(const std::string& name, std::vector<std::int64_t> cpus,
                      std::uint64_t mem = 1u << 30,
                      const std::string& image = "/img") {
  cfg::VmConfig v;
  v.name = name;
  v.image = image;
  v.disk_type = cfg::DiskType::Raw;
  v.cpus = std::move(cpus);
  v.memory_bytes = mem;
  v.network = cfg::Network::User;
  return v;
}

}  // namespace

// --- CPU list parsing (used by the /proc readback verifier) -----------------
TEST(cpu_list_parsing) {
  CHECK(parse_cpu_list("1,3") == (std::set<int>{1, 3}));
  CHECK(parse_cpu_list("0-3") == (std::set<int>{0, 1, 2, 3}));
  CHECK(parse_cpu_list("1,3-5,7") == (std::set<int>{1, 3, 4, 5, 7}));
  CHECK(parse_cpu_list("2") == (std::set<int>{2}));
  CHECK(parse_cpu_list("").empty());
}

// --- fingerprint changes only on launch-relevant fields ---------------------
TEST(fingerprint_stability) {
  cfg::VmConfig a = make_vm("web", {0, 1});
  cfg::VmConfig b = make_vm("web", {1, 0});  // same set, different order
  CHECK_EQ(fingerprint(a), fingerprint(b));  // order-independent

  cfg::VmConfig c = make_vm("web", {0, 1}, 2u << 30);  // memory changed
  CHECK(fingerprint(a) != fingerprint(c));

  // restart policy is NOT part of the fingerprint (runtime concern).
  cfg::VmConfig d = make_vm("web", {0, 1});
  d.restart = cfg::RestartPolicy::Always;
  CHECK_EQ(fingerprint(a), fingerprint(d));
}

// --- desired, nothing running -> all start ----------------------------------
TEST(reconcile_all_start) {
  std::vector<cfg::VmConfig> desired = {make_vm("a", {0}), make_vm("b", {1})};
  std::map<std::string, ActualState> actual;  // nothing running
  ReconcilePlan p = reconcile(desired, actual);
  CHECK(p.to_start == (std::vector<std::string>{"a", "b"}));
  CHECK(p.to_stop.empty());
  CHECK(p.to_restart.empty());
}

// --- running but not desired -> stop ----------------------------------------
TEST(reconcile_stop_removed) {
  std::vector<cfg::VmConfig> desired = {make_vm("a", {0})};
  std::map<std::string, ActualState> actual;
  actual["a"] = {true, 100, fingerprint(make_vm("a", {0}))};
  actual["ghost"] = {true, 200, "whatever"};  // running, not in config
  ReconcilePlan p = reconcile(desired, actual);
  CHECK(p.to_stop == (std::vector<std::string>{"ghost"}));
  CHECK(p.unchanged == (std::vector<std::string>{"a"}));
  CHECK(p.to_start.empty());
}

// --- running, config changed -> restart -------------------------------------
TEST(reconcile_restart_on_change) {
  cfg::VmConfig now = make_vm("a", {0}, 4u << 30);   // desired: 4 GiB
  std::vector<cfg::VmConfig> desired = {now};
  std::map<std::string, ActualState> actual;
  actual["a"] = {true, 100, fingerprint(make_vm("a", {0}, 1u << 30))};  // was 1
  ReconcilePlan p = reconcile(desired, actual);
  CHECK(p.to_restart == (std::vector<std::string>{"a"}));
  CHECK(p.unchanged.empty());
}

// --- running, unchanged -> unchanged ----------------------------------------
TEST(reconcile_unchanged) {
  cfg::VmConfig v = make_vm("a", {0, 1});
  std::vector<cfg::VmConfig> desired = {v};
  std::map<std::string, ActualState> actual;
  actual["a"] = {true, 100, fingerprint(v)};
  ReconcilePlan p = reconcile(desired, actual);
  CHECK(p.unchanged == (std::vector<std::string>{"a"}));
  CHECK(p.empty());  // no start/stop/restart
}

// --- adopted process (no fingerprint) -> unchanged, not force-restarted -----
TEST(reconcile_adopted_no_fingerprint) {
  cfg::VmConfig v = make_vm("a", {0});
  std::vector<cfg::VmConfig> desired = {v};
  std::map<std::string, ActualState> actual;
  actual["a"] = {true, 100, ""};  // adopted: fingerprint unknown
  ReconcilePlan p = reconcile(desired, actual);
  CHECK(p.unchanged == (std::vector<std::string>{"a"}));
  CHECK(p.to_restart.empty());  // must NOT churn an adopted guest
}

// --- mixed scenario ---------------------------------------------------------
TEST(reconcile_mixed) {
  std::vector<cfg::VmConfig> desired = {
      make_vm("keep", {0}), make_vm("change", {1}, 2u << 30),
      make_vm("new", {2})};
  std::map<std::string, ActualState> actual;
  actual["keep"] = {true, 1, fingerprint(make_vm("keep", {0}))};
  actual["change"] = {true, 2, fingerprint(make_vm("change", {1}, 1u << 30))};
  actual["old"] = {true, 3, "x"};  // to be stopped
  ReconcilePlan p = reconcile(desired, actual);
  CHECK(p.to_start == (std::vector<std::string>{"new"}));
  CHECK(p.to_stop == (std::vector<std::string>{"old"}));
  CHECK(p.to_restart == (std::vector<std::string>{"change"}));
  CHECK(p.unchanged == (std::vector<std::string>{"keep"}));
}
