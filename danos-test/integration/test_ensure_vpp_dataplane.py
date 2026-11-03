#!/usr/bin/env python3
"""Test the VPP dataplane provisioning helper with a fake vppctl.

Covers the fresh-runtime path (only local0 -> create loopback + address),
idempotent rerun (no duplicate create/address) and reuse of an existing
dataplane interface.
"""
import os
import socket
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

HELPER = Path(__file__).with_name("ensure_vpp_dataplane.sh")

FAKE = r"""#!/bin/sh
set -eu
sock="$2"; shift 2
state="$FAKE_STATE"
echo "$*" >> "$FAKE_LOG"
loop_count=0; has_addr=0; up=0
[ -f "$state" ] && . "$state"
save() { printf 'loop_count=%s\nhas_addr=%s\nup=%s\n' "$loop_count" "$has_addr" "$up" > "$state"; }
case "$1 $2" in
  "show interface")
    if [ "${3:-}" = "address" ]; then
      [ "$has_addr" = 1 ] && { echo "loop0 (up):"; echo "  L3 10.0.0.1/24"; }
      exit 0
    fi
    echo "Name               Idx    State  MTU (L3/IP4/IP6/MPLS)"
    echo "local0             0      down          0/0/0/0"
    [ "$loop_count" -ge 1 ] && echo "loop0              1      $([ "$up" = 1 ] && echo up || echo down)            9000/0/0/0"
    [ "$loop_count" -ge 2 ] && echo "loop1              2      up            9000/0/0/0"
    exit 0
    ;;
  "create loopback")
    loop_count=$((loop_count + 1))
    save
    ;;
  "set interface")
    case "$3" in
      state) up=1 ;;
      ip) has_addr=1 ;;
    esac
    save
    ;;
  *) echo "unexpected: $*" >&2; exit 1 ;;
esac
"""


def make_fake(root: Path):
    vppctl = root / "vppctl"
    vppctl.write_text(FAKE)
    vppctl.chmod(vppctl.stat().st_mode | stat.S_IXUSR)
    return vppctl


def run(helper, vppctl, sock, root):
    log = root / "commands.log"
    env = dict(os.environ)
    env.update({
        "VPPCTL": str(vppctl),
        "VPP_CLI_SOCK": str(sock),
        "FAKE_STATE": str(root / "state"),
        "FAKE_LOG": str(log),
    })
    result = subprocess.run(["bash", str(helper)], env=env,
                            text=True, capture_output=True)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    facts = {}
    for line in result.stdout.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            facts[key] = value
    commands = log.read_text() if log.exists() else ""
    return facts, commands


def main():
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        sock = root / "cli.sock"
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(str(sock))
        srv.listen(1)
        vppctl = make_fake(root)

        facts, commands = run(HELPER, vppctl, sock, root)
        assert facts["VPP_DP_IF_NAME"] == "loop0", facts
        assert facts["VPP_DP_SW_IF_INDEX"] == "1", facts
        assert facts["VPP_DP_IF_COUNT"] == "2", facts
        assert commands.count("create loopback") == 2, commands
        assert "set interface ip address loop0 10.0.0.1/24" in commands

        # Second run reuses the interface and does not duplicate creation.
        (root / "commands.log").unlink()
        facts, commands = run(HELPER, vppctl, sock, root)
        assert "create loopback" not in commands, commands
        assert "set interface ip address" not in commands, commands

        # Existing interfaces without address get the address but no create.
        (root / "state").write_text("loop_count=2\nhas_addr=0\nup=0\n")
        (root / "commands.log").unlink()
        facts, commands = run(HELPER, vppctl, sock, root)
        assert "create loopback" not in commands
        assert "set interface ip address loop0 10.0.0.1/24" in commands

        srv.close()
    print("[PASS] ensure_vpp_dataplane: create, idempotent reuse and address repair")
    return 0


if __name__ == "__main__":
    sys.exit(main())
