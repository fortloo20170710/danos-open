#!/bin/bash
# Live FRR zebra ZAPI -> DANOS FIB/DPA -> VPP acceptance harness.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BRIDGE="${BRIDGE:-$ROOT/build/danos-test/fib_live_bridge}"
ZEBRA_SOCK="${ZEBRA_SOCK:-/var/run/frr/zserv.api}"
VPP_SOCK="${VPP_SOCK:-/run/vpp/api.sock}"
MESSAGES="${MESSAGES:-1}"
VPP_IFINDEX_MAP="${DANOS_VPP_IFINDEX_MAP:-}"
VPP_READY_TIMEOUT="${VPP_READY_TIMEOUT:-30}"
VPP_CLI_SOCK="${VPP_CLI_SOCK:-/run/vpp/cli.sock}"
DPA_IFINDEX="${DPA_IFINDEX:-}"
# The bridge's default protocol is FRR 10.3 static (4); subscribe to static
# redistribution unless the caller supplies an explicit type list.
: "${DANOS_ZAPI_ROUTE_TYPES:=4}"
export DANOS_ZAPI_ROUTE_TYPES
PROVISION=1
RECONNECT=0
usage() { echo "usage: $0 [--zebra-sock PATH] [--vpp-sock PATH] [--vpp-cli-sock PATH] [--dpa-ifindex N] [--messages N] [--reconnect] [--no-vpp-provision]"; }
while [ "$#" -gt 0 ]; do
    case "$1" in
        --zebra-sock) ZEBRA_SOCK="$2"; shift 2 ;;
        --vpp-sock) VPP_SOCK="$2"; shift 2 ;;
        --vpp-cli-sock) VPP_CLI_SOCK="$2"; shift 2 ;;
        --dpa-ifindex) DPA_IFINDEX="$2"; shift 2 ;;
        --messages) MESSAGES="$2"; shift 2 ;;
        --no-vpp-provision) PROVISION=0; shift ;;
        --reconnect) RECONNECT=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage; exit 2 ;;
    esac
done
test -x "$BRIDGE" || { echo "[BLOCKED] bridge binary missing: $BRIDGE"; exit 2; }
test -S "$ZEBRA_SOCK" || { echo "[BLOCKED] zebra socket missing: $ZEBRA_SOCK"; exit 2; }
for ((i=0; i<VPP_READY_TIMEOUT; i++)); do
    [ -S "$VPP_SOCK" ] && break
    sleep 1
done
test -S "$VPP_SOCK" || { echo "[BLOCKED] VPP API socket missing: $VPP_SOCK"; exit 2; }
# Container restarts may recreate the socket with root-only permissions.  The
# harness is normally run as the same privileged integration user and can
# restore the documented shared-socket contract.
chmod 666 "$VPP_SOCK" 2>/dev/null || true
test -r "$VPP_SOCK" -a -w "$VPP_SOCK" || {
    echo "[BLOCKED] VPP API socket is not readable/writable: $VPP_SOCK"; exit 2;
}
args=(--zebra-sock "$ZEBRA_SOCK" --vpp-sock "$VPP_SOCK" --messages "$MESSAGES")
[ "$RECONNECT" -eq 1 ] && args+=(--reconnect)
echo "[INFO] running FRR ZAPI -> DPA -> VPP bridge"
if [ "$PROVISION" -eq 1 ]; then
    # A fresh software VPP has only local0; route paths on local0 fail with
    # VPP retval -54.  Ensure a usable loopback dataplane interface exists
    # before the bridge starts, and derive the FRR->VPP sw_if_index map.
    facts=$(VPP_CLI_SOCK="$VPP_CLI_SOCK" bash "$ROOT/danos-test/integration/ensure_vpp_dataplane.sh") || {
        echo "[BLOCKED] cannot provision a VPP dataplane interface"
        exit 2
    }
    eval "$facts"
    echo "[INFO] VPP dataplane: $VPP_DP_IF_NAME sw_if_index=$VPP_DP_SW_IF_INDEX addr=$VPP_DP_IF_ADDR"
    if [ -z "$VPP_IFINDEX_MAP" ] && [ -n "$DPA_IFINDEX" ]; then
        VPP_IFINDEX_MAP="$DPA_IFINDEX:$VPP_DP_SW_IF_INDEX"
    fi
fi
if [ -n "$VPP_IFINDEX_MAP" ]; then
    echo "[INFO] using FRR->VPP ifindex map: $VPP_IFINDEX_MAP"
    export DANOS_VPP_IFINDEX_MAP="$VPP_IFINDEX_MAP"
else
    echo "[WARN] no FRR->VPP ifindex map provided; pass --dpa-ifindex if the FRR ifindex differs"
fi
"$BRIDGE" "${args[@]}"
echo "[PASS] FRR ZAPI -> DPA -> VPP bridge processed $MESSAGES message(s)"
