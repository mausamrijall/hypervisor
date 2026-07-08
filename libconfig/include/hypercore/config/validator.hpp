// Validator: semantic checks over a typed Config, as a separate pass.
//
// Runs AFTER the parser. Accumulates ALL problems (never stops at the first)
// and returns them as Diagnostics. Splits into errors (reject the config) and
// warnings (accept but flag). See docs/schema.md E1-E10 / W1-W4 for the rules;
// this pass owns E2-E10 and W1-W4 (E1 belongs to the parser).
//
// Host-dependent rules (E7 CPU-out-of-range, W2 RAM overcommit) compare against
// an injected HostInfo so results are deterministic and testable.
#pragma once

#include "hypercore/config/diagnostics.hpp"
#include "hypercore/config/host_info.hpp"
#include "hypercore/config/types.hpp"

namespace hypercore::config {

// Validate `cfg` against `host`. Returns every error and warning found.
// Pure: does not touch the filesystem or any global. (Path existence checks
// like "image file present" are intentionally deferred to the daemon at launch
// time — a config can be validated on a machine that doesn't hold the images.)
Diagnostics validate(const Config& cfg, const HostInfo& host);

}  // namespace hypercore::config
