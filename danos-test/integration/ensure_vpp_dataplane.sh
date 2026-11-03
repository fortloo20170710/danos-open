#!/bin/bash
# Ensure the isolated VPP test runtime has one usable dataplane interface.
#
# A fresh software VPP only has local0 (sw_if_index 0); ip_route_add_del with
# a path on local0 fails (VPP retval -54).  This helper creates a loopback
# with an IPv4 connected subnet and prints shell-evalable facts:
#
#   VPP_DP_IF_NAME=loop0
#   VPP_DP_SW_IF_INDEX=1
#   VPP_DP_IF_ADDR=10.0.0.1/24
#
# It is idempotent: existing non-local interfaces are reused and the first
# interface's address is only added when missing.  VPP_DP_IF_COUNT defaults to
# 2 because the ECMP/route test fixture needs loop0 and loop1.  Set
# VPP_DP_IF_NAME/VPP_DP_IF_ADDR/VPP_DP_IF_COUNT to override.
set -euo pipefail

VPPCTL="${VPPCTL:-vppctl}"
VPP_CLI_SOCK="${VPP_CLI_SOCK:-/run/vpp/cli.sock}"
if [ ! -S "$VPP_CLI_SOCK" ]; then
    for candidate in /run/danos-vpp/cli.sock /run/vpp/cli.sock; do
        if [ -S "$candidate" ]; then VPP_CLI_SOCK="$candidate"; break; fi
    done
fi
test -S "$VPP_CLI_SOCK" || {
    echo "[BLOCKED] VPP CLI socket missing (tried VPP_CLI_SOCK and /run/danos-vpp/cli.sock)" >&2
    exit 2
}
command -v "$VPPCTL" >/dev/null || { echo "[BLOCKED] vppctl missing: $VPPCTL" >&2; exit 2; }

vppctl_() { "$VPPCTL" -s "$VPP_CLI_SOCK" "$@"; }
show_interfaces() { vppctl_ show interface 2>/dev/null; }

# First non-local interface from `show interface`:
#   Name               Idx    State  MTU ...
#   local0              0     down   ...
#   loop0               1      up    ...
first_dataplane() {
    show_interfaces | awk 'NR>1 && $1 != "local0" && $2 ~ /^[0-9]+$/ {
        print $1, $2; exit }'
}

IF_NAME="${VPP_DP_IF_NAME:-}"
IF_ADDR="${VPP_DP_IF_ADDR:-10.0.0.1/24}"
IF_COUNT="${VPP_DP_IF_COUNT:-2}"
IF_INDEX=""

dataplane_count() {
    show_interfaces | awk 'NR>1 && $1 != "local0" && $2 ~ /^[0-9]+$/ { n++ } END { print n+0 }'
}

# VPP's route/ECMP fixture expects loop0 and loop1; a fresh software runtime
# has neither.  Create loopbacks until the requested count exists.
have=$(dataplane_count)
while [ "$have" -lt "$IF_COUNT" ]; do
    vppctl_ create loopback interface >/dev/null || {
        echo "[BLOCKED] VPP refused to create a loopback interface" >&2
        exit 2
    }
    have=$((have + 1))
done

read -r cur_name cur_index < <(first_dataplane || true) || true
test -n "${cur_name:-}" || {
    echo "[BLOCKED] VPP has no dataplane interface after creating loopbacks" >&2
    exit 2
}
IF_NAME="${IF_NAME:-$cur_name}"
IF_INDEX="$cur_index"

vppctl_ set interface state "$IF_NAME" up >/dev/null || {
    echo "[BLOCKED] VPP cannot bring $IF_NAME up" >&2
    exit 2
}
# Add the connected subnet if it is not already present.  A duplicate add is
# harmless but noisy, so check first.
if ! vppctl_ show interface address 2>/dev/null | grep -Fq -- "$IF_ADDR"; then
    vppctl_ set interface ip address "$IF_NAME" "$IF_ADDR" >/dev/null || {
        echo "[BLOCKED] VPP cannot assign $IF_ADDR to $IF_NAME" >&2
        exit 2
    }
fi

cat <<FACTS
VPP_DP_IF_NAME=$IF_NAME
VPP_DP_SW_IF_INDEX=$IF_INDEX
VPP_DP_IF_ADDR=$IF_ADDR
VPP_DP_IF_COUNT=$(dataplane_count)
FACTS
