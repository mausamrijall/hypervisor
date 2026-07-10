#!/usr/bin/env bash
# build_iso.sh — produce a Rufus-flashable hypercore ISO.
#
# Pipeline:
#   1. Assemble a root filesystem tree (staging/) with busybox userland,
#      hypercored + hypercore, the default config, and hypercore-init as
#      /sbin/init.
#   2. mksquashfs that tree  -> rootfs.squashfs  (compressed read-only root).
#   3. Build a tiny initramfs that finds the boot medium, mounts the squashfs,
#      and switch_root's into it.
#   4. Lay out the ISO tree (kernel, initramfs, squashfs, GRUB config).
#   5. grub-mkrescue -> hybrid BIOS/UEFI ISO, writable to USB with Rufus or dd.
#
# PREREQUISITES (see docs/iso.md): mksquashfs, cpio, gzip, grub-mkrescue,
# xorriso, and a kernel image. On a machine missing xorriso the final packaging
# step will fail with a clear message; every earlier stage still runs so you can
# inspect the staging tree and squashfs.
set -euo pipefail

# --- args -------------------------------------------------------------------
BUILD_DIR="${PWD}/build"          # where hypercored/hypercore were built
# Default kernel: prefer the running kernel, fall back to any vmlinuz in /boot.
_default_kernel="/boot/vmlinuz-$(uname -r)"
if [ ! -r "$_default_kernel" ]; then
  _default_kernel="$(find /boot -maxdepth 1 -name 'vmlinuz-*' -type f | sort -V | tail -1)"
fi
KERNEL="${_default_kernel}"
OUT="${PWD}/build/hypercore.iso"
CONFIG="${PWD}/config/hypervisor.cfg"
LABEL="HYPERCORE"
WORK="${PWD}/build/iso-work"

usage() { grep '^#' "$0" | sed 's/^# \{0,1\}//'; }
while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --kernel)    KERNEL="$2"; shift 2 ;;
    --config)    CONFIG="$2"; shift 2 ;;
    --out)       OUT="$2"; shift 2 ;;
    --work)      WORK="$2"; shift 2 ;;
    -h|--help)   usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

die() { echo "build_iso: $*" >&2; exit 1; }
log() { echo "build_iso: $*"; }

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
INIT_SRC="$SELF_DIR/../init/hypercore-init"

# --- preflight --------------------------------------------------------------
command -v mksquashfs >/dev/null || die "mksquashfs not installed (squashfs-tools)"
command -v cpio >/dev/null       || die "cpio not installed"
command -v gzip >/dev/null       || die "gzip not installed"
command -v busybox >/dev/null    || die "busybox not installed"
[ -r "$KERNEL" ]     || die "kernel not readable: $KERNEL (pass --kernel)"
[ -x "$BUILD_DIR/bin/hypercored" ] || die "hypercored not found in $BUILD_DIR/bin (build first)"
[ -x "$BUILD_DIR/bin/hypercore" ]  || die "hypercore not found in $BUILD_DIR/bin (build first)"
[ -r "$CONFIG" ]     || die "config not readable: $CONFIG"
[ -r "$INIT_SRC" ]   || die "init script not found: $INIT_SRC"

HAVE_GRUB_MKRESCUE=1
command -v grub-mkrescue >/dev/null || HAVE_GRUB_MKRESCUE=0
HAVE_XORRISO=1
command -v xorriso >/dev/null || HAVE_XORRISO=0

BB="$(command -v busybox)"
log "build-dir=$BUILD_DIR kernel=$KERNEL out=$OUT"

rm -rf "$WORK"
mkdir -p "$WORK"
STAGE="$WORK/staging"        # becomes the squashfs root
IRD="$WORK/initramfs"        # becomes the initramfs
ISO="$WORK/iso"              # becomes the ISO tree

# Copy a dynamic binary plus its shared libraries into a root tree.
copy_with_libs() {
  local bin="$1" root="$2" dest="$3"
  mkdir -p "$root$(dirname "$dest")"
  cp "$bin" "$root$dest"
  local paths
  paths="$(ldd "$bin" 2>/dev/null | grep -oE '/[^ ]+\.so[^ ]*' || true)"
  local p
  for p in $paths; do
    [ -r "$p" ] || continue
    mkdir -p "$root$(dirname "$p")"
    cp -n "$p" "$root$p"
  done
}

# ============================================================================
# 1. Staging root filesystem
# ============================================================================
log "assembling staging root"
mkdir -p "$STAGE"/{bin,sbin,etc/hypercore,proc,sys,dev,run,tmp,var/lib/hypercore,lib,lib64}

copy_with_libs "$BB" "$STAGE" /bin/busybox
chmod 0755 "$STAGE/bin/busybox"
# Relative applet symlinks (POSIX tools the init + daemon shell out to).
for app in sh mount umount cat echo sleep ls mkdir ln kill ps grep sed cut \
           basename dirname switch_root poweroff reboot; do
  ln -sf busybox "$STAGE/bin/$app"
done

# hypercore binaries.
copy_with_libs "$BUILD_DIR/bin/hypercored" "$STAGE" /sbin/hypercored
copy_with_libs "$BUILD_DIR/bin/hypercore"  "$STAGE" /bin/hypercore
chmod 0755 "$STAGE/sbin/hypercored" "$STAGE/bin/hypercore"

# QEMU: a real appliance ships qemu-system-x86_64 in the image. If present on
# the build host, bundle it (+libs); otherwise warn — the ISO still builds but
# guests won't launch until qemu is added.
if command -v qemu-system-x86_64 >/dev/null; then
  copy_with_libs "$(command -v qemu-system-x86_64)" "$STAGE" /usr/bin/qemu-system-x86_64
  chmod 0755 "$STAGE/usr/bin/qemu-system-x86_64"
  log "bundled qemu-system-x86_64"
else
  log "WARNING: qemu-system-x86_64 not on build host; not bundled (guests won't run)"
fi

# Unprivileged user for QEMU children (privilege separation). The daemon runs
# as root (PID 1) and drops each QEMU to this user via --qemu-user, so a guest
# escape cannot own host RAM as root. Provision a fixed uid/gid in the image so
# getpwnam("hypercore") resolves at boot; without it the daemon fail-closes and
# refuses to launch guests.
HC_UID=976
HC_GID=976
cat > "$STAGE/etc/passwd" <<PASSWD
root:x:0:0:root:/root:/bin/sh
hypercore:x:${HC_UID}:${HC_GID}:hypercore qemu:/nonexistent:/sbin/nologin
PASSWD
cat > "$STAGE/etc/group" <<GROUP
root:x:0:
kvm:x:104:hypercore
hypercore:x:${HC_GID}:
GROUP
mkdir -p "$STAGE/root"
chmod 0700 "$STAGE/root"

# Config + init.
cp "$CONFIG" "$STAGE/etc/hypercore/hypervisor.cfg"
cp "$INIT_SRC" "$STAGE/sbin/hypercore-init"
chmod 0755 "$STAGE/sbin/hypercore-init"
ln -sf hypercore-init "$STAGE/sbin/init"

# ============================================================================
# 2. squashfs the root
# ============================================================================
log "building rootfs.squashfs"
mksquashfs "$STAGE" "$WORK/rootfs.squashfs" -noappend -quiet -comp gzip

# ============================================================================
# 3. initramfs: find medium, mount squashfs, switch_root
# ============================================================================
log "building initramfs"
mkdir -p "$IRD"/{bin,proc,sys,dev,mnt,newroot}
copy_with_libs "$BB" "$IRD" /bin/busybox
for app in sh mount umount switch_root sleep echo ls cat losetup find mkdir; do
  ln -sf busybox "$IRD/bin/$app"
done

cat > "$IRD/init" <<'IRDINIT'
#!/bin/sh
# initramfs init: locate the hypercore medium, mount its squashfs root, and
# switch into it. Kept deliberately small; the real init is /sbin/init in the
# squashfs.
export PATH=/bin
mount -t proc  proc  /proc 2>/dev/null
mount -t sysfs sysfs /sys  2>/dev/null
mount -t devtmpfs dev /dev 2>/dev/null

LABEL_FILE="/rootfs.squashfs"   # path of the squashfs on the boot medium

# Scan block devices for the one carrying our squashfs. We try each read-only.
found=""
for i in $(seq 1 30); do
  for dev in /dev/sr0 /dev/sr1 $(ls /dev/sd* /dev/vd* /dev/nvme* 2>/dev/null); do
    [ -b "$dev" ] || continue
    mount -o ro "$dev" /mnt 2>/dev/null || continue
    if [ -f "/mnt$LABEL_FILE" ]; then found="$dev"; break; fi
    umount /mnt 2>/dev/null
  done
  [ -n "$found" ] && break
  sleep 1
done

if [ -z "$found" ]; then
  echo "hypercore initramfs: could not find boot medium with $LABEL_FILE"
  exec sh
fi

echo "hypercore initramfs: root medium = $found"
# Mount the squashfs read-only as the new root.
mount -t squashfs -o ro "/mnt$LABEL_FILE" /newroot 2>/dev/null || {
  echo "hypercore initramfs: failed to mount squashfs"; exec sh; }

# Hand off to the real init inside the squashfs.
exec switch_root /newroot /sbin/init
IRDINIT
chmod 0755 "$IRD/init"

( cd "$IRD" && find . -print0 | cpio --null -o -H newc 2>/dev/null ) \
  | gzip -9 > "$WORK/initramfs.cpio.gz"

# ============================================================================
# 4. ISO tree + GRUB config
# ============================================================================
log "laying out ISO tree"
mkdir -p "$ISO/boot/grub"
cp "$KERNEL" "$ISO/boot/vmlinuz"
cp "$WORK/initramfs.cpio.gz" "$ISO/boot/initramfs.cpio.gz"
cp "$WORK/rootfs.squashfs" "$ISO/rootfs.squashfs"

cat > "$ISO/boot/grub/grub.cfg" <<GRUBCFG
set timeout=3
set default=0

menuentry "hypercore" {
    linux /boot/vmlinuz quiet loglevel=3
    initrd /boot/initramfs.cpio.gz
}

menuentry "hypercore (debug shell)" {
    linux /boot/vmlinuz loglevel=7 hypercore.debug
    initrd /boot/initramfs.cpio.gz
}
GRUBCFG

# ============================================================================
# 5. Package the hybrid ISO
# ============================================================================
if [ "$HAVE_GRUB_MKRESCUE" = "1" ] && [ "$HAVE_XORRISO" = "1" ]; then
  log "packaging ISO with grub-mkrescue"
  grub-mkrescue -o "$OUT" "$ISO" --volid "$LABEL"
  log "done: $OUT ($(du -h "$OUT" | cut -f1))"
else
  log "SKIP final packaging: grub-mkrescue/xorriso not both present."
  log "  grub-mkrescue: $([ "$HAVE_GRUB_MKRESCUE" = 1 ] && echo yes || echo MISSING)"
  log "  xorriso:       $([ "$HAVE_XORRISO" = 1 ] && echo yes || echo MISSING)"
  log "  Staged tree, squashfs, and initramfs are ready under: $WORK"
  log "  Install xorriso (and grub-pc-bin for BIOS) then re-run to emit $OUT."
  exit 3
fi
