# Design notes & open questions

Running log of design decisions and flagged issues that later phases surfaced in
earlier ones. Items marked **OPEN** are deferred to the testing + security audit
phase for a decision, per the build-forward plan.

## RESOLVED (security audit) — hardening pass

Four hardening items were implemented during the security audit phase:

1. **SSH argument injection (HC-2026-001, critical).** The guest agent is
   untrusted; its reported IP flowed into the operator's `ssh` argv unvalidated,
   letting a malicious guest redirect/inject the operator's SSH session. Fixed at
   three layers: `agent_get_ipv4()` now accepts only `inet_pton(AF_INET)`-valid
   dotted quads; the CLI `cmd_ssh` re-validates the IP and invokes ssh with
   `-oProxyCommand=none -oPermitLocalCommand=no --` so a hostile value can never
   be parsed as an option or spawn a local command. Report:
   `../reports/hypercore-guest-agent-ssh-argument-injection.md`.

2. **QEMU privilege separation.** The daemon runs as root (PID 1). Each forked
   QEMU child now drops to an unprivileged user (`--qemu-user`, default
   `hypercore`) via `setgroups`/`setgid`/`setuid` BEFORE `execvp`, keeping only
   the `kvm` group so it can open `/dev/kvm`. The drop is verified (re-`setuid(0)`
   must fail; uid must actually change) and **fail-closed**: if the daemon is
   root but can't resolve a non-root target, or the drop can't be verified, it
   refuses to launch rather than run QEMU as root. So a guest/QEMU escape no
   longer yields root over host RAM. The ISO image provisions the `hypercore`
   user (uid/gid 976) so this resolves at boot.

3. **Unbounded read DoS.** `UnixClient::read_line` (used against the untrusted
   guest-agent socket) now caps a single line at `kMaxLineBytes` (64 KiB) so a
   guest streaming bytes without a newline cannot exhaust daemon memory.

4. **Control-parser panic safety.** `handle_request` is wrapped so no exception
   (missed `map::at`, `bad_alloc`, `stoi` edge cases) can unwind out of the
   accept loop and kill the daemon; malformed input yields an error reply. A new
   `tests/hardening_test.cpp` fuzzes the parser with empty/whitespace/
   non-printable/overlong/proto-overflow inputs and asserts it never throws.

**Memory-boundary audit result:** the libconfig parser and control-socket path
use `std::string`/`std::string_view` + toml++ throughout; the only fixed buffers
(`char buf[512]`, `char chunk[1024]`) are read-bounded by `sizeof`, both
`strncpy` into `sun_path` are length-guarded and NUL-safe, and every manual index
in `guest_agent`/`control_server` is bounds-checked before dereference. No buffer
overflow was found.

## RESOLVED — health-failed guest with `restart = "never"` (was: left running)

**Surfaced in:** Phase 4 (dashboard showed `state=failed` for a guest whose QEMU
process was still alive and burning CPU).

**Original behavior:** In `Supervisor::handle_failure`, a guest whose health
checks failed (agent unresponsive past the threshold) with `restart = never` was
marked `VmState::Failed` but its QEMU process was **left running**, holding its
pinned cores and RAM with no automatic reclamation.

**Resolution (1.0 roadmap, Phase A):** chose interpretation B — "don't bring it
back, but do stop the broken one." `handle_failure` now, under `restart = never`:
1. issues `SIGKILL` to the QEMU process so its pinned CPU cores and memory are
   reclaimed immediately;
2. transitions to a distinct `VmState::HealthPanic` (wire: `health_panic`) so
   `hypercore list`/`status` and the dashboard (bold red) show exactly why the
   guest was terminated, rather than the ambiguous `failed`;
3. clears sampled stats, removes the pid file, and releases the SSH port.

The distinct state preserves the "operator visibility" benefit of interpretation
A (you know it health-panicked) while gaining the resource hygiene of B. See
`docs/protocol.md` for the state definition.

Note: a guest whose *process exits* (not a health-check failure) with
`restart = never` was already correctly left with no process; this change only
affects the health-check path (where the process was previously left alive).

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
