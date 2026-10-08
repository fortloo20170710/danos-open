#!/bin/sh
# Discover DPDK candidate Ethernet PCI functions at boot, preserve interfaces
# carrying default routes, bind the remaining functions, and generate VPP config.
set -eu

sysfs_root=${DANOS_PCI_SYSFS_ROOT:-/sys}
runtime_root=${DANOS_DPDK_RUNTIME_ROOT:-/run}
bind_helper=${DANOS_DPDK_BIND_HELPER:-${DANOS_I211_BIND_HELPER:-/usr/libexec/danos/bind_dpdk_pci.sh}}
console=${DANOS_CONSOLE:-/dev/console}
ports=
excluded=
ip_cmd=${DANOS_IP_COMMAND:-ip}
seen_ifaces=

pci_bdf_from_netdev() {
    path=$(readlink -f "$1" 2>/dev/null || true)
    while test -n "$path" && test "$path" != /; do
        bdf=${path##*/}
        case "$bdf" in
            [[:xdigit:]][[:xdigit:]][[:xdigit:]][[:xdigit:]]:[[:xdigit:]][[:xdigit:]]:[[:xdigit:]][[:xdigit:]].[0-7])
                printf '%s\n' "$bdf"
                return 0
                ;;
        esac
        path=${path%/*}
        test -n "$path" || path=/
    done
    return 1
}

exclude_iface() {
    iface=$1
    case " $seen_ifaces " in *" $iface "*) return ;; esac
    seen_ifaces="$seen_ifaces $iface"
    link="$sysfs_root/class/net/$iface/device"
    if test -e "$link"; then
        bdf=$(pci_bdf_from_netdev "$link" || true)
        test -z "$bdf" || excluded="$excluded $bdf"
    fi
    # Resolve bridge/bond/VLAN/upper devices to their lower PCI-backed ports.
    for lower in "$sysfs_root"/class/net/"$iface"/lower_*; do
        test -e "$lower" || continue
        exclude_iface "${lower##*/lower_}"
    done
}

# Never take over the current default-route interface: it may be the only
# management path. All other PCI Ethernet functions are treated as dataplane
# candidates; VPP/DPDK will match only PMDs supported by the installed runtime.
if command -v "$ip_cmd" >/dev/null 2>&1; then
    for iface in $(
        { "$ip_cmd" -o route show default 2>/dev/null; "$ip_cmd" -6 -o route show default 2>/dev/null; } |
            awk '{for (i=1;i<=NF;i++) if ($i=="dev") print $(i+1)}' | sort -u
    ); do
        exclude_iface "$iface"
    done
fi

for device_dir in "$sysfs_root"/bus/pci/devices/*; do
    test -r "$device_dir/class" || continue
    case "$(cat "$device_dir/class")" in 0x020000|0x020000*) ;; *) continue ;; esac
    bdf=${device_dir##*/}
    case " $excluded " in *" $bdf "*)
        echo "DANOS-DPDK-AUTO-SKIP bdf=$bdf reason=default-route-management"
        continue
    esac
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
        driver=uio_pci_generic
        # Prefer VFIO only when IOMMU is active and each candidate's complete
        # group is among the Ethernet candidates. Otherwise use UIO safely.
        if modprobe vfio-pci 2>/dev/null && test -e /dev/vfio/vfio; then
            vfio_safe=1
            for bdf in $ports; do
                group_link="$sysfs_root/bus/pci/devices/$bdf/iommu_group"
                test -L "$group_link" || { vfio_safe=0; break; }
                group=$(basename "$(readlink -f "$group_link")")
                for member in "$sysfs_root"/kernel/iommu_groups/"$group"/devices/*; do
                    test -e "$member" || continue
                    member_bdf=${member##*/}
                    case " $ports " in *" $member_bdf "*) ;; *) vfio_safe=0; break ;; esac
                done
                test "$vfio_safe" = 1 || break
            done
            test "$vfio_safe" = 1 && driver=vfio-pci
        fi
        test "$driver" = vfio-pci || modprobe uio_pci_generic
        DANOS_DPDK_SYSFS_ROOT="$sysfs_root" \
        DANOS_BUILD_DPDK_BIND_DRIVER="$driver" \
        DANOS_BUILD_DPDK_EXPECTED_PCI_ID=any \
        DANOS_BUILD_DPDK_PORTS="$ports" \
            "$bind_helper"
    fi
    printf '%s\n' "$ports" > "$runtime_root/danos/dpdk-bdfs"
    echo "DANOS-DPDK-AUTO ports=$ports driver=${driver:-skipped}" > "$console" 2>/dev/null || true
else
    rm -f "$runtime_root/danos/dpdk-bdfs"
    echo 'DANOS-DPDK-AUTO no-eligible-pci-ethernet; starting VPP without DPDK ports' \
        > "$console" 2>/dev/null || true
fi
