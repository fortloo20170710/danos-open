"""Bounded UART RPC for fixed VPP show commands and installed-system inventory."""
import base64
import fcntl
import json
import os
import re
import select
import subprocess
import stat
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


def _wire_command(script):
    """Keep UART input lines short and remote Python source single-line.

    Some login shells have bracketed-paste / line-editing modes enabled; raw
    newlines in a ``python3 -c`` payload can be interpreted as separate shell
    commands, and long physical-console lines can exceed the tty input buffer.
    Send the Base64 payload through short shell-variable append lines, then
    execute one compact bootstrap. No remote file is created.
    """
    encoded = base64.b64encode(script.encode()).decode()
    commands = ["__DANOS_UART_B64=\r"]
    for offset in range(0, len(encoded), 256):
        chunk = encoded[offset:offset + 256]
        commands.append('__DANOS_UART_B64="${__DANOS_UART_B64}' + chunk + '"\r')
    bootstrap = "import base64;exec(compile(base64.b64decode('${__DANOS_UART_B64}'),'<uart-rpc>','exec'))"
    commands.append('python3 -c "' + bootstrap + '"\r')
    return "".join(commands)


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
    rc, stdout, stderr = _exchange(device, script, marker)
    return subprocess.CompletedProcess(argv, rc, stdout, stderr)


def read_installed_inventory(device):
    """Read fixed runtime identity/status paths; never read TLS keys or change services."""
    marker = "DANOS_RO_RPC_" + uuid.uuid4().hex + ":"
    commands = {
        "root": ["findmnt", "-n", "-o", "SOURCE,FSTYPE", "/"],
        "kernel": ["uname", "-r"],
        "disks": ["lsblk", "-J", "-o", "NAME,TYPE,TRAN,SIZE,MODEL,MOUNTPOINTS"],
        "pci": ["lspci", "-nnk"],
        "services": ["systemctl", "show", "vpp", "frr", "danos-mgrd", "-p", "Id,ActiveState,SubState,MainPID,UnitFileState"],
        "vpp_interfaces": ["/usr/bin/vppctl", "-s", "/run/vpp/cli.sock", "show", "interface"],
        "vpp_version": ["/usr/bin/vppctl", "-s", "/run/vpp/cli.sock", "show", "version"],
    }
    paths = ["/etc/danos/build-info.env", "/etc/danos/runner-profile", "/proc/cmdline", "/proc/uptime"]
    script = ("import subprocess,json,base64,hashlib; from pathlib import Path; "
              f"commands={commands!r}; "
              "results={k:dict(returncode=r.returncode,stdout=r.stdout,stderr=r.stderr) for k,a in commands.items() "
              "for r in [subprocess.run(a,capture_output=True,text=True,timeout=10)]}; "
              f"results['files']={{p:Path(p).read_text() if Path(p).is_file() else None for p in {paths!r}}}; "
              "results['mgrd_sha256']=hashlib.sha256(Path('/usr/bin/danos-mgrd').read_bytes()).hexdigest(); "
              "results['firmware']='UEFI' if Path('/sys/firmware/efi').exists() else 'BIOS'; "
              "results['sockets']={p:Path(p).is_socket() for p in ['/run/vpp/api.sock','/run/vpp/stats.sock','/run/vpp/cli.sock']}; "
              f"print({marker!r}+base64.b64encode(json.dumps(results).encode()).decode())")
    return _exchange(device, script, marker)


def _exchange(device, script, marker, reply_timeout=15):
    if not 0 < reply_timeout <= 60:
        raise ValueError("bounded UART reply timeout required")
    wire = _wire_command(script)
    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        if not stat.S_ISCHR(os.fstat(fd).st_mode):
            raise ValueError("UART device must be a character device")
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        attrs = termios.tcgetattr(fd)
        attrs[0] = attrs[1] = attrs[3] = 0
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[4] = attrs[5] = termios.B115200
        attrs[6][termios.VMIN] = attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        # Abort any stale unmatched quote/continuation prompt left by an
        # interrupted earlier RPC before beginning a fresh bounded command.
        os.write(fd, b"\x03")
        time.sleep(.15)
        for byte in wire.encode():
            os.write(fd, bytes([byte]))
            time.sleep(.007)
        deadline = time.monotonic() + reply_timeout
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
                    return reply
        raise TimeoutError("UART read-only RPC deadline exceeded; tail=" + repr(raw[-800:]))
    finally:
        os.close(fd)
