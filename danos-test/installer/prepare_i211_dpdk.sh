#!/bin/sh
# Prepare the VPP startup config for an I211 lab runner. Only Intel 8086:1539
# functions are selected; unrelated devices remain owned by their drivers.
set -eu

sysfs_root=${DANOS_PCI_SYSFS_ROOT:-/sys}
runtime_root=${DANOS_DPDK_RUNTIME_ROOT:-/run}
bind_helper=${DANOS_I211_BIND_HELPER:-/usr/libexec/danos/bind_dpdk_pci.sh}
console=${DANOS_CONSOLE:-/dev/console}
ports=

for device_dir in "$sysfs_root"/bus/pci/devices/*; do
    test -r "$device_dir/vendor" && test -r "$device_dir/device" || continue
    test "$(cat "$device_dir/vendor")" = 0x8086 || continue
    test "$(cat "$device_dir/device")" = 0x1539 || continue
    ports="$ports ${device_dir##*/}"
done

mkdir -p "$runtime_root/vpp" "$runtime_root/danos"
startup="$runtime_root/vpp/startup.conf"
{
    cat <<'CONFIG'
unix {
  nodaemon
  log /var/log/vpp/vpp.log
  cli-listen /run/vpp/cli.sock
}
plugins {
  plugin dpdk_plugin.so { enable }
  plugin ping_plugin.so { enable }
}
api-segment { prefix vpp }
socksvr { default }
statseg { socket-name /run/vpp/stats.sock }
CONFIG
    if test -n "$ports"; then
        echo 'dpdk {'
        for bdf in $ports; do
            printf '  dev %s {\n    no-rx-interrupts\n  }\n' "$bdf"
        done
        echo '}'
    fi
} > "$startup"

if test -n "$ports"; then
    if test "${DANOS_SKIP_PCI_BIND:-0}" != 1; then
        modprobe uio_pci_generic
        DANOS_DPDK_SYSFS_ROOT="$sysfs_root" \
        DANOS_BUILD_DPDK_BIND_DRIVER=uio_pci_generic \
        DANOS_BUILD_DPDK_EXPECTED_PCI_ID=8086:1539 \
        DANOS_BUILD_DPDK_PORTS="$ports" \
            "$bind_helper"
    fi
    printf '%s\n' "$ports" > "$runtime_root/danos/i211-bdfs"
    echo "DANOS-I211-DPDK ports=$ports" > "$console" 2>/dev/null || true
else
    rm -f "$runtime_root/danos/i211-bdfs"
    echo 'DANOS-I211-DPDK no-I211-devices; starting VPP without DPDK ports' \
        > "$console" 2>/dev/null || true
fi
