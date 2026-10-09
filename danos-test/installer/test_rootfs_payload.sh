#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/danos-rootfs-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
src="$WORK/source"
mkdir -p "$src"/{etc,usr/bin,boot,proc,sys,dev,run,tmp,var/tmp,var/lib/dbus,var/lib/dpkg} "$WORK/extracted"
printf 'Package: fixture\nStatus: install ok installed\nArchitecture: amd64\nVersion: 1\n' > "$src/var/lib/dpkg/status"
printf 'ID=debian\n' > "$src/etc/os-release"
for file in usr/bin/danos-mgrd usr/bin/vpp boot/vmlinuz-test boot/initrd.img-test; do
    printf 'fixture\n' > "$src/$file"
done
chmod 0751 "$src/usr/bin/danos-mgrd"
ln "$src/usr/bin/danos-mgrd" "$src/usr/bin/hardlink"
ln -s danos-mgrd "$src/usr/bin/symlink"
printf 'private identity\n' > "$src/etc/machine-id"
printf 'transient\n' > "$src/run/socket-placeholder"
bash "$ROOT/danos-test/installer/build_rootfs_payload.sh" "$src" "$WORK/output"
tar -xzf "$WORK/output/rootfs.tar.gz" -C "$WORK/extracted"
test "$(stat -c '%a' "$WORK/extracted/usr/bin/danos-mgrd")" = 751
test "$(stat -c '%i' "$WORK/extracted/usr/bin/danos-mgrd")" = "$(stat -c '%i' "$WORK/extracted/usr/bin/hardlink")"
test "$(readlink "$WORK/extracted/usr/bin/symlink")" = danos-mgrd
test ! -e "$WORK/extracted/etc/machine-id"
test ! -e "$WORK/extracted/run/socket-placeholder"
digest=$(sha256sum "$WORK/output/rootfs.tar.gz" | awk '{print $1}')
grep -qx "DANOS_ROOTFS_SHA256=$digest" "$WORK/output/rootfs.manifest"
if bash "$ROOT/danos-test/installer/build_rootfs_payload.sh" "$src" "$WORK/output"; then exit 1; fi
if bash "$ROOT/danos-test/installer/build_rootfs_payload.sh" / "$WORK/host-root"; then exit 1; fi
if bash "$ROOT/danos-test/installer/build_rootfs_payload.sh" "$src" "$src/output"; then exit 1; fi
echo 'Rootfs packaging fixture tests PASS (privileged xattr/owner qualification pending)'
