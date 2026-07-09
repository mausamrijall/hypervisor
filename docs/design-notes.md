# Design notes & open questions

Running log of design decisions and flagged issues that later phases surfaced in
earlier ones. Items marked **OPEN** are deferred to the testing + security audit
phase for a decision, per the build-forward plan.

## OPEN — health-failed guest with `restart = "never"` is left running

**Surfaced in:** Phase 4 (dashboard showed `state=failed` for a guest whose QEMU
process was still alive and burning CPU).

**Current behavior:** In `Supervisor::handle_failure`, a guest whose health
checks fail (agent unresponsive past the threshold) with `restart = never` is
marked `VmState::Failed` but its QEMU process is **left running**. Only
`on-failure`/`always` stop-and-relaunch.

**The question:** what should `never` mean for a *health* failure (as opposed to
a process exit)?
- Interpretation A (current): "never touch it" — leave the possibly-wedged
  process alone so an operator can inspect it.
- Interpretation B: "don't bring it back, but do stop the broken one" — a
  health-failed guest is stopped and stays stopped.

**Why it matters:** a wedged guest under interpretation A keeps its pinned cores
and RAM and may spin CPU, with no automatic reclamation. Under B we reclaim
resources but lose forensic state.

**Recommendation:** lean B for resource hygiene, but make it explicit. Deferred
to audit — flagging rather than silently changing behavior.

Note: a guest whose *process exits* (not a health-check failure) with
`restart = never` is already correctly left in `Failed` with no process, which
is unambiguous. This question is only about the health-check path.

## RESOLVED — stale CPU/RSS/health on stopped guests

**Surfaced in:** Phase 4. A manually stopped guest showed stale sampled CPU%,
RSS, and a lingering `unhealthy` health value.

**Fix:** `Supervisor::stop` now clears `cpu_percent`, `rss_bytes`, `ip`, resets
the CPU sampler, and sets health to `Unknown` on every stop path. (Committed in
Phase 4.)

## SSH endpoint resolution (Phase 4 decision)

Per user decision: **guest-agent + hostfwd**.
- **user networking:** the daemon assigns a stable per-guest SSH host-forward
  port (FNV-1a hash of the name into 20000–29999) at launch, so `ssh <name>`
  connects to `127.0.0.1:<port>` → guest:22. Stable across restarts/adoption.
- **bridge networking:** the daemon learns the guest IP opportunistically via
  the guest agent (`guest-network-get-interfaces`) during health ticks.

**Known limitation (for audit):** the hostfwd port is a name hash, so two guests
could theoretically collide on a port; the second QEMU would then fail to bind
and report a launch failure rather than silently misroute. Acceptable for v1;
revisit with an allocation table if it bites.

## CPU/RAM source (Phase 4 decision)

Per user decision: **host `/proc` sampling**. CPU% from `/proc/<pid>/stat`
utime+stime deltas across health ticks; memory from `/proc/<pid>/status` VmRSS.
Reflects the QEMU process's host-side cost (no guest cooperation needed).
