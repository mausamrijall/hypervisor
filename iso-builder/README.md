# iso-builder/ — Rufus-flashable ISO builder (Phase 6)

`build_iso.sh` produces a hybrid BIOS/UEFI ISO that boots straight into the
hypercore environment. Pipeline: stage a root filesystem (busybox + hypercored
+ hypercore + bundled QEMU + default config + `hypercore-init` as `/sbin/init`)
→ `mksquashfs` → tiny initramfs that `switch_root`s into the squashfs →
GRUB config → `grub-mkrescue`/`xorriso` packaging.

```sh
cmake --build build -j
./iso-builder/build_iso.sh --build-dir build --out build/hypercore.iso
```

Prerequisites, options, flashing instructions (Rufus / `dd`), and how to
test-boot the ISO in QEMU are in [../docs/iso.md](../docs/iso.md).

`testguest/` holds the throwaway busybox guest used by Phase 3 integration
tests — unrelated to the appliance ISO.
