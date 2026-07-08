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

**Leaning: a simple line-delimited protocol (likely newline-delimited JSON)
over the Unix socket. Final decision recorded in Phase 3.**

Rationale: the API is tiny (`list`, `start`, `stop`, `status`, `logs`,
`reload`), local-only (Unix socket, same host, same trust domain), and low
frequency. Protobuf buys schema evolution and speed we don't need here, at the
cost of a codegen toolchain and a heavier dependency in the initramfs. A
line protocol is trivially debuggable with `socat`/`nc`, which fits the
developer-first ethos. **Tradeoff:** we give up rigid schema versioning; we
mitigate with an explicit `version` field in every message.

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
| 2 | TOML config schema + parser + unit tests | ⏳ next |
| 3 | Daemon: reconcile, QEMU launch, pinning, control socket, health checks | — |
| 4 | CLI subcommands + live TTY dashboard | — |
| 5 | init + boot environment | — |
| 6 | ISO builder | — |
