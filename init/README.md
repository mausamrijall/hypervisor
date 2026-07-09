# init/ — boot-environment init (Phase 5)

`hypercore-init` is PID 1 for the booted hypercore environment. It mounts the
pseudo-filesystems, quiets the kernel console, starts `hypercored` under a
respawn loop, waits for the control socket, and hands the local TTY to the
live dashboard (also respawned).

**Why a custom PID 1 rather than systemd:** hypercore is a single-purpose
appliance; a ~100-line shell PID 1 gives an appliance exactly what it needs
without systemd's size, boot cost, and dependency surface. Full justification
and the complete boot flow are in [../docs/boot.md](../docs/boot.md).

Kernel cmdline options: `hypercore.config=PATH`, `hypercore.debug` (recovery
shell). The ISO builder (Phase 6) installs this as the initramfs `/init`.
