#!/usr/bin/env bash
# Build a generic DPDK installable ISO by replacing the disk payload in a
# previously QEMU-qualified BIOS installer ISO. The legacy payload pathname is
# retained inside the boot initramfs for compatibility; installed OS is generic.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BASE_ISO=${DANOS_INSTALLER_BASE_ISO:-}
PAYLOAD=${DANOS_INSTALLER_DISK_IMAGE:-}
DISK_MANIFEST=${DANOS_INSTALLER_DISK_MANIFEST:-}

if test -z "$BASE_ISO"; then
    BASE_ISO=$(find "$ROOT/build" -maxdepth 1 -type f \
        -name 'danos-open-v0.16.0-rc1-i211-installable-*.iso' -print | sort | tail -n 1)
fi
if test -z "$PAYLOAD"; then
    PAYLOAD=$(find "$ROOT/build/dpdk-installable" -maxdepth 1 -type f \
        -name 'danos-open-v0.16.0-rc1-dpdk-installed-*.raw.gz' -print | sort | tail -n 1)
fi
if test -z "$DISK_MANIFEST" && test -n "$PAYLOAD"; then
    DISK_MANIFEST=${PAYLOAD%.raw.gz}.manifest
fi
for input in "$BASE_ISO" "$PAYLOAD" "$DISK_MANIFEST"; do
    test -r "$input" || { echo "ERROR: required installer input missing: $input" >&2; exit 2; }
done
for tool in xorriso cpio gzip install; do
    command -v "$tool" >/dev/null || { echo "ERROR: $tool is required" >&2; exit 2; }
done

RUN_ID=${PAYLOAD##*-installed-}
RUN_ID=${RUN_ID%.raw.gz}
OUT_ISO=${1:-$ROOT/build/danos-open-v0.16.0-rc1-generic-dpdk-installable-$RUN_ID.iso}
case "$OUT_ISO" in /*) ;; *) OUT_ISO="$ROOT/$OUT_ISO" ;; esac
test "$OUT_ISO" != "$BASE_ISO" || { echo 'ERROR: output must not overwrite base ISO' >&2; exit 2; }
if test -e "$OUT_ISO"; then
    output_base=${OUT_ISO%.iso}
    revision=2
    while test -e "$output_base-r$revision.iso"; do revision=$((revision + 1)); done
    OUT_ISO="$output_base-r$revision.iso"
fi
mkdir -p "$(dirname "$OUT_ISO")"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/danos-dpdk-iso-verify.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
PAYLOAD_SHA=$(sha256sum "$PAYLOAD" | awk '{print $1}')
BASE_SHA=$(sha256sum "$BASE_ISO" | awk '{print $1}')

# Refresh the installer in the base initramfs so prompts and payload naming
# match the generic profile while retaining its qualified kernel/modules.
mkdir -p "$TMP/initramfs"
xorriso -osirrox on -indev "$BASE_ISO" \
    -extract /initramfs.cpio.gz "$TMP/base-initramfs.cpio.gz" -end >/dev/null 2>&1
gzip -dc "$TMP/base-initramfs.cpio.gz" | \
    (cd "$TMP/initramfs" && cpio -idmu --quiet)
install -m 0755 "$ROOT/danos-test/installer/install_disk.sh" \
    "$TMP/initramfs/bin/danos-install"
BUILD_DIRTY=$(if git -C "$ROOT" diff --quiet && git -C "$ROOT" diff --cached --quiet; then echo 0; else echo 1; fi)
cat > "$TMP/initramfs/etc/danos/build-info.env" <<EOF
DANOS_BUILD_COMMIT=$(git -C "$ROOT" rev-parse HEAD)
DANOS_BUILD_SOURCE_DIRTY=$BUILD_DIRTY
DANOS_BUILD_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)
DANOS_BUILD_ISO=$(basename "$OUT_ISO")
DANOS_BUILD_DPDK_NO_RX_INTERRUPTS=0
EOF
(cd "$TMP/initramfs" && find . | cpio -o -H newc --quiet | gzip -6) \
    > "$TMP/initramfs.cpio.gz"
INITRAMFS_SHA=$(sha256sum "$TMP/initramfs.cpio.gz" | awk '{print $1}')

# The qualified base ISO's initramfs expects this historical path. Only the
# payload path is compatibility-frozen; the disk contents and profile are generic.
xorriso -indev "$BASE_ISO" -outdev "$OUT_ISO" -boot_image any replay \
    -map "$TMP/initramfs.cpio.gz" /initramfs.cpio.gz \
    -map "$PAYLOAD" /installer/danos-i211-installed.raw.gz \
    -map "$DISK_MANIFEST" /installer/disk-image.env \
    -map "$ROOT/danos-test/installer/dpdk-installer-boot.msg" /isolinux/boot.msg \
    -commit -end

ISO_SHA=$(sha256sum "$OUT_ISO" | awk '{print $1}')
ISO_BYTES=$(stat -c '%s' "$OUT_ISO")
cat > "${OUT_ISO}.manifest" <<EOF
DANOS_INSTALLER_ISO=$OUT_ISO
DANOS_INSTALLER_ISO_BYTES=$ISO_BYTES
DANOS_INSTALLER_ISO_SHA256=$ISO_SHA
DANOS_INSTALLER_BASE_ISO=$BASE_ISO
DANOS_INSTALLER_BASE_ISO_SHA256=$BASE_SHA
DANOS_INSTALLER_PAYLOAD=$PAYLOAD
DANOS_INSTALLER_PAYLOAD_SHA256=$PAYLOAD_SHA
DANOS_INSTALLER_DISK_MANIFEST=$DISK_MANIFEST
DANOS_INSTALLER_BOOT_MODE=legacy-bios
DANOS_INSTALLER_INITRAMFS_SHA256=$INITRAMFS_SHA
DANOS_SOURCE_COMMIT=$(git -C "$ROOT" rev-parse HEAD)
DANOS_SOURCE_DIRTY=$(if git -C "$ROOT" diff --quiet && git -C "$ROOT" diff --cached --quiet; then echo 0; else echo 1; fi)
EOF

xorriso -indev "$OUT_ISO" -ls /installer -end >"$TMP/iso-tree.log" 2>&1
xorriso -osirrox on -indev "$OUT_ISO" \
    -extract /installer/danos-i211-installed.raw.gz "$TMP/embedded.raw.gz" \
    -extract /installer/disk-image.env "$TMP/embedded.disk.env" \
    -extract /isolinux/boot.msg "$TMP/embedded.boot.msg" \
    -extract /initramfs.cpio.gz "$TMP/embedded.initramfs.cpio.gz" -end >/dev/null 2>&1
test "$(sha256sum "$TMP/embedded.raw.gz" | awk '{print $1}')" = "$PAYLOAD_SHA"
cmp -s "$TMP/embedded.disk.env" "$DISK_MANIFEST"
cmp -s "$TMP/embedded.boot.msg" "$ROOT/danos-test/installer/dpdk-installer-boot.msg"
test "$(sha256sum "$TMP/embedded.initramfs.cpio.gz" | awk '{print $1}')" = "$INITRAMFS_SHA"
mkdir -p "$TMP/initramfs-check"
gzip -dc "$TMP/embedded.initramfs.cpio.gz" | \
    (cd "$TMP/initramfs-check" && cpio -idmu --quiet)
cmp -s "$TMP/initramfs-check/bin/danos-install" "$ROOT/danos-test/installer/install_disk.sh"
grep -Fq "DANOS_BUILD_ISO=$(basename "$OUT_ISO")" "$TMP/initramfs-check/etc/danos/build-info.env"
xorriso -indev "$OUT_ISO" -report_el_torito plain -end >"$TMP/boot-report.log" 2>&1
grep -q 'El Torito' "$TMP/boot-report.log"

printf 'INSTALLABLE_ISO=%s\nINSTALLABLE_ISO_SHA256=%s\nINSTALLABLE_ISO_MANIFEST=%s.manifest\n' \
    "$OUT_ISO" "$ISO_SHA" "$OUT_ISO"
