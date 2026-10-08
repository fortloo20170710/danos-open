#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SCRIPT="$ROOT/danos-test/installer/prepare_i211_dpdk.sh"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/danos-i211-prepare-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

make_nic() {
    local bdf="$1" vendor="$2" device="$3" class="$4"
    mkdir -p "$WORK/sys/bus/pci/devices/$bdf"
    printf '%s\n' "$vendor" > "$WORK/sys/bus/pci/devices/$bdf/vendor"
    printf '%s\n' "$device" > "$WORK/sys/bus/pci/devices/$bdf/device"
    printf '%s\n' "$class" > "$WORK/sys/bus/pci/devices/$bdf/class"
}

make_nic 0000:01:00.0 0x8086 0x1539 0x020000
make_nic 0000:02:00.0 0x8086 0x100e 0x020000
make_nic 0000:03:00.0 0x8086 0x100e 0x060000

DANOS_PCI_SYSFS_ROOT="$WORK/sys" \
DANOS_DPDK_RUNTIME_ROOT="$WORK/run" \
DANOS_CONSOLE="$WORK/console" \
DANOS_SKIP_PCI_BIND=1 \
DANOS_DPDK_BIND_HELPER="$WORK/no-bind-helper" \
    sh "$SCRIPT"

grep -Fq 'dev 0000:01:00.0 {' "$WORK/run/vpp/startup.conf"
grep -Fq 'dev 0000:02:00.0 {' "$WORK/run/vpp/startup.conf"
! grep -Fq 'dev 0000:03:00.0 {' "$WORK/run/vpp/startup.conf"
grep -Fq 'no-rx-interrupts' "$WORK/run/vpp/startup.conf"
grep -Fq '0000:01:00.0 0000:02:00.0' "$WORK/run/danos/dpdk-bdfs"

rm -rf "$WORK/sys/bus/pci/devices/0000:01:00.0" \
    "$WORK/sys/bus/pci/devices/0000:02:00.0"
DANOS_PCI_SYSFS_ROOT="$WORK/sys" \
DANOS_DPDK_RUNTIME_ROOT="$WORK/run-empty" \
DANOS_CONSOLE="$WORK/console" \
    sh "$SCRIPT"
! grep -q '^dpdk {$' "$WORK/run-empty/vpp/startup.conf"
test ! -e "$WORK/run-empty/danos/dpdk-bdfs"
grep -Fq 'no-eligible-pci-ethernet' "$WORK/console"

# Preserve the NIC carrying a default route while discovering other Ethernet
# controllers without vendor/device/BDF configuration.
make_nic 0000:01:00.0 0x8086 0x1539 0x020000
make_nic 0000:02:00.0 0x8086 0x100e 0x020000
mkdir -p "$WORK/bin" "$WORK/sys/class/net/mgmt0" "$WORK/sys/class/net/eth1" \
    "$WORK/sys/devices/pci0000:00/0000:02:00.0/virtio0"
cat > "$WORK/bin/ip" <<'IP'
#!/bin/sh
case "$*" in
    '-6 -o route show default') printf '%s\n' 'default via 2001:db8::1 dev mgmt0' ;;
    *) printf '%s\n' 'default via 192.0.2.1 dev mgmt0' ;;
esac
IP
chmod +x "$WORK/bin/ip"
ln -s "../../../devices/pci0000:00/0000:02:00.0/virtio0" "$WORK/sys/class/net/eth1/device"
ln -s ../eth1 "$WORK/sys/class/net/mgmt0/lower_eth1"
DANOS_PCI_SYSFS_ROOT="$WORK/sys" \
DANOS_DPDK_RUNTIME_ROOT="$WORK/run-mgmt" \
DANOS_CONSOLE="$WORK/console" \
DANOS_SKIP_PCI_BIND=1 \
DANOS_IP_COMMAND="$WORK/bin/ip" \
    sh "$SCRIPT" > "$WORK/route.out"
grep -Fq 'dev 0000:01:00.0 {' "$WORK/run-mgmt/vpp/startup.conf"
! grep -Fq 'dev 0000:02:00.0 {' "$WORK/run-mgmt/vpp/startup.conf"
grep -Fq 'reason=default-route-management' "$WORK/route.out"

# Driver choice is runtime-derived and the binder receives wildcard identity
# matching plus only the discovered, non-management BDFs.
mkdir -p "$WORK/mockbin"
cat > "$WORK/mockbin/modprobe" <<'MODPROBE'
#!/bin/sh
test "$1" = uio_pci_generic
MODPROBE
cat > "$WORK/mock-bind" <<'BIND'
#!/bin/sh
printf 'driver=%s expected=%s ports=%s\n' \
    "$DANOS_BUILD_DPDK_BIND_DRIVER" \
    "$DANOS_BUILD_DPDK_EXPECTED_PCI_ID" \
    "$DANOS_BUILD_DPDK_PORTS" > "$DANOS_TEST_BIND_CAPTURE"
BIND
chmod +x "$WORK/mockbin/modprobe" "$WORK/mock-bind"
DANOS_PCI_SYSFS_ROOT="$WORK/sys" \
DANOS_DPDK_RUNTIME_ROOT="$WORK/run-bind" \
DANOS_CONSOLE="$WORK/console" \
DANOS_IP_COMMAND="$WORK/bin/ip" \
DANOS_DPDK_BIND_HELPER="$WORK/mock-bind" \
DANOS_TEST_BIND_CAPTURE="$WORK/bind.env" \
PATH="$WORK/mockbin:$PATH" \
    sh "$SCRIPT" > "$WORK/bind.out"
grep -Fq 'driver=uio_pci_generic expected=any ports= 0000:01:00.0' "$WORK/bind.env"
grep -Fq 'DANOS-DPDK-AUTO-SKIP bdf=0000:02:00.0' "$WORK/bind.out"

echo 'Generic DPDK PCI startup autodiscovery PASS'
