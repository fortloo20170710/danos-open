#!/bin/bash
# DANOS-Open live ISO builder (v0.15)
#
# Produces a bootable ISO:
#   Debian kernel (trixie) + initramfs (busybox + mgrd + gnmic) +
#   isolinux bootloader. On boot: mgrd starts (real netlink, VM root),
#   and a built-in demo provisions a route via gNMI and prints the
#   kernel FIB on the serial console.
#
# Requirements: docker (debian:trixie-slim), host xorriso, gnmic binary
# Usage: bash danos-test/live/build_live_iso.sh [output.iso]

set -eu
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_COMMIT="$(git -C "$PROJECT_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
if git -C "$PROJECT_ROOT" diff --quiet && git -C "$PROJECT_ROOT" diff --cached --quiet; then
  BUILD_SOURCE_DIRTY=0
else
  BUILD_SOURCE_DIRTY=1
fi
BUILD_UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
OUT_ISO="${1:-$PROJECT_ROOT/build/danos-open-live.iso}"
case "$OUT_ISO" in
  /*) ;;
  *) OUT_ISO="$PROJECT_ROOT/$OUT_ISO" ;;
esac
GNMIC_SRC="${GNMIC:-/tmp/gnmic-bin}"
WORK="${DANOS_ISO_WORK_DIR:-/tmp/danos-iso-work-$$}"
BUILD_CONTAINER="danos-iso-build-$$"
ISO_BUILD_IMAGE="${DANOS_ISO_BUILD_IMAGE:-debian:trixie-slim}"
ISO_DOCKER_NETWORK="${DANOS_ISO_DOCKER_NETWORK:-bridge}"
ISO_SKIP_APT="${DANOS_ISO_SKIP_APT:-0}"
APT_MIRROR="${APT_MIRROR:-http://repo.huaweicloud.com/debian}"
VPP_IMAGE="${VPP_IMAGE:-}"
VPP_DPDK_ENABLE="${VPP_DPDK_ENABLE:-1}"
VPP_DPDK_DEVICE="${VPP_DPDK_DEVICE:-vmxnet3}"
VPP_DPDK_PORTS="${VPP_DPDK_PORTS:-0000:00:02.0}"
VPP_VMXNET3_NATIVE="${VPP_VMXNET3_NATIVE:-0}"
VPP_DPDK_TRAFFIC_TEST="${VPP_DPDK_TRAFFIC_TEST:-0}"
VPP_DPDK_NO_RX_INTERRUPTS="${VPP_DPDK_NO_RX_INTERRUPTS:-0}"
VPP_DPDK_BIND_DRIVER="${VPP_DPDK_BIND_DRIVER:-none}"
VPP_DPDK_EXPECTED_PCI_ID="${VPP_DPDK_EXPECTED_PCI_ID:-8086:1539}"
VPP_AUTOSTART="${VPP_AUTOSTART:-1}"
VPP_PING_ENABLE="${VPP_PING_ENABLE:-0}"
VPP_STATIC_NEIGHBORS="${VPP_STATIC_NEIGHBORS:-1}"
VPP_TRAFFIC_TEST_DELAY="${VPP_TRAFFIC_TEST_DELAY:-0}"
VPP_ECMP_SOAK_COUNT="${VPP_ECMP_SOAK_COUNT:-0}"
VPP_ECMP_SOAK_INTERVAL="${VPP_ECMP_SOAK_INTERVAL:-0.01}"
VPP_TRAFFIC_READY_ENDPOINT="${VPP_TRAFFIC_READY_ENDPOINT:-}"
VPP_TRAFFIC_READY_TIMEOUT="${VPP_TRAFFIC_READY_TIMEOUT:-15}"
VPP_PEER_MAC1="${VPP_PEER_MAC1:-52:54:00:11:01:02}"
VPP_PEER_MAC2="${VPP_PEER_MAC2:-52:54:00:12:01:02}"
VPP_PEER_IP1="${VPP_PEER_IP1:-10.10.0.2}"
VPP_PEER_IP2="${VPP_PEER_IP2:-10.20.0.2}"
VPP_IF1_ADDR="${VPP_IF1_ADDR:-10.10.0.1/24}"
VPP_IF2_ADDR="${VPP_IF2_ADDR:-10.20.0.1/24}"
VPP_IF2_EXTRA_ADDRS="${VPP_IF2_EXTRA_ADDRS:-}"
DANOS_MGMT_ADDR="${DANOS_MGMT_ADDR:-10.0.2.15/24}"
DANOS_MGMT_GW="${DANOS_MGMT_GW:-10.0.2.2}"
DANOS_PEER_ADDR2="${DANOS_PEER_ADDR2:-}"
DANOS_PEER_EXTRA_ADDRS="${DANOS_PEER_EXTRA_ADDRS:-}"
DANOS_PEER_RETURN_GW="${DANOS_PEER_RETURN_GW:-}"
DANOS_TRAFFIC_TARGETS="${DANOS_TRAFFIC_TARGETS:-}"
DANOS_TRAFFIC_COUNT="${DANOS_TRAFFIC_COUNT:-0}"
DANOS_TRAFFIC_INTERVAL="${DANOS_TRAFFIC_INTERVAL:-0.01}"
DANOS_PEER_NEIGH1_IP="${DANOS_PEER_NEIGH1_IP:-}"
DANOS_PEER_NEIGH1_MAC="${DANOS_PEER_NEIGH1_MAC:-}"
DANOS_PEER_NEIGH2_IP="${DANOS_PEER_NEIGH2_IP:-}"
DANOS_PEER_NEIGH2_MAC="${DANOS_PEER_NEIGH2_MAC:-}"
DANOS_FIB_BRIDGE_ENABLE="${DANOS_FIB_BRIDGE_ENABLE:-0}"
DANOS_ZEBRA_ENDPOINT="${DANOS_ZEBRA_ENDPOINT:-}"
DANOS_ZAPI_HELLO_ONLY="${DANOS_ZAPI_HELLO_ONLY:-0}"
DANOS_INSTALLER_ENABLE="${DANOS_INSTALLER_ENABLE:-0}"
DANOS_INSTALLER_DISK_IMAGE="${DANOS_INSTALLER_DISK_IMAGE:-}"
DANOS_INSTALLER_DISK_MANIFEST="${DANOS_INSTALLER_DISK_MANIFEST:-}"

if test "$DANOS_INSTALLER_ENABLE" = 1; then
  test -r "$DANOS_INSTALLER_DISK_IMAGE" || {
    echo 'ERROR: installer mode requires DANOS_INSTALLER_DISK_IMAGE' >&2
    exit 2
  }
  test -r "$DANOS_INSTALLER_DISK_MANIFEST" || {
    echo 'ERROR: installer mode requires DANOS_INSTALLER_DISK_MANIFEST' >&2
    exit 2
  }
fi

# The live traffic/ECMP acceptance path invokes VPP's CLI `ping` command.
# That command is provided by ping_plugin.so, not by the base VPP runtime or
# dpdk_plugin. Keep the image self-consistent whenever those tests are enabled.
if test "$VPP_DPDK_TRAFFIC_TEST" = 1 || {
  [[ "$VPP_ECMP_SOAK_COUNT" =~ ^[0-9]+$ ]] && test "$VPP_ECMP_SOAK_COUNT" -gt 0;
}; then
  VPP_PING_ENABLE=1
fi
DANOS_ZAPI_REG_STAGE="${DANOS_ZAPI_REG_STAGE:-0}"
DANOS_ZAPI_PACE_US="${DANOS_ZAPI_PACE_US:-0}"
DANOS_SKIP_VPP_RECOVERY="${DANOS_SKIP_VPP_RECOVERY:-0}"
DANOS_VPP_HEALTH_PROBE="${DANOS_VPP_HEALTH_PROBE:-0}"
DANOS_VPP_RESTART_TEST="${DANOS_VPP_RESTART_TEST:-${DANOS_VPP_HEALTH_PROBE:-0}}"

case "$VPP_DPDK_BIND_DRIVER" in
  none|uio_pci_generic) ;;
  *) echo "ERROR: unsupported VPP_DPDK_BIND_DRIVER=$VPP_DPDK_BIND_DRIVER" >&2; exit 2 ;;
esac
case "$ISO_SKIP_APT" in
  0|1) ;;
  *) echo "ERROR: DANOS_ISO_SKIP_APT must be 0 or 1" >&2; exit 2 ;;
esac
if test "$VPP_DPDK_BIND_DRIVER" != none; then
  test "$VPP_DPDK_ENABLE" = 1 || { echo 'ERROR: PCI binding requires VPP_DPDK_ENABLE=1' >&2; exit 2; }
  test -n "$VPP_DPDK_PORTS" || { echo 'ERROR: PCI binding requires explicit VPP_DPDK_PORTS' >&2; exit 2; }
fi

mkdir -p "$WORK" "$PROJECT_ROOT/build"

# --- 1. trixie build container: kernel + busybox + isolinux + mgrd ------
docker rm -f "$BUILD_CONTAINER" >/dev/null 2>&1 || true
trap 'docker rm -f "$BUILD_CONTAINER" >/dev/null 2>&1 || true' EXIT
docker run -d --name "$BUILD_CONTAINER" --network "$ISO_DOCKER_NETWORK" \
    -v "$PROJECT_ROOT:/src" \
    -w /src "$ISO_BUILD_IMAGE" sh -c "
set -eu
if test \"$ISO_SKIP_APT\" != 1; then
rm -f /etc/apt/sources.list /etc/apt/sources.list.d/debian.sources
cat > /etc/apt/sources.list.d/danos-mirror.sources <<EOF
Types: deb
URIs: $APT_MIRROR
Suites: trixie trixie-updates
Components: main
EOF
apt-get update -qq -o Acquire::http::Timeout=30 -o Acquire::Retries=2
apt-get install -y -qq --no-install-recommends build-essential cmake libssl-dev openssl python3 busybox-static zstd kmod \
    linux-image-amd64 isolinux syslinux-common
else
for required_tool in cmake gcc make python3 busybox zstd modprobe; do
    command -v "\$required_tool" >/dev/null || { echo "missing preinstalled ISO build tool: \$required_tool" >&2; exit 1; }
done
fi
test -n \"\$(find /boot -maxdepth 1 -name 'vmlinuz-*' -print -quit)\"
test -x /bin/busybox
cmake -B /tmp/b -S /src -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/b -j\$(nproc) --target danos-mgrd fib_live_bridge
test -x /tmp/b/danos-mgrd/danos-mgrd
test -x /tmp/b/danos-test/fib_live_bridge
echo ISO-DEPS-OK
sleep 600"
# wait for deps to be ready (log marker), max ~6 min
for i in $(seq 1 180); do
    docker logs "$BUILD_CONTAINER" 2>&1 | grep -q ISO-DEPS-OK && break
    if test "$(docker inspect --format '{{.State.Running}}' "$BUILD_CONTAINER")" != true; then
        docker logs "$BUILD_CONTAINER" >&2
        echo "ERROR: ISO dependency/build container exited" >&2
        exit 1
    fi
    sleep 2
done
docker logs "$BUILD_CONTAINER" 2>&1 | grep -q ISO-DEPS-OK \
    || { echo "ERROR: ISO deps build failed"; exit 1; }

# --- 2. extract pieces ---------------------------------------------------
mkdir -p "$WORK"
VKERNEL=$(docker exec "$BUILD_CONTAINER" sh -c 'ls /boot/vmlinuz-* | head -1')
KERNEL_VERSION=${VKERNEL##*/vmlinuz-}
docker cp "$BUILD_CONTAINER:$VKERNEL" "$WORK/vmlinuz"
docker cp "$BUILD_CONTAINER:/bin/busybox" "$WORK/busybox"
docker cp "$BUILD_CONTAINER:/usr/lib/ISOLINUX/isolinux.bin" "$WORK/isolinux.bin"
docker cp "$BUILD_CONTAINER:/usr/lib/ISOLINUX/isohdpfx.bin" "$WORK/isohdpfx.bin"
docker cp "$BUILD_CONTAINER:/usr/lib/syslinux/modules/bios/ldlinux.c32" "$WORK/ldlinux.c32"
docker cp "$BUILD_CONTAINER:/usr/lib/x86_64-linux-gnu/libc.so.6" "$WORK/libc.so.6"
docker cp "$BUILD_CONTAINER:/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" "$WORK/ld-linux.so.2"
MODULE_ROOTS='/lib/modules /usr/lib/modules'
E1000=$(docker exec "$BUILD_CONTAINER" sh -c \
    'find /lib/modules /usr/lib/modules -name "e1000.ko*" 2>/dev/null | head -1')
docker cp "$BUILD_CONTAINER:$E1000" "$WORK/e1000.ko.raw"
VMXNET3=$(docker exec "$BUILD_CONTAINER" sh -c \
    'find /lib/modules /usr/lib/modules -name "vmxnet3.ko*" 2>/dev/null | head -1')
docker cp "$BUILD_CONTAINER:$VMXNET3" "$WORK/vmxnet3.ko.raw"
VIRTIO_NET=$(docker exec "$BUILD_CONTAINER" sh -c \
    'find /lib/modules /usr/lib/modules -name "virtio_net.ko*" 2>/dev/null | head -1')
docker cp "$BUILD_CONTAINER:$VIRTIO_NET" "$WORK/virtio_net.ko.raw"
NET_FAILOVER=$(docker exec "$BUILD_CONTAINER" sh -c \
    'find /lib/modules /usr/lib/modules -name "net_failover.ko*" 2>/dev/null | head -1')
FAILOVER=$(docker exec "$BUILD_CONTAINER" sh -c \
    'find /lib/modules /usr/lib/modules -name "failover.ko*" 2>/dev/null | head -1')
VIRTIO_PCI=$(docker exec "$BUILD_CONTAINER" sh -c \
    'find /lib/modules /usr/lib/modules -name "virtio_pci.ko*" 2>/dev/null | head -1')
UIO=$(docker exec "$BUILD_CONTAINER" sh -c \
    'find /lib/modules /usr/lib/modules -name "uio.ko*" 2>/dev/null | head -1')
UIO_PCI_GENERIC=$(docker exec "$BUILD_CONTAINER" sh -c \
    'find /lib/modules /usr/lib/modules -name "uio_pci_generic.ko*" 2>/dev/null | head -1')
for module in usb_common usbcore input_core hid xhci_hcd xhci_pci \
    ehci_hcd ehci_pci uhci_hcd ohci_hcd ohci_pci usbhid hid_generic \
    failover net_failover virtio_pci uio uio_pci_generic; do
  module_file="${module//_/-}"
  source=$(docker exec "$BUILD_CONTAINER" sh -c \
    "find /lib/modules /usr/lib/modules \\( -name '${module}.ko*' -o -name '${module_file}.ko*' \\) 2>/dev/null | head -1")
  eval "${module^^}=\$source"
  if test -n "$source"; then
    docker cp "$BUILD_CONTAINER:$source" "$WORK/${module}.ko.raw"
  fi
done

if test "$DANOS_INSTALLER_ENABLE" = 1; then
  declare -A installer_module_seen=()
  : > "$WORK/installer-modules.map"
  for root_module in sd_mod usb_storage uas ahci ata_piix isofs sr_mod virtio_blk nvme mmc_block; do
    dependency_lines=$(docker exec "$BUILD_CONTAINER" \
      modprobe --show-depends --set-version "$KERNEL_VERSION" "$root_module") || {
        echo "ERROR: cannot resolve installer storage module $root_module" >&2
        exit 1
      }
    while read -r action module_path; do
      test "$action" = insmod || continue
      module_file=${module_path##*/}
      module_name=${module_file%%.ko*}
      module_name=${module_name//-/_}
      test -n "${installer_module_seen[$module_name]:-}" && continue
      installer_module_seen[$module_name]=1
      docker cp "$BUILD_CONTAINER:$module_path" "$WORK/$module_file.raw"
      printf '%s %s\n' "$module_name" "$module_path" >> "$WORK/installer-modules.map"
    done <<< "$dependency_lines"
  done
fi
if test -x "$GNMIC_SRC"; then
    cp "$GNMIC_SRC" "$WORK/gnmic"
else
    echo "INFO: gnmic not provided; ISO will skip the optional gNMI demo"
fi

# --- 3. initramfs tree ---------------------------------------------------
rm -rf "$WORK/initramfs"
mkdir -p "$WORK/initramfs"/{bin,dev,proc,sys,tmp,lib,lib64,modules}
mkdir -p "$WORK/initramfs/etc/danos"
{
  printf 'DANOS_BUILD_COMMIT=%q\n' "$BUILD_COMMIT"
  printf 'DANOS_BUILD_SOURCE_DIRTY=%q\n' "$BUILD_SOURCE_DIRTY"
  printf 'DANOS_BUILD_UTC=%q\n' "$BUILD_UTC"
  printf 'DANOS_BUILD_ISO=%q\n' "$(basename "$OUT_ISO")"
  printf 'DANOS_BUILD_VPP_IMAGE=%q\n' "$VPP_IMAGE"
  printf 'DANOS_BUILD_DPDK_PORTS=%q\n' "$VPP_DPDK_PORTS"
  printf 'DANOS_BUILD_DPDK_NO_RX_INTERRUPTS=%q\n' "$VPP_DPDK_NO_RX_INTERRUPTS"
  printf 'DANOS_BUILD_DPDK_BIND_DRIVER=%q\n' "$VPP_DPDK_BIND_DRIVER"
  printf 'DANOS_BUILD_DPDK_EXPECTED_PCI_ID=%q\n' "$VPP_DPDK_EXPECTED_PCI_ID"
  printf 'DANOS_BUILD_DPDK_DEVICE=%q\n' "$VPP_DPDK_DEVICE"
  printf 'DANOS_BUILD_DPDK_ENABLE=%q\n' "$VPP_DPDK_ENABLE"
  printf 'DANOS_BUILD_DPDK_AUTOSTART=%q\n' "$VPP_AUTOSTART"
  printf 'DANOS_BUILD_PING_ENABLE=%q\n' "$VPP_PING_ENABLE"
  printf 'DANOS_BUILD_DPDK_TRAFFIC_TEST=%q\n' "$VPP_DPDK_TRAFFIC_TEST"
  printf 'DANOS_BUILD_DPDK_PORT_COUNT=%q\n' "$(printf '%s\n' $VPP_DPDK_PORTS | wc -l | tr -d ' ')"
  printf 'DANOS_BUILD_ECMP_SOAK_COUNT=%q\n' "$VPP_ECMP_SOAK_COUNT"
  printf 'DANOS_BUILD_ECMP_SOAK_INTERVAL=%q\n' "$VPP_ECMP_SOAK_INTERVAL"
  printf 'DANOS_BUILD_TRAFFIC_READY_ENDPOINT=%q\n' "$VPP_TRAFFIC_READY_ENDPOINT"
  printf 'DANOS_BUILD_PEER_MAC1=%q\n' "$VPP_PEER_MAC1"
  printf 'DANOS_BUILD_PEER_MAC2=%q\n' "$VPP_PEER_MAC2"
  printf 'DANOS_BUILD_PEER_IP1=%q\n' "$VPP_PEER_IP1"
  printf 'DANOS_BUILD_PEER_IP2=%q\n' "$VPP_PEER_IP2"
  printf 'DANOS_BUILD_IF1_ADDR=%q\n' "$VPP_IF1_ADDR"
  printf 'DANOS_BUILD_IF2_ADDR=%q\n' "$VPP_IF2_ADDR"
  printf 'DANOS_BUILD_IF2_EXTRA_ADDRS=%q\n' "$VPP_IF2_EXTRA_ADDRS"
  printf 'DANOS_BUILD_STATIC_NEIGHBORS=%q\n' "$VPP_STATIC_NEIGHBORS"
} > "$WORK/initramfs/etc/danos/build-info.env"
if test -n "$VPP_IMAGE"; then
  # Keep the image container alive so readlink can resolve SONAME targets.
  VPP_CID=$(docker create "$VPP_IMAGE" sleep infinity)
  docker start "$VPP_CID" >/dev/null
  trap 'docker rm -f "$BUILD_CONTAINER" "$VPP_CID" >/dev/null 2>&1 || true' EXIT
  mkdir -p "$WORK/initramfs"/{usr/bin,usr/lib/x86_64-linux-gnu/vpp_plugins,lib/x86_64-linux-gnu,etc/vpp,run/vpp,var/log/vpp}
  copy_vpp_plugin() {
    local plugin="$1" candidate
    for candidate in \
      "/usr/lib/x86_64-linux-gnu/vpp_plugins/$plugin" \
      "/lib/x86_64-linux-gnu/vpp_plugins/$plugin"; do
      if docker cp "$VPP_CID:$candidate" \
          "$WORK/initramfs/usr/lib/x86_64-linux-gnu/vpp_plugins/$plugin" 2>/dev/null; then
        return 0
      fi
    done
    echo "ERROR: VPP runtime image lacks $plugin in standard plugin directories" >&2
    return 1
  }
  docker cp "$VPP_CID:/usr/bin/vpp" "$WORK/initramfs/usr/bin/vpp"
  docker cp "$VPP_CID:/usr/bin/vppctl" "$WORK/initramfs/usr/bin/vppctl"
  for etc_file in passwd group nsswitch.conf hosts; do
    docker cp "$VPP_CID:/etc/$etc_file" "$WORK/initramfs/etc/$etc_file" 2>/dev/null || true
  done
  # Import the complete trixie runtime directory so transitive glibc,
  # crypto, compression and loader objects cannot be mixed with the ISO
  # builder image.  The guest profile is explicitly an integration image.
  docker cp "$VPP_CID:/lib/x86_64-linux-gnu/." \
    "$WORK/initramfs/lib/x86_64-linux-gnu/"
  copy_vpp_plugin dpdk_plugin.so
  # Keep glibc and math/loader objects from the same Debian trixie VPP
  # runtime.  docker cp of a SONAME symlink can otherwise copy only the
  # link, leaving a mixed host/build-container ABI in the initramfs.
  copy_vpp_real() {
    local name="$1" path
    path=$(docker exec "$VPP_CID" readlink -f "/lib/x86_64-linux-gnu/$name") || return 0
    test -n "$path" || return 0
    docker cp "$VPP_CID:$path" "$WORK/initramfs/lib/x86_64-linux-gnu/$name"
  }
  copy_vpp_real libc.so.6
  copy_vpp_real libm.so.6
  copy_vpp_real ld-linux-x86-64.so.2
  for lib in libvnet.so.26.10 libvlibmemory.so.26.10 libvlibapi.so.26.10 \
      libsvm.so.26.10 libvlib.so.26.10 libvppinfra.so.26.10 \
      libnuma.so.1 libcrypto.so.3 libz.so.1 libzstd.so.1 libm.so.6; do
    copy_vpp_real "$lib"
  done
  if test "$VPP_DPDK_ENABLE" = 1; then
    VPP_PLUGIN_LINE='  plugin dpdk_plugin.so { enable }'
    if test "$VPP_DPDK_NO_RX_INTERRUPTS" = 1; then
      VPP_DPDK_BLOCK=$(printf 'dpdk {\n')
      for vpp_dpdk_bdf in $VPP_DPDK_PORTS; do
        VPP_DPDK_BLOCK="${VPP_DPDK_BLOCK}  dev ${vpp_dpdk_bdf} {$(printf '\n')"
        VPP_DPDK_BLOCK="${VPP_DPDK_BLOCK}    no-rx-interrupts$(printf '\n  }\n')"
      done
    else
      VPP_DPDK_BLOCK=$(printf 'dpdk {\n')
      for vpp_dpdk_bdf in $VPP_DPDK_PORTS; do
        VPP_DPDK_BLOCK="${VPP_DPDK_BLOCK}  dev ${vpp_dpdk_bdf}$(printf '\n')"
      done
    fi
    VPP_DPDK_BLOCK="${VPP_DPDK_BLOCK}$(printf '\n}')"
  else
    VPP_PLUGIN_LINE='  plugin dpdk_plugin.so { disable }'
    VPP_DPDK_BLOCK=''
  fi
  if test "$VPP_VMXNET3_NATIVE" = 1; then
    copy_vpp_plugin vmxnet3_plugin.so
    if test "$VPP_DPDK_ENABLE" = 1; then
      VPP_PLUGIN_LINE='  plugin dpdk_plugin.so { enable }'
    else
      VPP_PLUGIN_LINE='  plugin dpdk_plugin.so { disable }'
    fi
    VPP_PLUGIN_LINE="$VPP_PLUGIN_LINE
  plugin vmxnet3_plugin.so { enable }"
  fi
  if test "$VPP_PING_ENABLE" = 1; then
    copy_vpp_plugin ping_plugin.so
    VPP_PLUGIN_LINE="${VPP_PLUGIN_LINE}$(printf '\n  plugin ping_plugin.so { enable }')"
  fi
  cat > "$WORK/initramfs/etc/vpp/startup.conf" <<EOF
unix {
  nodaemon
  log /var/log/vpp/vpp.log
  cli-listen /run/vpp/cli.sock
}
plugins {
${VPP_PLUGIN_LINE}
}
api-segment {
  prefix vpp
}
socksvr {
  default
}
statseg {
  socket-name /run/vpp/stats.sock
}
${VPP_DPDK_BLOCK}
EOF
  test "$VPP_AUTOSTART" = 1 && touch "$WORK/initramfs/vpp-dpdk.enabled"
  test "$VPP_VMXNET3_NATIVE" = 1 && touch "$WORK/initramfs/vpp-vmxnet3-native.enabled"
  test "$VPP_VMXNET3_NATIVE" = 1 && printf '%s\n' "$VPP_DPDK_PORTS" > "$WORK/initramfs/vpp-vmxnet3-native-ports"
  test "$VPP_DPDK_DEVICE" = e1000 && touch "$WORK/initramfs/vpp-dpdk-e1000.enabled"
  printf '%s\n' "$VPP_DPDK_DEVICE" > "$WORK/initramfs/vpp-dpdk-device"
  test "$(printf '%s\n' $VPP_DPDK_PORTS | wc -l)" -ge 2 && touch "$WORK/initramfs/vpp-dpdk-2port.enabled"
  test "$(printf '%s\n' $VPP_DPDK_PORTS | wc -l)" -ge 2 && touch "$WORK/initramfs/vpp-dpdk-e1000-2port.enabled"
  test "$VPP_DPDK_TRAFFIC_TEST" = 1 && touch "$WORK/initramfs/vpp-dpdk-traffic-test.enabled"
  mkdir -p "$WORK/initramfs/etc/danos"
printf 'VPP_PEER_MAC1=%q\nVPP_PEER_MAC2=%q\nVPP_PEER_IP1=%q\nVPP_PEER_IP2=%q\nVPP_IF1_ADDR=%q\nVPP_IF2_ADDR=%q\nVPP_STATIC_NEIGHBORS=%q\nVPP_TRAFFIC_TEST_DELAY=%q\nVPP_TRAFFIC_READY_ENDPOINT=%q\nVPP_TRAFFIC_READY_TIMEOUT=%q\nVPP_ECMP_SOAK_COUNT=%q\nVPP_ECMP_SOAK_INTERVAL=%q\n' \
  "$VPP_PEER_MAC1" "$VPP_PEER_MAC2" "$VPP_PEER_IP1" "$VPP_PEER_IP2" \
  "$VPP_IF1_ADDR" "$VPP_IF2_ADDR" "$VPP_STATIC_NEIGHBORS" "$VPP_TRAFFIC_TEST_DELAY" \
  "$VPP_TRAFFIC_READY_ENDPOINT" "$VPP_TRAFFIC_READY_TIMEOUT" \
  "$VPP_ECMP_SOAK_COUNT" "$VPP_ECMP_SOAK_INTERVAL" \
    > "$WORK/initramfs/etc/danos/vpp-traffic.env"
  chmod +x "$WORK/initramfs/usr/bin/vpp" "$WORK/initramfs/usr/bin/vppctl"
  docker rm -f "$VPP_CID" >/dev/null
fi
mkdir -p "$WORK/initramfs/etc/danos"
printf 'DANOS_MGMT_ADDR=%q\nDANOS_MGMT_GW=%q\nDANOS_PEER_ADDR2=%q\nDANOS_TRAFFIC_TARGETS=%q\nDANOS_TRAFFIC_COUNT=%q\nDANOS_TRAFFIC_INTERVAL=%q\nDANOS_PEER_NEIGH1_IP=%q\nDANOS_PEER_NEIGH1_MAC=%q\nDANOS_PEER_NEIGH2_IP=%q\nDANOS_PEER_NEIGH2_MAC=%q\n' \
  "$DANOS_MGMT_ADDR" "$DANOS_MGMT_GW" "$DANOS_PEER_ADDR2" "$DANOS_TRAFFIC_TARGETS" "$DANOS_TRAFFIC_COUNT" "$DANOS_TRAFFIC_INTERVAL" \
  "$DANOS_PEER_NEIGH1_IP" "$DANOS_PEER_NEIGH1_MAC" "$DANOS_PEER_NEIGH2_IP" "$DANOS_PEER_NEIGH2_MAC" \
  > "$WORK/initramfs/etc/danos/network.env"
printf 'VPP_IF2_EXTRA_ADDRS=%q\n' "$VPP_IF2_EXTRA_ADDRS" >> "$WORK/initramfs/etc/danos/network.env"
printf 'DANOS_PEER_EXTRA_ADDRS=%q\nDANOS_PEER_RETURN_GW=%q\n' \
  "$DANOS_PEER_EXTRA_ADDRS" "$DANOS_PEER_RETURN_GW" \
  >> "$WORK/initramfs/etc/danos/network.env"
cp "$WORK/busybox" "$WORK/initramfs/bin/busybox"
cp "$WORK/libc.so.6" "$WORK/initramfs/lib/libc.so.6"
cp "$WORK/ld-linux.so.2" "$WORK/initramfs/lib/ld-linux-x86-64.so.2"
cp "$WORK/ld-linux.so.2" "$WORK/initramfs/lib64/ld-linux-x86-64.so.2"
cp "$WORK/libc.so.6" "$WORK/initramfs/lib64/libc.so.6"
if test -n "$VPP_IMAGE"; then
    # The ELF interpreter is selected before LD_LIBRARY_PATH.  Use the same
    # trixie glibc pair for the VPP executable and the base initramfs paths.
    cp "$WORK/initramfs/lib/x86_64-linux-gnu/libc.so.6" \
       "$WORK/initramfs/lib/libc.so.6"
    cp "$WORK/initramfs/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" \
       "$WORK/initramfs/lib/ld-linux-x86-64.so.2"
    cp "$WORK/initramfs/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2" \
       "$WORK/initramfs/lib64/ld-linux-x86-64.so.2"
    cp "$WORK/initramfs/lib/x86_64-linux-gnu/libc.so.6" \
       "$WORK/initramfs/lib64/libc.so.6"
fi
# mgrd built in step 1 lives in the container; fetch fresh copy
docker cp "$BUILD_CONTAINER:/tmp/b/danos-mgrd/danos-mgrd" "$WORK/initramfs/bin/mgrd"
chmod +x "$WORK/initramfs/bin/mgrd"
# mgrd's TLS dependency is present even in this laboratory h2c image.
mkdir -p "$WORK/initramfs/lib/x86_64-linux-gnu"
for lib in libssl.so.3 libcrypto.so.3; do
  tls_lib_path=$(docker exec "$BUILD_CONTAINER" readlink -f "/lib/x86_64-linux-gnu/$lib")
  docker cp "$BUILD_CONTAINER:$tls_lib_path" "$WORK/initramfs/lib/x86_64-linux-gnu/$lib"
done
# Build the bridge from the same source and trixie toolchain as mgrd. Never
# silently import a stale host build with a potentially newer libc ABI.
docker cp "$BUILD_CONTAINER:/tmp/b/danos-test/fib_live_bridge" "$WORK/initramfs/bin/fib_live_bridge"
chmod +x "$WORK/initramfs/bin/fib_live_bridge"
test "$DANOS_FIB_BRIDGE_ENABLE" = 1 && touch "$WORK/initramfs/danos-fib-bridge.enabled"
if test "$DANOS_FIB_BRIDGE_ENABLE" = 1 && test -n "$DANOS_ZEBRA_ENDPOINT"; then
  mkdir -p "$WORK/initramfs/etc/danos"
  printf 'DANOS_ZEBRA_ENDPOINT=%q\n' "$DANOS_ZEBRA_ENDPOINT" > "$WORK/initramfs/etc/danos/bridge.env"
  printf 'DANOS_ZAPI_ROUTE_TYPES=%q\n' "${DANOS_ZAPI_ROUTE_TYPES:-4,10}" >> "$WORK/initramfs/etc/danos/bridge.env"
  printf 'DANOS_ZAPI_PROTOCOL=%q\n' "${DANOS_ZAPI_PROTOCOL:-3}" >> "$WORK/initramfs/etc/danos/bridge.env"
  printf 'DANOS_ZAPI_REDISTRIBUTE_DEFAULT=%q\n' "${DANOS_ZAPI_REDISTRIBUTE_DEFAULT:-0}" >> "$WORK/initramfs/etc/danos/bridge.env"
  printf 'DANOS_ZAPI_HELLO_ONLY=%q\n' "$DANOS_ZAPI_HELLO_ONLY" >> "$WORK/initramfs/etc/danos/bridge.env"
  printf 'DANOS_ZAPI_REG_STAGE=%q\n' "$DANOS_ZAPI_REG_STAGE" >> "$WORK/initramfs/etc/danos/bridge.env"
  printf 'DANOS_ZAPI_PACE_US=%q\n' "$DANOS_ZAPI_PACE_US" >> "$WORK/initramfs/etc/danos/bridge.env"
  printf 'DANOS_SKIP_VPP_RECOVERY=%q\n' "$DANOS_SKIP_VPP_RECOVERY" >> "$WORK/initramfs/etc/danos/bridge.env"
  printf 'DANOS_VPP_HEALTH_PROBE=%q\n' "$DANOS_VPP_HEALTH_PROBE" >> "$WORK/initramfs/etc/danos/bridge.env"
  printf 'DANOS_VPP_RESTART_TEST=%q\n' "$DANOS_VPP_RESTART_TEST" >> "$WORK/initramfs/etc/danos/bridge.env"
  printf 'VPP_IF1_ADDR=%q\n' "$VPP_IF1_ADDR" >> "$WORK/initramfs/etc/danos/bridge.env"
  printf 'VPP_IF2_ADDR=%q\n' "$VPP_IF2_ADDR" >> "$WORK/initramfs/etc/danos/bridge.env"
fi
cp "$PROJECT_ROOT/danos-test/live/init" "$WORK/initramfs/init"
cp "$PROJECT_ROOT/danos-test/live/bind_dpdk_pci.sh" "$WORK/initramfs/bin/bind_dpdk_pci.sh"
chmod +x "$WORK/initramfs/bin/bind_dpdk_pci.sh"
chmod +x "$WORK/initramfs/init"
if test -x "$WORK/gnmic"; then
    cp "$WORK/gnmic" "$WORK/initramfs/bin/gnmic"
    chmod +x "$WORK/initramfs/bin/mgrd" "$WORK/initramfs/bin/gnmic"
else
    chmod +x "$WORK/initramfs/bin/mgrd"
fi
# e1000 module: pre-decompress (busybox insmod can't read .xz)
case "$E1000" in
  *.xz) xz -q -d -c "$WORK/e1000.ko.raw" > "$WORK/initramfs/modules/e1000.ko" ;;
  *)    cp "$WORK/e1000.ko.raw" "$WORK/initramfs/modules/e1000.ko" ;;
esac
case "$VMXNET3" in
  *.xz) xz -q -d -c "$WORK/vmxnet3.ko.raw" > "$WORK/initramfs/modules/vmxnet3.ko" ;;
  *)    cp "$WORK/vmxnet3.ko.raw" "$WORK/initramfs/modules/vmxnet3.ko" ;;
esac
case "$VIRTIO_NET" in
  *.xz) xz -q -d -c "$WORK/virtio_net.ko.raw" > "$WORK/initramfs/modules/virtio_net.ko" ;;
  *)    cp "$WORK/virtio_net.ko.raw" "$WORK/initramfs/modules/virtio_net.ko" ;;
esac
for module in usb_common usbcore input_core hid xhci_hcd xhci_pci \
    ehci_hcd ehci_pci uhci_hcd ohci_hcd ohci_pci usbhid hid_generic \
    failover net_failover virtio_pci uio uio_pci_generic; do
  eval "source=\$${module^^}"
  test -f "$WORK/${module}.ko.raw" || continue
  case "$source" in
    *.xz) xz -q -d -c "$WORK/${module}.ko.raw" > "$WORK/initramfs/modules/${module}.ko" ;;
    *.zst) docker exec "$BUILD_CONTAINER" zstd -q -d -c "$source" > "$WORK/initramfs/modules/${module}.ko" ;;
    *)    cp "$WORK/${module}.ko.raw" "$WORK/initramfs/modules/${module}.ko" ;;
  esac
done

if test "$DANOS_INSTALLER_ENABLE" = 1; then
  : > "$WORK/installer-modules.load"
  while read -r module_name module_path; do
    module_file=${module_path##*/}
    case "$module_path" in
      *.xz) xz -q -d -c "$WORK/$module_file.raw" > "$WORK/initramfs/modules/$module_name.ko" ;;
      *.zst) docker exec "$BUILD_CONTAINER" zstd -q -d -c "$module_path" > "$WORK/initramfs/modules/$module_name.ko" ;;
      *) cp "$WORK/$module_file.raw" "$WORK/initramfs/modules/$module_name.ko" ;;
    esac
    printf '%s\n' "$module_name" >> "$WORK/installer-modules.load"
  done < "$WORK/installer-modules.map"
  cp "$WORK/installer-modules.load" "$WORK/initramfs/modules.load"
  cp "$PROJECT_ROOT/danos-test/installer/install_disk.sh" \
    "$WORK/initramfs/bin/danos-install"
  chmod +x "$WORK/initramfs/bin/danos-install"
fi

# --- 4. pack initramfs ----------------------------------------------------
cd "$WORK/initramfs"
find . | cpio -o -H newc --quiet | gzip -6 > "$WORK/initramfs.cpio.gz"

# --- 5. ISO ---------------------------------------------------------------
rm -rf "$WORK/isoroot"; mkdir -p "$WORK/isoroot/isolinux"
cp "$WORK/vmlinuz" "$WORK/initramfs.cpio.gz" "$WORK/isoroot/"
cp "$WORK/isolinux.bin" "$WORK/ldlinux.c32" "$WORK/isoroot/isolinux/"
if test "$DANOS_INSTALLER_ENABLE" = 1; then
  mkdir -p "$WORK/isoroot/installer"
  cp "$DANOS_INSTALLER_DISK_IMAGE" \
    "$WORK/isoroot/installer/danos-runner-installed.raw.gz"
  cp "$DANOS_INSTALLER_DISK_MANIFEST" \
    "$WORK/isoroot/installer/disk-image.env"
  cp "$PROJECT_ROOT/danos-test/installer/dpdk-installer-isolinux.cfg" \
    "$WORK/isoroot/isolinux/isolinux.cfg"
  cp "$PROJECT_ROOT/danos-test/installer/dpdk-installer-boot.msg" \
    "$WORK/isoroot/isolinux/boot.msg"
else
  cat > "$WORK/isoroot/isolinux/isolinux.cfg" <<'EOF'
SERIAL 0 115200
DEFAULT danos
PROMPT 0
TIMEOUT 20
LABEL danos
  KERNEL /vmlinuz
  APPEND initrd=/initramfs.cpio.gz console=ttyS0,115200 console=tty0
EOF
fi
xorriso -as mkisofs -o "$OUT_ISO" -isohybrid-mbr "$WORK/isohdpfx.bin" \
    -isohybrid-gpt-basdat -b isolinux/isolinux.bin \
    -c isolinux/boot.cat -no-emul-boot -boot-load-size 4 \
    -boot-info-table -J -R "$WORK/isoroot" 2>&1 | tail -1

echo "=== ISO: $OUT_ISO ==="
ls -la "$OUT_ISO"
