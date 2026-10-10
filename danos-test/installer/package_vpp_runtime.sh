#!/usr/bin/env bash
# Package the already-built Debian trixie VPP runtime image for an offline
# generic DPDK runner root filesystem. This deliberately does not build VPP itself.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
VPP_IMAGE=${VPP_IMAGE:-danos-vpp-runtime-recover:local}
OUT=${1:-"$ROOT/build/danos-vpp-runtime_26.10-1_amd64.deb"}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/danos-vpp-runtime-package.XXXXXX")
CID="danos-vpp-package-$$"
cleanup() {
    docker rm -f "$CID" >/dev/null 2>&1 || true
    rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$WORK/root/usr/bin" \
    "$WORK/root/usr/lib/x86_64-linux-gnu/vpp_plugins" \
    "$WORK/root/usr/lib/systemd/system" \
    "$WORK/root/usr/libexec/danos" \
    "$WORK/root/etc/vpp" \
    "$WORK/root/var/log/vpp" \
    "$WORK/root/DEBIAN" \
    "$WORK/runtime-libs" \
    "$(dirname "$OUT")"

docker create --name "$CID" "$VPP_IMAGE" sleep infinity >/dev/null
docker cp "$CID:/usr/bin/vpp" "$WORK/root/usr/bin/vpp"
docker cp "$CID:/usr/bin/vppctl" "$WORK/root/usr/bin/vppctl"
docker cp "$CID:/usr/lib/x86_64-linux-gnu/vpp_plugins/dpdk_plugin.so" \
    "$WORK/root/usr/lib/x86_64-linux-gnu/vpp_plugins/dpdk_plugin.so"
docker cp "$CID:/usr/lib/x86_64-linux-gnu/vpp_plugins/ping_plugin.so" \
    "$WORK/root/usr/lib/x86_64-linux-gnu/vpp_plugins/ping_plugin.so"
docker cp "$CID:/lib/x86_64-linux-gnu/." "$WORK/runtime-libs"

for lib in "$WORK"/runtime-libs/libv*.so.26.10 \
    "$WORK"/runtime-libs/libsvm*.so.26.10; do
    test -f "$lib" || continue
    install -m 0644 "$lib" "$WORK/root/usr/lib/x86_64-linux-gnu/"
done

install -m 0755 "$ROOT/danos-test/installer/prepare_i211_dpdk.sh" \
    "$WORK/root/usr/libexec/danos/prepare-dpdk-pci"

install -m 0755 "$ROOT/danos-test/live/bind_dpdk_pci.sh" \
    "$WORK/root/usr/libexec/danos/bind_dpdk_pci.sh"
install -m 0755 "$ROOT/danos-test/installer/replay_physical_lab.sh" \
    "$WORK/root/usr/libexec/danos/replay-physical-lab"

cat > "$WORK/root/usr/lib/systemd/system/vpp.service" <<'UNIT'
[Unit]
Description=FD.io VPP packet processor (DANOS generic DPDK runner)
After=local-fs.target systemd-modules-load.service
Before=danos-mgrd.service

[Service]
Type=simple
RuntimeDirectory=vpp
RuntimeDirectoryMode=0755
ExecStartPre=/usr/libexec/danos/prepare-dpdk-pci
ExecStart=/usr/bin/vpp -c /run/vpp/startup.conf
ExecStartPost=/bin/sh -c 'for i in $(seq 1 30); do test -S /run/vpp/api.sock && exit 0; sleep 1; done; exit 1'
ExecStartPost=/bin/sh /usr/libexec/danos/replay-physical-lab
ExecStopPost=-/usr/bin/vppctl -s /run/vpp/cli.sock quit
Restart=on-failure
RestartSec=2
LimitNOFILE=65536
AmbientCapabilities=CAP_SYS_ADMIN CAP_NET_ADMIN CAP_IPC_LOCK CAP_SYS_NICE
CapabilityBoundingSet=CAP_SYS_ADMIN CAP_NET_ADMIN CAP_IPC_LOCK CAP_SYS_NICE CAP_NET_RAW CAP_SYS_RESOURCE
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
UNIT

cat > "$WORK/root/DEBIAN/control" <<'CONTROL'
Package: danos-vpp-runtime
Version: 26.10-1
Section: net
Priority: optional
Architecture: amd64
Depends: libc6 (>= 2.38), libnuma1, libssl3t64, zlib1g, libzstd1
Provides: vpp (= 26.10)
Maintainer: DANOS-Open Project <i-danos@users.noreply.github.com>
Description: VPP 26.10 runtime for the DANOS-Open generic DPDK hardware runner
 Includes the VPP binary, API client, DPDK/ping plugins, systemd service,
 and boot-time PCI Ethernet discovery, management-route exclusion, and automatic VFIO/UIO binding.
CONTROL

chmod 0755 "$WORK/root/usr/bin/vpp" "$WORK/root/usr/bin/vppctl"
dpkg-deb --root-owner-group --build "$WORK/root" "$OUT" >/dev/null
dpkg-deb --field "$OUT" Package Version Architecture Depends Provides
echo "VPP_RUNTIME_DEB=$OUT"
sha256sum "$OUT"
