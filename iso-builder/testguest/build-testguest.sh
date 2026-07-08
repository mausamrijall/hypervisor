#!/usr/bin/env bash
# build-testguest.sh — assemble the throwaway busybox guest for Phase 3
# integration tests.
#
# This is NOT a real VM example. It is the smallest thing that boots under
# QEMU fast and predictably so we can test the daemon's launch / CPU-pin /
# health-check / graceful-shutdown paths against a genuine guest.
#
# Output (into --out DIR, default ./build/testguest):
#   vmlinuz          the host kernel, copied (virtio-console is built-in)
#   initramfs.cpio.gz  busybox + libs + init + guest-agent responder
#   guestinfo.env    KERNEL=/... INITRAMFS=/... for the test harness to source
#
# DEPENDENCIES: only what is already installed — busybox, cpio, gzip, and a
# readable host kernel at /boot/vmlinuz-$(uname -r). NOTHING is downloaded.
# If any of these is missing the script exits non-zero with a clear message so
# the caller can skip integration tests rather than fail them.
set -euo pipefail

OUT="${PWD}/build/testguest"
KERNEL_SRC="/boot/vmlinuz-$(uname -r)"
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --kernel) KERNEL_SRC="$2"; shift 2 ;;
    -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

die() { echo "build-testguest: $*" >&2; exit 1; }

# --- dependency preflight (fail clearly, don't half-build) -------------------
command -v busybox >/dev/null || die "busybox not installed"
command -v cpio    >/dev/null || die "cpio not installed"
command -v gzip    >/dev/null || die "gzip not installed"
[ -r "$KERNEL_SRC" ] || die "kernel not readable: $KERNEL_SRC (pass --kernel)"

BB="$(command -v busybox)"

echo "build-testguest: out=$OUT kernel=$KERNEL_SRC busybox=$BB"
rm -rf "$OUT"
mkdir -p "$OUT"
ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT

# --- initramfs skeleton ------------------------------------------------------
mkdir -p "$ROOT"/{bin,sbin,proc,sys,dev,etc,tmp,lib,lib64}

cp "$BB" "$ROOT/bin/busybox"
chmod 0755 "$ROOT/bin/busybox"

# busybox on this host is dynamically linked; bundle exactly its resolved
# shared libraries (copied from the installed system — nothing fetched). If
# busybox were static this loop simply copies nothing and still works.
copy_libs() {
  local bin="$1"
  # Extract absolute .so paths from ldd output. Note: `grep|head` returns
  # non-zero on lines with no match (e.g. the vdso line); with `set -o
  # pipefail + errexit` that would abort the script, so each extraction is
  # guarded with `|| true`.
  local paths
  paths="$(ldd "$bin" 2>/dev/null | grep -oE '/[^ ]+\.so[^ ]*' || true)"
  local path dest
  for path in $paths; do
    [ -r "$path" ] || continue
    dest="$ROOT$path"
    mkdir -p "$(dirname "$dest")"
    cp -n "$path" "$dest"
  done
}
copy_libs "$BB"

# Applet symlinks. We deliberately do NOT use `busybox --install` here: it can
# emit absolute symlinks pointing at the (temporary) build directory, which are
# dangling inside the guest. Instead create RELATIVE symlinks for exactly the
# applets init and the agent responder use.
for app in sh mount umount cat echo sleep poweroff kill sed ls mkdir ln; do
  ln -sf busybox "$ROOT/bin/$app"
done

# --- guest-agent responder ---------------------------------------------------
# Speaks the real QEMU guest-agent JSON-RPC wire protocol (guest-sync-delimited
# / guest-ping / guest-shutdown) over the virtio-serial port. It is a TEST
# FIXTURE standing in for qemu-ga (which isn't installed on the build host),
# not a reimplementation shipped to users. The daemon talks to it exactly as it
# would to the real agent.
cat > "$ROOT/bin/guest-agentd" <<'AGENT'
#!/bin/sh
# Minimal guest-agent responder. Reads one JSON object per line from the
# virtio-serial port and writes one JSON response per line.
#
# A virtio-serial port returns EOF on read whenever no host client is
# connected, so a single `while read` loop would exit instantly at boot. We
# therefore wrap it in an outer reopen loop: each time the host disconnects
# (read hits EOF) we re-open the port and wait for the next connection. The
# sleep bounds the reopen spin to ~1/s (adequate for health-check latency).
PORT="/dev/virtio-ports/org.qemu.guest_agent.0"
# Wait for the port symlink (init creates it from sysfs). Integer sleeps only:
# busybox `sleep` is not guaranteed to accept fractional seconds.
i=0; while [ ! -e "$PORT" ] && [ $i -lt 20 ]; do sleep 1; i=$((i+1)); done
[ -e "$PORT" ] || exit 0

while true; do
  if exec 3<>"$PORT"; then
    while IFS= read -r line <&3; do
      case "$line" in
        *guest-sync-delimited*|*guest-sync*)
          # Echo back the id the host sent so it can resynchronize.
          id="$(echo "$line" | sed -n 's/.*"id"[ ]*:[ ]*\([0-9]*\).*/\1/p')"
          [ -n "$id" ] || id=0
          printf '{"return":%s}\n' "$id" >&3
          ;;
        *guest-ping*)
          printf '{"return":{}}\n' >&3
          ;;
        *guest-shutdown*)
          printf '{"return":{}}\n' >&3
          # Give the host a moment to read the ack, then power off cleanly.
          (sleep 1; poweroff -f) &
          ;;
        *)
          printf '{"error":{"class":"GenericError","desc":"unsupported"}}\n' >&3
          ;;
      esac
    done
    exec 3<&- 3>&-
  fi
  sleep 1
done
AGENT
chmod 0755 "$ROOT/bin/guest-agentd"

# --- init (PID 1) ------------------------------------------------------------
cat > "$ROOT/init" <<'INIT'
#!/bin/sh
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sys /sys 2>/dev/null
mount -t devtmpfs dev /dev 2>/dev/null
# There is no udev in this minimal guest, so the friendly
# /dev/virtio-ports/<name> symlinks that the guest agent expects are not
# created automatically. Recreate them from sysfs the way udev's
# 60-serial.rules does: for each virtio port, read its name and symlink it to
# the matching /dev/vportNpM device node.
mkdir -p /dev/virtio-ports
for p in /sys/class/virtio-ports/*; do
  [ -e "$p/name" ] || continue
  name="$(cat "$p/name")"
  dev="${p##*/}"                  # e.g. vport0p1 (avoids needing `basename`)
  [ -n "$name" ] && [ -e "/dev/$dev" ] && ln -sf "/dev/$dev" "/dev/virtio-ports/$name"
done
# Start the guest-agent responder in the background.
/bin/guest-agentd &
# Readiness marker the test harness greps for on the serial console.
echo "HYPERCORE_GUEST_READY"
# Idle forever; the host stops us via guest-agent shutdown or SIGKILL.
while true; do sleep 1000; done
INIT
chmod 0755 "$ROOT/init"

# --- pack --------------------------------------------------------------------
( cd "$ROOT" && find . -print0 | cpio --null -o -H newc 2>/dev/null ) \
  | gzip -9 > "$OUT/initramfs.cpio.gz"
cp "$KERNEL_SRC" "$OUT/vmlinuz"

cat > "$OUT/guestinfo.env" <<EOF
KERNEL=$OUT/vmlinuz
INITRAMFS=$OUT/initramfs.cpio.gz
EOF

echo "build-testguest: done"
echo "  kernel:    $OUT/vmlinuz ($(du -h "$OUT/vmlinuz" | cut -f1))"
echo "  initramfs: $OUT/initramfs.cpio.gz ($(du -h "$OUT/initramfs.cpio.gz" | cut -f1))"
