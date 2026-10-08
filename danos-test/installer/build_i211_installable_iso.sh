#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT_DIR=${1:-$ROOT/build/dpdk-installable}
OUT_ISO=${2:-}
mkdir -p "$OUT_DIR"
OUT_DIR=$(cd "$OUT_DIR" && pwd)
RUN_LOG="$OUT_DIR/installable-build-$(date -u +%Y%m%dT%H%M%SZ).log"

PAYLOAD=${DANOS_INSTALLER_DISK_IMAGE:-}
MANIFEST=${DANOS_INSTALLER_DISK_MANIFEST:-}
if test -z "$PAYLOAD" || test -z "$MANIFEST"; then
    bash "$ROOT/danos-test/installer/build_i211_disk_image.sh" "$OUT_DIR" 2>&1 | tee "$RUN_LOG"
    PAYLOAD=$(sed -n 's/^INSTALLER_PAYLOAD=//p' "$RUN_LOG" | tail -n 1)
    MANIFEST=$(sed -n 's/^INSTALL_MANIFEST=//p' "$RUN_LOG" | tail -n 1)
else
    printf 'Reusing validated disk payload: %s\nManifest: %s\n' "$PAYLOAD" "$MANIFEST" | tee "$RUN_LOG"
fi
test -r "$PAYLOAD" && test -r "$MANIFEST" || {
    echo "ERROR: disk-image builder did not emit a complete payload; log: $RUN_LOG" >&2
    exit 1
}
RUN_ID=${PAYLOAD##*-installed-}
RUN_ID=${RUN_ID%.raw.gz}
if test -z "$OUT_ISO"; then
    OUT_ISO="$ROOT/build/danos-open-v0.16.0-rc1-dpdk-installable-$RUN_ID.iso"
fi

VPP_DPDK_ENABLE=0 VPP_AUTOSTART=0 \
DANOS_INSTALLER_ENABLE=1 \
DANOS_INSTALLER_DISK_IMAGE="$PAYLOAD" \
DANOS_INSTALLER_DISK_MANIFEST="$MANIFEST" \
    bash "$ROOT/danos-test/live/build_live_iso.sh" "$OUT_ISO" \
    2>&1 | tee -a "$RUN_LOG"

ISO_SHA=$(sha256sum "$OUT_ISO" | awk '{print $1}')
ISO_BYTES=$(stat -c '%s' "$OUT_ISO")
cat > "${OUT_ISO}.manifest" <<EOF
DANOS_INSTALLER_ISO=$OUT_ISO
DANOS_INSTALLER_ISO_BYTES=$ISO_BYTES
DANOS_INSTALLER_ISO_SHA256=$ISO_SHA
DANOS_INSTALLER_PAYLOAD=$PAYLOAD
DANOS_INSTALLER_DISK_MANIFEST=$MANIFEST
DANOS_INSTALLER_BOOT_MODE=legacy-bios
DANOS_INSTALLER_TARGET_MIN_BYTES=$(sed -n 's/^DANOS_INSTALLED_DISK_BYTES=//p' "$MANIFEST")
DANOS_SOURCE_COMMIT=$(git -C "$ROOT" rev-parse HEAD)
DANOS_SOURCE_DIRTY=$(if git -C "$ROOT" diff --quiet && git -C "$ROOT" diff --cached --quiet; then echo 0; else echo 1; fi)
EOF
printf 'INSTALLABLE_ISO=%s\n' "$OUT_ISO"
printf 'INSTALLABLE_ISO_SHA256=%s\n' "$ISO_SHA"
printf 'INSTALLABLE_ISO_MANIFEST=%s.manifest\n' "$OUT_ISO"
