# Testing hypercore

hypercore has two tiers of tests, split by whether they need hardware
virtualization:

| Tier | What | Needs KVM? | Runs in CI? |
|------|------|:---------:|:-----------:|
| **Unit** | Config parse/validate, reconcile diff, CPU-list parsing | no | ✅ always |
| **Integration** | Real QEMU guest: launch, pin+readback, health, shutdown, adoption, path checks | **yes** (`/dev/kvm`) | ⏭️ **skipped** without `/dev/kvm` |

Everything is driven by CTest. The integration tests **skip cleanly** (they do
not fail) when the host lacks `/dev/kvm` or `qemu-system-x86_64`, so the same
`ctest` command is correct on a laptop with KVM and on a stock GitHub Actions
runner without it.

## Quick start

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Check what your host can do:

```sh
./build/bin/hostcaps
# {"kvm":true,"qemu":true,"can_run_guests":true,"qemu_path":"/usr/bin/qemu-system-x86_64",...}
```

`hostcaps` exits 0 when guests can run, 1 otherwise — it is the exact gate the
integration tests use.

## Unit tests (no KVM)

- `config_test` — parser + validator (Phase 2).
- `reconcile_test` — reconciliation diff and `Cpus_allowed_list` parsing. This
  is host-independent by construction: it feeds synthetic desired/actual state
  into the pure `reconcile()` function, so it never spawns a process and runs
  identically everywhere.

```sh
ctest --test-dir build -R 'config_test|reconcile_test' --output-on-failure
```

## Integration tests (require KVM)

These exercise the real runtime engine against a genuine QEMU guest — no mocks.
They cover:

- **3a** launch tracked by PID file (with a `-name` ownership marker),
- **3b** `sched_setaffinity` verified by reading back `/proc/<pid>/status`
  `Cpus_allowed_list`,
- **3c** graceful shutdown (guest-agent) and the forced SIGTERM/SIGKILL
  fallback on timeout,
- **3d** PID-file adoption across a daemon restart (no double-launch),
- **3e** launch-time path checks (missing image fails just that VM),
- **4** guest-agent health probe and restart policy (`never` / `on-failure` /
  `always`).

### Prerequisites

- `/dev/kvm` readable+writable by your user (`ls -l /dev/kvm`; add yourself to
  the `kvm` group or grant an ACL).
- `qemu-system-x86_64` installed.
- To build the throwaway guest: `busybox`, `cpio`, `gzip`, and a readable
  kernel at `/boot/vmlinuz-$(uname -r)`. **Nothing is downloaded** — the guest
  is assembled entirely from these already-installed pieces.

### The throwaway busybox guest

The integration tests boot a minimal busybox initramfs (no disk image) that
comes up in ~1s and speaks the real QEMU guest-agent protocol over
virtio-serial. CTest builds it automatically via a setup fixture, but you can
build it by hand:

```sh
./iso-builder/testguest/build-testguest.sh --out build/testguest
# produces build/testguest/{vmlinuz,initramfs.cpio.gz}
```

> **Note on the guest agent:** the real `qemu-ga` binary is normally installed
> inside a guest OS. Since this throwaway guest has no OS, its agent is a small
> shell responder (`iso-builder/testguest/`) that implements the guest-agent
> wire protocol (`guest-sync` / `guest-ping` / `guest-shutdown`). The daemon
> talks to it exactly as it would to the real agent; for testing it is actually
> preferable because we can make it stop responding on demand to exercise the
> unhealthy-threshold and restart-policy paths.

### Running just the integration tests

```sh
ctest --test-dir build -R integration_test --output-on-failure
# or directly, for verbose per-check output:
./build/bin/integration_test build/testguest
```

On a host without KVM this prints a `SKIP:` line and returns 77 (CTest's skip
code), so it shows as *Skipped*, not *Failed*.

## What runs where — summary

- **Local dev box with KVM:** all 11 tests run; integration takes ~25–30s.
- **CI (GitHub Actions, no `/dev/kvm`):** unit tests + daemon/CLI smoke tests
  run and must pass; `build_test_guest`, `integration_test`, and
  `host_capabilities` report *Skipped*. CTest still exits 0.

To reproduce the CI (no-KVM) behavior locally without giving up your `/dev/kvm`:

```sh
HYPERCORE_FORCE_NO_KVM=1 ctest --test-dir build
# integration_test + host_capabilities => Skipped; everything else passes
```

## Manual end-to-end smoke (optional)

Drive the live daemon and its control socket by hand:

```sh
# tiny disk so the daemon's normal (disk-boot) path launches a real QEMU
qemu-img create -f qcow2 /tmp/demo.qcow2 64M
cat > /tmp/demo.cfg <<'EOF'
[hypercore]
socket = "/tmp/hc.sock"
[vm.demo]
image = "/tmp/demo.qcow2"
disk_type = "qcow2"
cpus = [1]
memory = "128M"
network = "user"
restart = "never"
EOF

./build/bin/hypercored --config /tmp/demo.cfg --socket /tmp/hc.sock \
  --runtime-dir /tmp/hcrt &
printf '1 list\n'        | socat -t2 - UNIX-CONNECT:/tmp/hc.sock
printf '1 status demo\n' | socat -t2 - UNIX-CONNECT:/tmp/hc.sock

# reconcile preview against the running guest, touching nothing:
./build/bin/hypercored --reconcile --dry-run --config /tmp/demo.cfg \
  --runtime-dir /tmp/hcrt
```

See [protocol.md](protocol.md) for the full control-socket wire format.
