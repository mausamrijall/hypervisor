#include "hypercore/core/portmap.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

// We use nlohmann/json (already vendored) for the allocation file so the
// format stays human-readable and grep-able in /run/hypercore/ports.json.
#include "json.hpp"

namespace hypercore::core {

namespace {

std::string portmap_path(const std::string& runtime_dir) {
  return runtime_dir + "/ports.json";
}

// Load the current allocation map from disk. Returns an empty map on any
// read or parse error (treated as "no allocations yet").
std::unordered_map<std::string, int> load_map(const std::string& runtime_dir) {
  std::unordered_map<std::string, int> m;
  std::ifstream f(portmap_path(runtime_dir));
  if (!f) return m;
  try {
    nlohmann::json j;
    f >> j;
    if (j.is_object()) {
      for (auto& [k, v] : j.items()) {
        if (v.is_number_integer()) m[k] = v.get<int>();
      }
    }
  } catch (...) {
    // Corrupted file: start fresh. The next save will overwrite it.
  }
  return m;
}

// Atomically persist the allocation map.
bool save_map(const std::string& runtime_dir,
              const std::unordered_map<std::string, int>& m) {
  nlohmann::json j = nlohmann::json::object();
  for (const auto& [k, v] : m) j[k] = v;

  std::string path = portmap_path(runtime_dir);
  std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) return false;
    f << j.dump(2) << "\n";
    if (!f) return false;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace

int portmap_acquire(const std::string& runtime_dir,
                    const std::string& vm_name) {
  auto m = load_map(runtime_dir);

  // Idempotent: already assigned to this VM — return existing port.
  auto it = m.find(vm_name);
  if (it != m.end() && it->second >= PORT_MIN && it->second <= PORT_MAX)
    return it->second;

  // Build the set of already-allocated ports.
  std::unordered_map<int, bool> used;
  for (const auto& [name, port] : m)
    if (port >= PORT_MIN && port <= PORT_MAX) used[port] = true;

  // Sequential scan for the next free port.
  int assigned = 0;
  for (int p = PORT_MIN; p <= PORT_MAX; ++p) {
    if (used.find(p) == used.end()) { assigned = p; break; }
  }
  if (assigned == 0) return 0;  // range exhausted

  m[vm_name] = assigned;
  save_map(runtime_dir, m);  // best-effort; if it fails the port is still used
  return assigned;
}

void portmap_release(const std::string& runtime_dir,
                     const std::string& vm_name) {
  auto m = load_map(runtime_dir);
  if (m.erase(vm_name) == 0) return;  // nothing to release
  save_map(runtime_dir, m);
}

int portmap_lookup(const std::string& runtime_dir,
                   const std::string& vm_name) {
  auto m = load_map(runtime_dir);
  auto it = m.find(vm_name);
  if (it != m.end() && it->second >= PORT_MIN && it->second <= PORT_MAX)
    return it->second;
  return 0;
}

}  // namespace hypercore::core
