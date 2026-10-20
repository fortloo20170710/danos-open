#!/usr/bin/env python3
"""NETCONF-over-SSH acceptance client.

Connects to a real sshd with an RFC 6242 subsystem and drives a session in
both base 1.0 (end-of-message) and base 1.1 (chunked) framing.

usage: netconf_ssh_client.py <host> <port> <identity-file> [user]
"""

import os
import select
import subprocess
import sys
import time

EOM = b"]]>]]>"

HELLO_10 = (
    b'<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">'
    b"<capabilities><capability>urn:ietf:params:netconf:base:1.0"
    b"</capability></capabilities></hello>"
)
HELLO_11 = (
    b'<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">'
    b"<capabilities><capability>urn:ietf:params:netconf:base:1.0"
    b"</capability><capability>urn:ietf:params:netconf:base:1.1"
    b"</capability></capabilities></hello>"
)
GET = b"<rpc><get-config><source><running/></source></get-config></rpc>"
GET_STARTUP = (
    b"<rpc><get-config><source><startup/></source></get-config></rpc>"
)
COPY_TO_STARTUP = (
    b"<rpc><copy-config><target><startup/></target>"
    b"<source><running/></source></copy-config></rpc>"
)
LOCK = b"<rpc><lock><target><running/></target></lock></rpc>"
UNLOCK = b"<rpc><unlock><target><running/></target></unlock></rpc>"
COMMIT = b"<rpc><commit/></rpc>"
CLOSE = b"<rpc><close-session/></rpc>"


def chunked(msg):
    return b"\n#%x\n" % len(msg) + msg + b"\n##\n"


class Session:
    def __init__(self, host, port, key, user):
        argv = [
            "ssh", "-F", "/dev/null",
            "-i", key,
            "-p", str(port),
            "-o", "StrictHostKeyChecking=no",
            "-o", "UserKnownHostsFile=/dev/null",
            "-o", "BatchMode=yes",
            "-o", "LogLevel=ERROR",
            "-o", "ConnectTimeout=10",
            "-s", "%s@%s" % (user, host),
            "netconf",
        ]
        self.proc = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.buf = b""

    def _fill(self, timeout=15):
        deadline = time.time() + timeout
        while True:
            left = deadline - time.time()
            if left <= 0:
                raise AssertionError("timed out waiting for frame")
            ready, _, _ = select.select([self.proc.stdout], [], [], left)
            if not ready:
                continue
            data = os.read(self.proc.stdout.fileno(), 4096)
            if not data:
                err = self.proc.stderr.read().decode("utf-8", "replace")
                raise AssertionError("ssh session closed early: %s" % err)
            self.buf += data
            return

    def write(self, data):
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def read_eom(self):
        while EOM not in self.buf:
            self._fill()
        msg, self.buf = self.buf.split(EOM, 1)
        return msg

    def read_chunked(self):
        out = b""
        while True:
            while b"\n" not in self.buf[1:]:
                self._fill()
            assert self.buf[0:1] == b"\n", "chunk must start with LF"
            idx = self.buf.index(b"\n", 1)
            line = self.buf[1:idx]
            assert line.startswith(b"#"), "chunk header must start with '#'"
            if line == b"##":
                self.buf = self.buf[idx + 1:]
                return out
            size = int(line[1:], 16)
            start = idx + 1
            while len(self.buf) < start + size:
                self._fill()
            out += self.buf[start:start + size]
            self.buf = self.buf[start + size:]

    def finish(self):
        self.proc.stdin.close()
        rc = self.proc.wait(timeout=15)
        err = self.proc.stderr.read().decode("utf-8", "replace")
        assert rc == 0, "ssh exited %d: %s" % (rc, err)


def run(host, port, key, user, base11):
    s = Session(host, port, key, user)
    hello = s.read_eom()  # server hello is always EOM framed
    assert b"<hello" in hello, hello
    assert b"urn:ietf:params:netconf:base:1.0" in hello
    assert b"urn:ietf:params:netconf:base:1.1" in hello
    assert b"capability:writable-running" in hello
    assert b"capability:candidate" in hello
    assert b"capability:startup" in hello
    assert b"session-id" in hello

    if base11:
        s.write(HELLO_11 + EOM + chunked(GET))
        reply = s.read_chunked()
    else:
        s.write(HELLO_10 + EOM + GET + EOM)
        reply = s.read_eom()
    assert b"<data>" in reply and b"<interfaces" in reply, reply

    # Datastore surface: copy running to startup, read it back, lock and
    # unlock, then commit an empty candidate.
    extra = [COPY_TO_STARTUP, GET_STARTUP, LOCK, UNLOCK, COMMIT]
    if base11:
        s.write(b"".join(chunked(rpc) for rpc in extra))
        replies = [s.read_chunked() for _ in extra]
    else:
        s.write(b"".join(rpc + EOM for rpc in extra))
        replies = [s.read_eom() for _ in extra]
    for idx in (0, 2, 3, 4):
        assert b"<ok/>" in replies[idx], replies[idx]
    assert b"<data>" in replies[1], replies[1]

    if base11:
        s.write(chunked(CLOSE))
        ack = s.read_chunked()
    else:
        s.write(CLOSE + EOM)
        ack = s.read_eom()
    assert b"<ok/>" in ack, ack
    s.finish()
    print("[PASS] NETCONF over SSH, base %s"
          % ("1.1 (chunked)" if base11 else "1.0 (EOM)"))


def main():
    if len(sys.argv) not in (4, 5):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    host, port, key = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    user = sys.argv[4] if len(sys.argv) == 5 else "root"
    run(host, port, key, user, base11=False)
    run(host, port, key, user, base11=True)
    print("=== netconf_ssh_client: ALL PASSED ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
