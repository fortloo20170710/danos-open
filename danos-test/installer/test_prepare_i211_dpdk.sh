#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SCRIPT="$ROOT/danos-test/installer/prepare_i211_dpdk.sh"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/danos-i211-prepare-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

make_nic() {
    local bdf="$1" vendor="$2" device="$3"
    mkdir -p "$WORK/sys/bus/pci/devices/$bdf"
    printf '%s\n' "$vendor" > "$WORK/sys/bus/pci/devices/$bdf/vendor"
    printf '%s\n' "$device" > "$WORK/sys/bus/pci/devices/$bdf/device"
}

make_nic 0000:01:00.0 0x8086 0x1539
make_nic 0000:02:00.0 0x8086 0x1539
make_nic 0000:03:00.0 0x8086 0x100e

DANOS_PCI_SYSFS_ROOT="$WORK/sys" \
DANOS_DPDK_RUNTIME_ROOT="$WORK/run" \
DANOS_CONSOLE="$WORK/console" \
DANOS_SKIP_PCI_BIND=1 \
DANOS_I211_BIND_HELPER="$WORK/no-bind-helper" \
    sh "$SCRIPT"

grep -Fq 'dev 0000:01:00.0 {' "$WORK/run/vpp/startup.conf"
grep -Fq 'dev 0000:02:00.0 {' "$WORK/run/vpp/startup.conf"
! grep -Fq 'dev 0000:03:00.0 {' "$WORK/run/vpp/startup.conf"
grep -Fq 'no-rx-interrupts' "$WORK/run/vpp/startup.conf"
grep -Fq '0000:01:00.0 0000:02:00.0' "$WORK/run/danos/i211-bdfs"

rm -rf "$WORK/sys/bus/pci/devices/0000:01:00.0" \
    "$WORK/sys/bus/pci/devices/0000:02:00.0"
DANOS_PCI_SYSFS_ROOT="$WORK/sys" \
DANOS_DPDK_RUNTIME_ROOT="$WORK/run-empty" \
DANOS_CONSOLE="$WORK/console" \
    sh "$SCRIPT"
! grep -q '^dpdk {$' "$WORK/run-empty/vpp/startup.conf"
test ! -e "$WORK/run-empty/danos/i211-bdfs"
grep -Fq 'no-I211-devices' "$WORK/console"

echo 'I211 DPDK startup preparation PASS'
