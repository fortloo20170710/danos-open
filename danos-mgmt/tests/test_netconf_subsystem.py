#!/usr/bin/env python3
"""Acceptance test for the stdio NETCONF subsystem (RFC 6242 framing).

Runs the subsystem directly over pipes. The SSH half of the transport is
exercised by danos-test/integration/run_netconf_ssh.sh in CI.
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
CLOSE = b"<rpc><close-session/></rpc>"


def chunked(msg):
    return b"\n#%x\n" % len(msg) + msg + b"\n##\n"


class Session:
    def __init__(self, binary, env):
        self.proc = subprocess.Popen(
            [binary],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )
        self.buf = b""

    def _fill(self, timeout=10):
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
                raise AssertionError("subsystem closed stdout early")
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
            while len(self.buf) < 2:
                self._fill()
            assert self.buf[0:1] == b"\n", "chunk must start with LF"
            while b"\n" not in self.buf[1:]:
                self._fill()
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

    def finish(self, expect=0):
        self.proc.stdin.close()
        rc = self.proc.wait(timeout=10)
        err = self.proc.stderr.read().decode("utf-8", "replace")
        assert rc == expect, "exit %d (expected %d): %s" % (rc, expect, err)


def env_for(role="admin"):
    env = dict(os.environ)
    env["DANOS_NETCONF_ROLE"] = role
    # Keep audit writes out of the source tree and away from /var/log.
    env.setdefault("TMPDIR", "/tmp")
    return env


def test_base_1_0(binary):
    s = Session(binary, env_for())
    hello = s.read_eom()
    assert b"<hello" in hello
    assert b"urn:ietf:params:netconf:base:1.0" in hello
    assert b"urn:ietf:params:netconf:base:1.1" in hello
    assert b"capability:candidate" not in hello
    assert b"rollback-on-error" not in hello
    assert b"capability:validate" not in hello
    assert b"<session-id>" in hello

    # Client hello and pipelined RPCs in one write: the bytes after the
    # hello delimiter must reach the message decoder.
    s.write(HELLO_10 + EOM + GET + EOM + CLOSE + EOM)
    reply = s.read_eom()
    assert b"<data>" in reply and b"<interfaces" in reply
    ack = s.read_eom()
    assert b"<ok/>" in ack
    s.finish(0)
    print("[PASS] test_base_1_0: hello, get-config, close-session")


def test_base_1_1(binary):
    s = Session(binary, env_for())
    s.read_eom()  # server hello, always EOM framed

    s.write(HELLO_11 + EOM + chunked(GET) + chunked(CLOSE))
    reply = s.read_chunked()
    assert b"<data>" in reply and b"<interfaces" in reply
    ack = s.read_chunked()
    assert b"<ok/>" in ack
    s.finish(0)
    print("[PASS] test_base_1_1: chunked get-config and close-session")


def test_bad_chunk(binary):
    s = Session(binary, env_for())
    s.read_eom()
    s.write(HELLO_11 + EOM + b"\n#0\n\n##\n")
    s.finish(1)
    print("[PASS] test_bad_chunk: malformed chunked frame is rejected")


def test_unknown_rpc(binary):
    s = Session(binary, env_for())
    s.read_eom()
    s.write(HELLO_10 + EOM + b"<rpc><commit/></rpc>" + EOM)
    reply = s.read_eom()
    assert b"operation-not-supported" in reply
    s.write(CLOSE + EOM)
    assert b"<ok/>" in s.read_eom()
    s.finish(0)
    print("[PASS] test_unknown_rpc: unimplemented RPC is refused")


def main():
    if len(sys.argv) != 2:
        print("usage: test_netconf_subsystem.py <danos-netconf-subsystem>",
              file=sys.stderr)
        return 2
    binary = sys.argv[1]
    test_base_1_0(binary)
    test_base_1_1(binary)
    test_bad_chunk(binary)
    test_unknown_rpc(binary)
    print("=== test_netconf_subsystem: ALL PASSED ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
