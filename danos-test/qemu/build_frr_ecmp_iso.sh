#!/bin/bash
# Build the QEMU two-port e1000/DPDK ISO used by the FRR route lifecycle gate.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${1:-$ROOT/build/danos-vpp-dpdk-e1000-2port-frr-ecmp.iso}"

# Keep this profile aligned with danos-test/qemu/build_frr_vm_topology.sh and
# verify_frr_vpp_topology.sh. Callers may override any value explicitly.
: "${VPP_IMAGE:=danos-vpp-runtime-recover:local}"
: "${VPP_DPDK_ENABLE:=1}"
: "${VPP_DPDK_DEVICE:=e1000}"
: "${VPP_DPDK_PORTS:=0000:00:02.0 0000:00:03.0}"
: "${VPP_DPDK_NO_RX_INTERRUPTS:=1}"
: "${VPP_DPDK_TRAFFIC_TEST:=1}"
: "${VPP_PING_ENABLE:=1}"
: "${VPP_AUTOSTART:=1}"
: "${VPP_TRAFFIC_READY_ENDPOINT:=tcp://10.0.3.2:2603}"
: "${VPP_TRAFFIC_READY_TIMEOUT:=600}"
: "${VPP_TRAFFIC_TEST_DELAY:=0}"
: "${VPP_ECMP_FAILOVER_PROBE_COUNT:=200}"
: "${VPP_ECMP_FAILOVER_MAX_LOSS_PCT:=15}"
: "${DANOS_FIB_BRIDGE_ENABLE:=1}"
: "${DANOS_ZEBRA_ENDPOINT:=tcp://10.0.3.2:2600}"
: "${DANOS_ZAPI_ROUTE_TYPES:=4,10}"
: "${DANOS_VPP_RESTART_TEST:=1}"
: "${FRR_SHARED_PEER_L2:=0}"
[[ "$FRR_SHARED_PEER_L2" = 0 || "$FRR_SHARED_PEER_L2" = 1 ]] || {
    echo '[FAIL] FRR_SHARED_PEER_L2 must be 0 or 1' >&2; exit 1;
}
if test "$FRR_SHARED_PEER_L2" = 1; then
    # The learned BGP next hop resides on the shared FRR peer subnet.
    # L2 wiring alone does not give VPP an ARP source on that subnet.
    : "${VPP_IF2_EXTRA_ADDRS:=172.31.0.1/24}"
    export VPP_IF2_EXTRA_ADDRS
fi

export VPP_IMAGE VPP_DPDK_ENABLE VPP_DPDK_DEVICE VPP_DPDK_PORTS
export VPP_DPDK_NO_RX_INTERRUPTS VPP_DPDK_TRAFFIC_TEST VPP_PING_ENABLE
export VPP_AUTOSTART VPP_TRAFFIC_READY_ENDPOINT VPP_TRAFFIC_READY_TIMEOUT
export VPP_TRAFFIC_TEST_DELAY DANOS_FIB_BRIDGE_ENABLE DANOS_ZEBRA_ENDPOINT
export VPP_ECMP_FAILOVER_PROBE_COUNT VPP_ECMP_FAILOVER_MAX_LOSS_PCT
export DANOS_ZAPI_ROUTE_TYPES DANOS_VPP_RESTART_TEST

exec bash "$ROOT/danos-test/live/build_live_iso.sh" "$OUT"
