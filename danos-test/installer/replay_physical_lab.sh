#!/bin/sh
# Optional isolated-lab replay. Not management configuration or ledger replay.
set -eu
profile=${1:-/etc/danos/physical-lab.cli}
client=${DANOS_LAB_VPPCTL:-/usr/bin/vppctl}
socket=${DANOS_LAB_SOCKET:-/run/vpp/cli.sock}
if [ ! -e "$profile" ]; then
    exit 0
fi
test -f "$profile" && test -r "$profile"
while IFS= read -r command || [ -n "$command" ]; do
    [ -n "$command" ] || continue
    case "$command" in
        'set interface state '*|'set interface ip address '*|'ip route add '*|'create loopback interface instance 0') ;;
        *) echo "LAB-REPLAY FAIL: unsupported command" >&2; exit 1 ;;
    esac
    # vppctl may exit zero for a CLI error; require expected output as well.
    output=$(timeout 10 "$client" -s "$socket" "$command") || {
        echo "LAB-REPLAY FAIL: $command" >&2; exit 1;
    }
    output=$(printf '%s' "$output" | tr -d '\r')
    if [ "$command" = 'create loopback interface instance 0' ]; then
        test "$output" = loop0 || { echo "LAB-REPLAY FAIL: $output" >&2; exit 1; }
    else
        test -z "$output" || { echo "LAB-REPLAY FAIL: $output" >&2; exit 1; }
    fi
done < "$profile"
echo "LAB-REPLAY PASS (lab CLI only)"
