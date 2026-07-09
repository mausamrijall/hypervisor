# hypercore architecture

hypercore is a **Type-1 (bare-metal) hypervisor** delivered as a bootable USB
image. It boots a minimal Linux environment whose sole job is to run
KVM/QEMU-backed guests, managed by a single daemon and driven from a CLI. No
web UI, no external orchestration, no agent fleet.

The design north star: **Firecracker's "strip everything that isn't the VM"
speed philosophy, with Proxmox-grade day-to-day usability — minus the bloat.**

```
                       bare metal
  ┌─────────────────────────────────────────────────────────┐
  │  Linux kernel + KVM                                       │
  │                                                          │
  │   ┌──────────────┐   reconcile    ┌───────────────────┐  │
  │   │  hypercored  │───────────────▶│ QEMU  (guest A)   │  │
  │   │  (daemon)    │  pin+launch    │  virtio-net/-fs   │  │
  │   │              │◀──────────────▶│  guest agent sock │  │
  │   │  - config    │  health check  └───────────────────┘  │
  │   │  - reconcile │                ┌───────────────────┐  │
  │   │  - QEMU mgr  │───────────────▶│ QEMU  (guest B)   │  │
  │   │  - ctl socket│                └───────────────────┘  │
  │   └──────┬───────┘                                        │
  │          │ Unix domain socket (/run/hypercore.sock)       │
  │   ┌──────┴───────┐                                        │
  │   │ hypercore    │  list/start/stop/logs/ssh/dashboard    │
  │   │ (CLI client) │                                        │
  │   └──────────────┘                                        │
  └─────────────────────────────────────────────────────────┘
```

## Components

| Path          | Artifact     | Role |
|---------------|--------------|------|
| `src/`        | `hypercored` | The daemon. Owns config, VM lifecycle, CPU pinning, health checks, and the control socket. |
| `cli/`        | `hypercore`  | Thin client. Talks to the daemon socket; renders the live TTY dashboard. Holds no state itself. |
| `common/`     | headers      | Shared structured logging + version. |
| `config/`     | data         | Default `hypervisor.cfg` and schema reference. |
| `init/`       | scripts      | Boot-environment init that starts the daemon and hands off to the dashboard. |
| `iso-builder/`| scripts      | Produces the Rufus-flashable ISO. |

## Design decisions & tradeoffs

These are the places where a choice was made; each is called out because the
project brief asks for justification rather than silent picks.

### Daemon + thin CLI (not a monolith)

A long-lived daemon is required anyway for health checks and restart policy, so
the CLI stays stateless and just RPCs into it. This keeps the dashboard, `ssh`,
and scripting all going through one enforced code path (the socket API) instead
of two ways to touch VMs.

### Config: TOML vs a custom parser {#config-toml-vs-custom}

**Decision: TOML via a small, vendored, dependency-free library.**

The brief explicitly prefers this, and it's the right call: a hypervisor's
config is security-relevant (it decides what runs with what resources). A
hand-rolled parser is a classic source of subtle edge-case bugs — quoting,
escaping, number formats, nesting. TOML is a good fit for typed, tabular,
per-VM config, and a header-only library (e.g. `toml++`) adds no runtime deps,
which matters in a minimal initramfs. **Tradeoff:** we take on one vendored
dependency and slightly larger build. Accepted — the alternative trades a
one-time dependency for a permanent correctness/security liability.

### Control channel: line-protocol vs protobuf {#control-line-vs-protobuf}

**Decision (Phase 3, final): a line-delimited request grammar with ndjson
responses.** Full spec in [protocol.md](protocol.md).

The API is tiny (`list`, `start`, `stop`, `status`, `reload`), local-only (Unix
socket, same host, same trust domain), and low frequency. Protobuf buys schema
evolution and speed we don't need, at the cost of a codegen toolchain and a
generated-code dependency in the initramfs. Requests are a flat, whitespace-
delimited line (`<proto> <command> [arg]`) — tokenized with a split + allow-list
check, **no recursive parser on the privileged input path**. Responses are
ndjson so the CLI can machine-read structured status. The asymmetry is
deliberate: the daemon only ever *writes* JSON (safe), never parses it. Every
message carries a `proto` version so the CLI can detect a mismatch. **Tradeoff:**
we give up rigid schema versioning; mitigated by the explicit version field.

### Logging: structured, zero-dependency {#logging}

`common/hypercore/log.hpp` emits leveled, key/value records in either `logfmt`
(human/TTY) or line-delimited JSON (capture), selectable at runtime because the
same binary runs both interactively and headless at boot. No `spdlog`/`fmt`
dependency — C++20 `<format>` suffices in a minimal environment.

## Scope guardrails (v1)

- **Linux guests only.** virtio-net / virtio-fs / virtio-blk drivers assumed.
  Windows/FreeBSD are explicitly out of scope; a request for them will be
  flagged, not quietly absorbed.
- **No web UI. No external orchestration.** The CLI + dashboard are the
  interface.
- **Every phase builds and is VM-testable before the next begins.**

## Build & phase status

| Phase | Scope | Status |
|-------|-------|--------|
| 1 | Repo scaffolding, CMake, CI, structured logging, runnable skeletons | ✅ done |
| 2 | TOML config schema + parser + validator + unit tests | ✅ done |
| 3 | Daemon: reconcile, QEMU launch, pinning, control socket, health checks | ✅ done |
| 4 | CLI subcommands + live TTY dashboard | ✅ done |
| 5 | init + boot environment | — |
| 6 | ISO builder | — |

## Config module (Phase 2)

`libconfig/` is a standalone static library (linked by both the daemon and the
tests) with a strict **parse / validate split**:

- **Parser** (`parser.cpp`, over vendored toml++): TOML text → the strongly
  typed structs in `types.hpp` (one `VmConfig` per guest — never a generic
  key/value map). It reports *value-level* problems: syntax/type errors (E1),
  unknown keys (E10, strict by design so typos can't silently no-op), bad enum
  tokens (E5), and malformed memory sizes (E8).
- **Validator** (`validator.cpp`): a separate pass over the typed structs that
  reports *set-level* problems and accumulates **all** of them rather than
  stopping at the first — missing required fields (E2), bad names (E3),
  duplicates (E4), bad CPU lists (E6), **CPU index ≥ host core count (E7)**,
  virtiofs-without-share (E9), plus warnings for **CPU pin overlap (W1)**,
  **RAM overcommit (W2)**, no-VMs (W3), and agentless restart (W4).

Host-dependent checks (E7, W2) compare against an injected `HostInfo` (core
count, physical RAM), so unit tests pin a synthetic "4 cores, 8 GiB" host and
assert deterministic outcomes regardless of the CI machine. See
[schema.md](schema.md) for the full rule table.
