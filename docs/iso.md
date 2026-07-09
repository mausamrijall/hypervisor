# Building & flashing the hypercore ISO

`iso-builder/build_iso.sh` produces a hybrid BIOS/UEFI ISO that boots straight
into the hypercore environment (Phase 5 init → dashboard). It is writable to a
USB stick with Rufus (Windows) or `dd` (Linux/macOS).

## Prerequisites

Install on the build machine:

| Tool | Package (Debian/Ubuntu) | Used for |
|------|-------------------------|----------|
| `mksquashfs` | `squashfs-tools` | compress the root filesystem |
| `cpio`, `gzip` | `cpio`, `gzip` | build the initramfs |
| `busybox` | `busybox-static` | userland in the image |
| `grub-mkrescue` | `grub-common` | assemble the bootable image |
| `xorriso` | `xorriso` | emit the ISO (grub-mkrescue backend) |
| BIOS boot support | `grub-pc-bin` | legacy-BIOS El Torito (UEFI needs `grub-efi-amd64-bin`) |

A readable kernel image is also required (default `/boot/vmlinuz-$(uname -r)`).

> **Bundled QEMU:** if `qemu-system-x86_64` is on the build host it is copied
> into the image (with its libraries) so guests can run on the booted appliance.
> If absent, the ISO still builds but the appliance can't launch guests until
> QEMU is added — the builder prints a warning.

## Build

First build the hypercore binaries, then the ISO:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j

./iso-builder/build_iso.sh --build-dir build --out build/hypercore.iso
```

Options: `--kernel PATH` (default `/boot/vmlinuz-$(uname -r)`), `--config PATH`
(default `config/hypervisor.cfg`), `--out PATH`, `--work DIR`.

### What it does

1. **Staging root** — busybox userland + `hypercored` (`/sbin`), `hypercore`
   (`/bin`), bundled QEMU, the default config (`/etc/hypercore/`), and
   `hypercore-init` as `/sbin/init` (via symlink).
2. **`rootfs.squashfs`** — the staging tree, squashed read-only.
3. **initramfs** — a tiny early userspace that scans block devices for the one
   carrying `rootfs.squashfs`, mounts it, and `switch_root`s into `/sbin/init`.
4. **ISO tree** — kernel, initramfs, squashfs, and a GRUB config with two
   entries: normal (`quiet loglevel=3`) and a debug shell (`hypercore.debug`).
5. **`grub-mkrescue`** — packages the hybrid ISO.

If `grub-mkrescue`/`xorriso` are missing, the builder completes stages 1–4,
leaves the staged tree + squashfs + initramfs under `build/iso-work/`, and exits
3 with a message — so you can inspect artifacts or finish packaging on a machine
that has xorriso.

## Output

```
build/hypercore.iso          the flashable image
build/iso-work/              intermediate artifacts (staging/, squashfs, initramfs, iso/)
```

## Flashing to USB

**Rufus (Windows):** select `hypercore.iso`, choose your USB device, keep the
default "DD Image" / "Write in DD mode" when prompted (the image is a hybrid
ISO), and write.

**Linux/macOS (`dd`):**

```sh
# find your USB device first (e.g. /dev/sdX); make VERY sure it's the stick.
sudo dd if=build/hypercore.iso of=/dev/sdX bs=4M status=progress oflag=sync
```

Boot the target machine from the USB. GRUB shows the hypercore menu; the default
entry boots into the dashboard. Pick "hypercore (debug shell)" (or press `e` and
add `hypercore.debug`) for a recovery shell.

## Boot flow

Once flashed and booted, see [boot.md](boot.md) for the full sequence
(initramfs → squashfs → `hypercore-init` PID 1 → `hypercored` + dashboard).

## Testing the ISO in QEMU (optional)

If your build host has KVM you can boot the ISO itself in a VM (nested):

```sh
qemu-system-x86_64 -enable-kvm -m 2048 -cdrom build/hypercore.iso -serial mon:stdio
```

(Nested KVM must be enabled on the host for guests inside the appliance to
accelerate.)
