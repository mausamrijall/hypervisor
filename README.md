# hypercore

> A developer-first, Type-1 hypervisor that boots headless from a USB into a
> minimal Linux + KVM/QEMU environment. Near-native performance, near-zero boot
> overhead, and a CLI you actually want to use.
>
> **Firecracker's speed philosophy meets Proxmox's usability — minus the web UI
> and the bloat.**

[![ci](https://github.com/mausamrijall/hypervisor/actions/workflows/ci.yml/badge.svg)](https://github.com/mausamrijall/hypervisor/actions/workflows/ci.yml)

---

## What it is

hypercore boots a machine straight into a single-purpose appliance: a root
daemon (`hypercored`) that launches KVM/QEMU guests with verified CPU pinning,
reconciles desired-vs-actual state, health-checks guests via the QEMU guest
agent, and serves a Unix control socket. A thin CLI (`hypercore`) drives it —
`list`, `start`, `stop`, `logs`, `ssh`, and a live TTY dashboard. Ship it to a
USB stick as a bootable ISO and the box comes up running your VMs.

All six build phases are complete, plus a security-hardening pass:

| Phase | What | Status |
|-------|------|--------|
| 1 | Repo scaffolding, CMake build, CI, structured logging | ✅ |
| 2 | TOML config schema + parser + validator + unit tests | ✅ |
| 3 | Daemon: reconcile, QEMU launch, CPU pinning, control socket, health checks | ✅ |
| 4 | CLI subcommands + live TTY dashboard | ✅ |
| 5 | Minimal init (custom PID 1) + boot environment | ✅ |
| 6 | Rufus-flashable ISO builder | ✅ |
| — | Security hardening (SSH arg-injection fix, QEMU privilege separation, input hardening) | ✅ |

## Architecture at a glance

```
bare metal ── Linux + KVM
    hypercored (daemon, root)  ──▶  QEMU guests (virtio-net / virtio-fs)
        │  reconcile · pin · health-check · restart policy · drops QEMU to
        │  an unprivileged user before exec
        │  Unix socket  /run/hypercore.sock  (0600, owner-only)
    hypercore (CLI)            ──▶  list · start · stop · logs · ssh · dashboard
```

- **`hypercored`** (`src/` + `libcore/`) — the daemon. Parses config, reconciles
  desired vs actual VM state (diff-based, not launch-everything-on-boot),
  launches QEMU with KVM + explicit CPU pinning (verified via
  `/proc/<pid>/status` read-back), health-checks guests via the QEMU guest
  agent, applies restart policy, and serves a versioned Unix-socket control API.
  Each QEMU child drops root before `execvp`.
- **`hypercore`** (`cli/`) — a thin, stateless client and live TTY dashboard.
  Holds no VM state of its own; every command is an RPC to the daemon.

Full component map and the reasoning behind each tradeoff (TOML vs custom
parser, line-protocol vs protobuf, daemon vs monolith, custom PID 1 vs systemd)
is in [docs/architecture.md](docs/architecture.md).

## Repository layout

```
├── src/           hypercored entrypoint (daemon main)
├── libcore/       runtime engine: QEMU launch, pinning, QMP, health, reconcile,
│                  control socket, capabilities, privilege separation
├── libconfig/     TOML config parser + validator (typed, strict)
├── cli/           hypercore — CLI client + live dashboard
├── common/        shared headers (structured logging, version)
├── config/        default hypervisor.cfg
├── init/          hypercore-init — custom PID 1 for the booted appliance
├── iso-builder/   build_iso.sh (bootable ISO) + testguest/ (integration guest)
├── docs/          architecture, config schema, control protocol, boot, ISO,
│                  testing, design notes
├── tests/         CTest suite (unit + real-QEMU integration + hardening)
├── third_party/   vendored single headers (toml++, nlohmann/json)
└── .github/       CI workflow (GCC + Clang matrix)
```

## Build & run

Requires a C++20 compiler and CMake ≥ 3.16. No QEMU or ISO tooling is needed to
*build* the binaries.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Run the daemon and drive it with the CLI:

```sh
# Run the daemon (as root on a real host so it can pin CPUs and drop QEMU privs):
./build/bin/hypercored --config config/hypervisor.cfg

# Preview what reconciliation would do, touching nothing:
./build/bin/hypercored --reconcile --dry-run -c config/hypervisor.cfg

# CLI — talks to the daemon over the control socket:
./build/bin/hypercore --socket /run/hypercore.sock list
./build/bin/hypercore --socket /run/hypercore.sock status web
./build/bin/hypercore --socket /run/hypercore.sock start web
./build/bin/hypercore --socket /run/hypercore.sock dashboard   # live TTY view
./build/bin/hypercore --socket /run/hypercore.sock ssh web      # SSH into a guest
```

See [docs/testing.md](docs/testing.md) for running the real-QEMU integration
tests locally (they auto-skip on hosts without `/dev/kvm`).

## Building the bootable ISO

`iso-builder/build_iso.sh` produces a hybrid BIOS/UEFI ISO that boots straight
into the hypercore appliance and is flashable to USB with **Rufus** (Windows) or
`dd` (Linux/macOS).

**Prerequisites** (Debian/Ubuntu package names):

```sh
sudo apt-get install -y squashfs-tools cpio gzip busybox-static \
                        grub-common grub-pc-bin xorriso qemu-system-x86
```

**Build:**

```sh
# 1. Build the hypercore binaries first.
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j

# 2. Build the ISO.
./iso-builder/build_iso.sh --build-dir build --out build/hypercore.iso
```

The script stages a root filesystem (busybox + `hypercored` + `hypercore` +
bundled QEMU + default config + the `hypercore` unprivileged user + the custom
PID 1), `mksquashfs`es it, builds a tiny initramfs that `switch_root`s into the
squashfs, generates a GRUB config (normal + a `hypercore.debug` recovery entry),
and packages it all with `grub-mkrescue`.

> If `xorriso`/`grub-mkrescue` aren't installed, the script still completes every
> earlier stage and leaves the staged tree, squashfs, and initramfs under
> `build/iso-work/` so you can finish packaging on a machine that has them.

**Flash to USB:**

```sh
# Linux/macOS — make VERY sure /dev/sdX is the USB stick:
sudo dd if=build/hypercore.iso of=/dev/sdX bs=4M status=progress oflag=sync
```

On Windows, point Rufus at `hypercore.iso` and write in "DD Image" mode. Boot the
target from USB; GRUB's default entry brings up the dashboard. Full options,
flashing details, and how to test-boot the ISO in a nested VM are in
[docs/iso.md](docs/iso.md); the boot sequence is documented in
[docs/boot.md](docs/boot.md).

## Configuration

Config is [TOML](https://toml.io). A worked default lives at
[`config/hypervisor.cfg`](config/hypervisor.cfg); the normative schema is
[`docs/schema.md`](docs/schema.md). Per-VM you can set image path, disk type
(raw/qcow2), explicit CPU pinning, RAM, network mode (bridge/user/virtiofs),
restart policy, and the guest-agent socket path.

## Security

- The control socket is `0600` (owner-only), created atomically via `umask`
  before `bind()`.
- Each QEMU guest runs as an **unprivileged user** — the daemon drops root
  (`setgroups`/`setgid`/`setuid`, verified) in the forked child before `execvp`,
  and fails closed if it can't. A guest escape does not yield root over host RAM.
- Guest-agent output is treated as **untrusted**: the guest-reported IP is
  strictly validated (`inet_pton`) before it can reach the operator's `ssh`
  invocation, and `ssh` is invoked with option-injection defenses.
- The control-socket request parser is bounded and panic-safe against malformed
  input.

Details and the audit report are in [docs/design-notes.md](docs/design-notes.md).

## Scope (v1)

- **Linux guests only** (virtio drivers). Windows/FreeBSD are explicitly out of
  scope for v1.
- **No web UI. No external orchestration dependencies.** CLI + dashboard only.

## Documentation

| Doc | Contents |
|-----|----------|
| [docs/architecture.md](docs/architecture.md) | Component map + design tradeoffs |
| [docs/schema.md](docs/schema.md) | Config schema + validation rules |
| [docs/protocol.md](docs/protocol.md) | Control-socket wire protocol |
| [docs/boot.md](docs/boot.md) | Boot sequence + init design |
| [docs/iso.md](docs/iso.md) | ISO build + flashing |
| [docs/testing.md](docs/testing.md) | Running unit + integration tests |
| [docs/design-notes.md](docs/design-notes.md) | Decisions, open questions, security fixes |

## License

Apache-2.0 — see [LICENSE](LICENSE). Chosen for the explicit patent grant,
appropriate for systems/infrastructure software.

## Contributing

CI builds the daemon and CLI under both GCC and Clang and runs the test suite on
every push and PR. Keep each change building and testable — that's the core
project constraint.
