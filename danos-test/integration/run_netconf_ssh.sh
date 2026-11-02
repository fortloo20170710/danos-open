#!/usr/bin/env bash
# NETCONF-over-SSH acceptance: start a loopback sshd with a `netconf`
# subsystem and drive it with an SSH client.
#
# usage: run_netconf_ssh.sh <path-to-danos-netconf-subsystem> [<danos-netconfd>]
#
# When the daemon path is supplied the subsystem proxies every SSH session to
# one long-lived daemon and the client additionally verifies cross-session
# locks and confirmed-commit persist tokens.
#
# The runner must be able to start sshd. In CI that means root (GitHub
# Actions `sudo`); locally it works for the current user when sshd is
# permitted to bind an unprivileged port.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
binary="${1:?usage: $0 <danos-netconf-subsystem> [<danos-netconfd>]}"
binary="$(readlink -f "$binary")"
daemon="${2:-}"
if [ -n "$daemon" ]; then
    daemon="$(readlink -f "$daemon")"
fi
port="${NETCONF_SSH_PORT:-2222}"
user="${NETCONF_SSH_USER:-$(id -un)}"

tmp="$(mktemp -d)"
pidfile="$tmp/sshd.pid"
sshd_bin="${SSHD:-$(command -v sshd || echo /usr/sbin/sshd)}"
daemon_pid=""
cleanup() {
    if [ -f "$pidfile" ]; then
        kill "$(cat "$pidfile")" 2>/dev/null || true
    fi
    if [ -n "$daemon_pid" ]; then
        kill "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" 2>/dev/null || true
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT

ssh-keygen -q -t ed25519 -N '' -f "$tmp/host_key"
ssh-keygen -q -t ed25519 -N '' -f "$tmp/client_key"
cp "$tmp/client_key.pub" "$tmp/authorized_keys"
chmod 600 "$tmp/authorized_keys" "$tmp/client_key"

# Root logins via key need the privilege separation directory that the
# packaged unit normally creates.
if [ "$(id -u)" -eq 0 ]; then
    mkdir -p /run/sshd
    permit_root="yes"
else
    permit_root="no"
fi

subsystem="Subsystem netconf $binary --startup $tmp/startup.db"
shared=0
if [ -n "$daemon" ]; then
    "$daemon" --socket "$tmp/netconf.sock" --startup "$tmp/startup.db" \
        >"$tmp/netconfd.log" 2>&1 &
    daemon_pid=$!
    for _ in $(seq 1 100); do
        [ -S "$tmp/netconf.sock" ] && break
        sleep 0.05
    done
    if [ ! -S "$tmp/netconf.sock" ]; then
        echo "danos-netconfd did not create its socket" >&2
        cat "$tmp/netconfd.log" >&2 || true
        exit 1
    fi
    subsystem="Subsystem netconf $binary --startup $tmp/startup.db --daemon $tmp/netconf.sock"
    shared=1
fi

cat > "$tmp/sshd_config" <<EOF
Port $port
ListenAddress 127.0.0.1
HostKey $tmp/host_key
PidFile $pidfile
AuthorizedKeysFile $tmp/authorized_keys
PermitRootLogin $permit_root
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
StrictModes no
LogLevel VERBOSE
SetEnv DANOS_NETCONF_ROLE=admin
$subsystem
EOF

"$sshd_bin" -f "$tmp/sshd_config" -E "$tmp/sshd.log"

for _ in $(seq 1 100); do
    if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
        exec 3<&- 3>&-
        break
    fi
    sleep 0.1
done

if ! (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
    echo "sshd did not start on port $port" >&2
    cat "$tmp/sshd.log" >&2 || true
    exit 1
fi
exec 3<&- 3>&- || true

python3 "$here/netconf_ssh_client.py" 127.0.0.1 "$port" "$tmp/client_key" \
    "$user" "$shared"
