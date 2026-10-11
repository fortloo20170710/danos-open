"""Bounded UART RPC for fixed VPP show commands; never a general shell API."""
import base64
import fcntl
import json
import os
import re
import select
import shlex
import subprocess
import termios
import time
import uuid


def decode_reply(raw, marker):
    for complete in raw.split(b"\n")[:-1]:
        line = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", complete.decode(errors="replace"))
        line = line.strip("\r")
        if line.startswith(marker):
            return json.loads(base64.b64decode(line[len(marker):], validate=True))
    return None


def read_command(device, command):
    allowed = command in (["show", "version"], ["show", "interface"],
                          ["show", "ip", "fib", "30.30.30.0/24"])
    if not allowed:
        allowed = (len(command) == 3 and command[:2] == ["show", "hardware-interfaces"]
                   and re.fullmatch(r"GigabitEthernet\d+/\d+/\d+", command[2]))
    if not allowed:
        raise ValueError("UART command outside read-only allowlist")
    marker = "DANOS_RO_RPC_" + uuid.uuid4().hex + ":"
    argv = ["/usr/bin/vppctl", "-s", "/run/vpp/cli.sock", *command]
    script = ("import subprocess,json,base64; "
              f"r=subprocess.run({argv!r},capture_output=True,text=True,timeout=10); "
              f"print({marker!r}+base64.b64encode(json.dumps([r.returncode,r.stdout,r.stderr]).encode()).decode())")
    wire = "python3 -c " + shlex.quote(script) + "\r"
    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        attrs = termios.tcgetattr(fd)
        attrs[0] = attrs[1] = attrs[3] = 0
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[4] = attrs[5] = termios.B115200
        attrs[6][termios.VMIN] = attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        for byte in wire.encode():
            os.write(fd, bytes([byte]))
            time.sleep(.007)
        deadline = time.monotonic() + 15
        raw = b""
        while time.monotonic() < deadline:
            if select.select([fd], [], [], .2)[0]:
                raw += os.read(fd, 65536)
                if len(raw) > 1024 * 1024:
                    raise ValueError("UART response exceeds capture bound")
                # Never decode a partial UART line: base64 may arrive in
                # multiple reads and padding is only present at its end.
                reply = decode_reply(raw, marker)
                if reply is not None:
                    rc, stdout, stderr = reply
                    return subprocess.CompletedProcess(argv, rc, stdout, stderr)
        raise TimeoutError("UART read-only RPC deadline exceeded; tail=" + repr(raw[-800:]))
    finally:
        os.close(fd)
