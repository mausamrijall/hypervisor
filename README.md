# hypercore

> A developer-first, Type-1 hypervisor that boots headless from a USB into a
> minimal Linux + KVM/QEMU environment. Near-native performance, near-zero boot
> overhead, and a CLI you actually want to use.
>
> **Firecracker's speed philosophy meets Proxmox's usability — minus the web UI
> and the bloat.**

[![ci](https://github.com/OWNER/hypercore/actions/workflows/ci.yml/badge.svg)](https://github.com/OWNER/hypercore/actions/workflows/ci.yml)

---

## Status: Phase 1 (scaffolding) — building incrementally

hypercore is being built in reviewable phases. Each phase compiles, runs, and
is testable before the next begins. See
[docs/architecture.md](docs/architecture.md) for the design and the
[phase status table](docs/architecture.md#build--phase-status).

| Phase | What | Status |
|-------|------|--------|
| 1 | Repo scaffolding, CMake build, CI, structured logging, runnable skeletons | ✅ |
| 2 | TOML config schema + parser + validator + unit tests | ✅ |
| 3 | Daemon: state reconcile, QEMU launch, CPU pinning, control socket, health checks | ✅ |
| 4 | CLI subcommands + live TTY dashboard | ✅ |
| 5 | Minimal init + boot environment | ✅ |
| 6 | Rufus-flashable ISO builder | — |

## Architecture at a glance

```
bare metal ── Linux + KVM
    hypercored (daemon)  ──▶  QEMU guests (virtio-net / virtio-fs)
        │  reconcile · pin · health-check · restart policy
        │  Unix socket  /run/hypercore.sock
    hypercore (CLI)      ──▶  list · start · stop · logs · ssh · dashboard
```

- **`hypercored`** (`src/`) — the daemon. Parses config, reconciles desired vs
  actual VM state (diff-based, not launch-everything-on-boot), launches QEMU
  with KVM + explicit CPU pinning, health-checks guests via the QEMU guest
  agent, and serves a Unix-socket control API.
- **`hypercore`** (`cli/`) — a thin, stateless client and live TTY dashboard.

Full component map and the reasoning behind each tradeoff (TOML vs custom
parser, line-protocol vs protobuf, daemon vs monolith) is in
[docs/architecture.md](docs/architecture.md).

## Repository layout

```
├── src/           hypercored — the daemon (C++)
├── cli/           hypercore  — thin CLI client
├── common/        shared headers (structured logging, version)
├── config/        default hypervisor.cfg + schema
├── init/          boot-environment init (Phase 5)
├── iso-builder/   Rufus-flashable ISO build scripts (Phase 6)
├── docs/          architecture + config schema
├── tests/         CTest suite (unit tests land in Phase 2)
└── .github/       CI workflow
```

## Build & run

Requires a C++20 compiler and CMake ≥ 3.16. **No QEMU or ISO tooling is needed
to build** — those are runtime/packaging concerns for later phases.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Try the skeletons:

```sh
./build/bin/hypercored --config config/hypervisor.cfg           # run the daemon
./build/bin/hypercored --reconcile --dry-run -c config/hypervisor.cfg  # plan only

# CLI (talks to the daemon over the control socket):
./build/bin/hypercore --socket /run/hypercore.sock list
./build/bin/hypercore --socket /run/hypercore.sock status web
./build/bin/hypercore --socket /run/hypercore.sock dashboard     # live TTY view
./build/bin/hypercore --socket /run/hypercore.sock ssh web        # exec ssh
```

The daemon launches KVM guests with verified CPU pinning, reconciles desired vs
actual state, and serves the control socket; the CLI is a pure client that
renders daemon state (list/status/start/stop/logs/ssh + a live dashboard). See
[docs/testing.md](docs/testing.md) for how to run it end to end.

## Scope (v1)

- **Linux guests only** (virtio drivers). Windows/FreeBSD are explicitly out of
  scope for v1 and will be flagged, not silently added.
- **No web UI. No external orchestration dependencies.** CLI + dashboard only.

## Configuration

Config is [TOML](https://toml.io). A worked default lives at
[`config/hypervisor.cfg`](config/hypervisor.cfg); the normative schema is
[`docs/schema.md`](docs/schema.md). Per-VM you can set image path, disk type
(raw/qcow2), explicit CPU pinning, RAM, network mode (bridge/user/virtiofs),
restart policy, and the guest-agent socket path.

## License

Apache-2.0 — see [LICENSE](LICENSE). Chosen for the explicit patent grant,
appropriate for systems/infrastructure software.

## Contributing

CI builds the daemon and CLI under both GCC and Clang and runs the test suite on
every push and PR. Keep each change building and testable — that's the core
project constraint.
