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
# It is idempotent: an existing non-local interface is reused and its address
# is only added when missing.  Set VPP_DP_IF_NAME/VPP_DP_IF_ADDR to override.
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
IF_INDEX=""
read -r cur_name cur_index < <(first_dataplane || true) || true
if [ -n "${cur_name:-}" ]; then
    IF_NAME="$cur_name"
    IF_INDEX="$cur_index"
else
    IF_NAME="${IF_NAME:-loop0}"
    # `create loopback interface` names the new interface loopN and gives it
    # the next sw_if_index.  Reuse an existing loopN if present.
    if ! show_interfaces | awk -v n="$IF_NAME" '$1 == n { found=1 } END { exit !found }'; then
        vppctl_ create loopback interface >/dev/null || {
            echo "[BLOCKED] VPP refused to create a loopback interface" >&2
            exit 2
        }
    fi
    read -r cur_name cur_index < <(first_dataplane || true) || true
    test -n "${cur_name:-}" || {
        echo "[BLOCKED] VPP has no dataplane interface after creating a loopback" >&2
        exit 2
    }
    IF_NAME="$cur_name"
    IF_INDEX="$cur_index"
fi

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
FACTS
