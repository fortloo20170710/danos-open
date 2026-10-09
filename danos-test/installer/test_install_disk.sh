#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SCRIPT="$ROOT/danos-test/installer/install_disk.sh"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/danos-install-disk-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/media/installer" "$WORK/sys/block/testdisk/device" "$WORK/dev"
dd if=/dev/zero of="$WORK/image.raw" bs=1M count=4 status=none
dd if=/dev/zero bs=512 count=1 status=none >> "$WORK/image.raw"
printf 'DANOS-TEST-IMAGE\n' | dd of="$WORK/image.raw" conv=notrunc status=none
gzip -1 -n -c "$WORK/image.raw" > "$WORK/media/installer/danos-runner-installed.raw.gz"
raw_bytes=$(stat -c '%s' "$WORK/image.raw")
raw_sha=$(sha256sum "$WORK/image.raw" | awk '{print $1}')
gzip_sha=$(sha256sum "$WORK/media/installer/danos-runner-installed.raw.gz" | awk '{print $1}')
cat > "$WORK/media/installer/disk-image.env" <<EOF
DANOS_INSTALLED_DISK_BYTES=$raw_bytes
DANOS_INSTALLED_DISK_SHA256=$raw_sha
DANOS_INSTALLED_DISK_GZIP_SHA256=$gzip_sha
EOF

target="$WORK/dev/testdisk"
dd if=/dev/zero of="$target" bs=1M count=8 status=none
printf '%s\n' 16384 > "$WORK/sys/block/testdisk/size"
printf '%s\n' 'DANOS mock target' > "$WORK/sys/block/testdisk/device/model"
: > "$WORK/mounts"
printf '%s\n' "$target" "ERASE $target" | \
    DANOS_INSTALL_TEST_MODE=1 \
    DANOS_INSTALL_TEST_TARGET_ROOT="$WORK/dev" \
    DANOS_INSTALLER_MEDIA_MOUNT="$WORK/media" \
    DANOS_INSTALLER_SYSFS_ROOT="$WORK/sys" \
    DANOS_INSTALLER_MOUNTS_FILE="$WORK/mounts" \
    DANOS_INSTALLER_CONSOLE="$WORK/console" \
    sh "$SCRIPT"
actual_sha=$(dd if="$target" bs=512 count=$((raw_bytes / 512)) status=none | sha256sum | awk '{print $1}')
test "$actual_sha" = "$raw_sha"
grep -Fq 'DANOS INSTALLER PASS' "$WORK/console"
grep -Fq 'write finished; verifying target readback digest' "$WORK/console"

before_sha=$(sha256sum "$target" | awk '{print $1}')
if printf '%s\n%s\n' "$target" 'ERASE wrong-target' | \
    DANOS_INSTALL_TEST_MODE=1 \
    DANOS_INSTALL_TEST_TARGET_ROOT="$WORK/dev" \
    DANOS_INSTALLER_MEDIA_MOUNT="$WORK/media" \
    DANOS_INSTALLER_SYSFS_ROOT="$WORK/sys" \
    DANOS_INSTALLER_MOUNTS_FILE="$WORK/mounts" \
    DANOS_INSTALLER_CONSOLE="$WORK/console-abort" \
    sh "$SCRIPT"; then
    echo 'wrong confirmation unexpectedly succeeded' >&2
    exit 1
fi
test "$(sha256sum "$target" | awk '{print $1}')" = "$before_sha"
grep -Fq 'target untouched' "$WORK/console-abort"

echo 'Install-to-disk safety and readback tests PASS'
