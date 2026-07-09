# hypercore boot sequence

hypercore boots headless from USB into a minimal Linux environment whose only
job is to run `hypercored` and hand the local console to the dashboard. This
document describes the boot flow and the design choices behind it.

## Why a custom PID 1, not systemd

hypercore is a **single-purpose appliance**, not a general-purpose OS. The
machine exists to run one daemon and its guests. A custom shell PID 1
(`init/hypercore-init`) is the right fit:

| | Custom PID 1 (chosen) | systemd |
|---|---|---|
| initramfs size | ~1 script + busybox | tens of MB, many units |
| boot time | mount + exec, sub-second | generators, dbus, journal |
| dependency surface | busybox `sh` | large, security-relevant |
| what we give up | we own supervision | unit management for free |

We only supervise **one** service (`hypercored`) plus the dashboard, so the
respawn loops in the init script are trivial to own — we don't need systemd's
dependency graph, socket activation, or cgroup delegation for a fixed
single-service workload. This directly serves the project goal: *Firecracker's
speed philosophy, minus the bloat.*

**Tradeoff, stated plainly:** we hand-roll service supervision (restart-on-exit)
instead of getting it from systemd. For one daemon this is ~10 lines; if the
service set ever grows substantially, revisit.

## Boot flow

```
firmware (BIOS/UEFI)
  └─ GRUB  (from the USB; Phase 6 builds this)
       └─ Linux kernel + initramfs
            └─ /init  ==  hypercore-init  (PID 1)
                 1. mount /proc /sys /dev /run /tmp, cgroup2
                 2. quiet the console (printk -> 1; cmdline `quiet loglevel=3`)
                 3. check /dev/kvm present (warn if not)
                 4. read config path from kernel cmdline (hypercore.config=…)
                 5. start hypercored under a respawn loop (background)
                 6. wait for the control socket to appear
                 7. exec the dashboard on /dev/console (respawned if it exits)
```

### 1. Mounts
`devtmpfs` on `/dev` gives us `/dev/kvm` and the virtio device nodes; `proc`
and `sysfs` are needed by both the daemon (capability detection, `/proc/<pid>`
sampling and affinity readback) and the guests. `cgroup2` is mounted for future
per-guest resource accounting.

### 2. Console quiet
`echo 1 > /proc/sys/kernel/printk` drops all but emergency kernel messages so
driver chatter doesn't scribble over the dashboard. The ISO's GRUB cmdline also
passes `quiet loglevel=3` (Phase 6) so early boot is quiet too.

### 3–4. Config discovery
Default config is `/etc/hypercore/hypervisor.cfg`. The kernel command line can
override it: `hypercore.config=/path/to.cfg`. `hypercore.debug` on the cmdline
drops to a shell instead of the dashboard (recovery).

### 5. Daemon supervision
`hypercored` runs in a `while true` respawn loop. If it exits for any reason we
log and restart after 2s. This is the supervision systemd would otherwise
provide.

### 6. Socket wait
Before launching the dashboard we poll for the control socket (up to ~5s) so the
dashboard's first `list` doesn't race daemon startup.

### 7. TTY handoff
The dashboard takes over `/dev/console`. It, too, is supervised: if the user
quits (`q`) or it crashes, PID 1 relaunches it after 1s. **PID 1 never exits** —
every path loops or drops to a recovery shell, because a PID 1 exit panics the
kernel.

## Kernel command-line options hypercore understands

| Option | Effect |
|--------|--------|
| `hypercore.config=PATH` | Use PATH as the hypervisor config. |
| `hypercore.debug` | Drop to a shell on the console instead of the dashboard. |
| `quiet loglevel=3` | (Standard) quiet kernel boot; set by the ISO's GRUB. |

## Recovery

If the dashboard is unusable, reboot and add `hypercore.debug` to the GRUB entry
(press `e` in the GRUB menu). You get a root shell on the console with
`hypercored` still supervised in the background, so `hypercore … list` etc. work
for debugging.

## Relationship to the ISO (Phase 6)

Phase 6's `build_iso.sh` installs this script as the initramfs `/init`, bundles
`hypercored` + `hypercore` + a default config, and writes the GRUB config with
the quiet cmdline. See [iso.md](iso.md).
