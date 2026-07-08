# iso-builder/ — Rufus-flashable ISO builder (Phase 6)

Not yet implemented. This directory will hold the scripts that assemble the
bootable image:

- a kernel + initramfs (built via `mksquashfs` for the root, `cpio` for the
  early userspace),
- the `hypercored` and `hypercore` binaries from `build/bin/`,
- the default `config/hypervisor.cfg`,
- a GRUB config, packaged into a hybrid ISO with `xorriso` so it is both
  BIOS- and UEFI-bootable and writable to USB with Rufus (or `dd`).

Build tooling (`xorriso`, `mksquashfs`, `grub-mkrescue`) is a Phase 6 concern
and deliberately not a dependency of the `hypercored`/`hypercore` build.
