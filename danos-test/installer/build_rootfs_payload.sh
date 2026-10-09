#!/usr/bin/env bash
# Package an offline, quiescent root filesystem; never read a running host root.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
test "$#" -eq 2 || { echo 'Usage: bash build_rootfs_payload.sh ROOTFS NEW_OUTPUT_DIRECTORY' >&2; exit 2; }
source_root=$(realpath "$1")
output=$(realpath -m "$2")
test "$source_root" != / || { echo 'Refusing host root filesystem' >&2; exit 2; }
test -d "$source_root" || exit 2
case "$output/" in "$source_root/"*) echo 'Output must be outside rootfs' >&2; exit 2 ;; esac
test ! -e "$output" || { echo 'Output already exists; refusing overwrite' >&2; exit 2; }
for required in etc/os-release usr/bin/danos-mgrd usr/bin/vpp var/lib/dpkg/status; do
    test -f "$source_root/$required" || { echo "Missing runtime file: $required" >&2; exit 2; }
done
compgen -G "$source_root/boot/vmlinuz-*" >/dev/null || { echo 'Missing installed kernel' >&2; exit 2; }
compgen -G "$source_root/boot/initrd.img-*" >/dev/null || { echo 'Missing installed initramfs' >&2; exit 2; }
tar --version | head -n 1 | grep -q 'GNU tar' || { echo 'GNU tar required' >&2; exit 2; }
mkdir -p "$output"
cp "$source_root/var/lib/dpkg/status" "$output/packages.status"
cp "$source_root/etc/os-release" "$output/os-release"
packages_digest=$(sha256sum "$output/packages.status" | awk '{print $1}')
os_digest=$(sha256sum "$output/os-release" | awk '{print $1}')
# Only the completed manifest indicates success. Preserve ownership, links,
# ACLs and all xattrs (including security.capability), omit transient contents.
tar --create --file=- --directory="$source_root" --numeric-owner \
    --acls --xattrs --xattrs-include='*' --sparse --one-file-system \
    --exclude='./proc/*' --exclude='./sys/*' --exclude='./dev/*' \
    --exclude='./run/*' --exclude='./tmp/*' --exclude='./var/tmp/*' \
    --exclude='./etc/machine-id' --exclude='./var/lib/dbus/machine-id' \
    . | gzip -1 -n > "$output/rootfs.tar.gz"
gzip -t "$output/rootfs.tar.gz"
digest=$(sha256sum "$output/rootfs.tar.gz" | awk '{print $1}')
bytes=$(stat -c '%s' "$output/rootfs.tar.gz")
commit=$(git -C "$ROOT" rev-parse HEAD)
dirty=$(if test -z "$(git -C "$ROOT" status --porcelain)"; then echo 0; else echo 1; fi)
printf '%s\n' \
    'DANOS_ROOTFS_FORMAT=1' \
    'DANOS_ROOTFS_ARCHIVE=rootfs.tar.gz' \
    "DANOS_ROOTFS_SHA256=$digest" \
    "DANOS_ROOTFS_BYTES=$bytes" \
    "DANOS_ROOTFS_PACKAGES_SHA256=$packages_digest" \
    "DANOS_ROOTFS_OS_RELEASE_SHA256=$os_digest" \
    "DANOS_ROOTFS_BUILDER_COMMIT=$commit" \
    "DANOS_ROOTFS_BUILDER_DIRTY=$dirty" \
    'DANOS_ROOTFS_QUALIFIED=0' > "$output/rootfs.manifest"
echo "Rootfs payload built (not installation-qualified): $output"
