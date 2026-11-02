#!/usr/bin/env python3
"""Acceptance test for the shared NETCONF daemon.

Two SSH-subsystem proxy processes talk to one `danos-netconfd`; the test
checks that candidate locks, confirmed-commit tokens and session-close
rollback are shared across those sessions, which the per-session subsystem
cannot provide.
"""

import os
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

EOM = b"]]>]]>"
HELLO_10 = (
    b'<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">'
    b"<capabilities><capability>urn:ietf:params:netconf:base:1.0"
    b"</capability></capabilities></hello>"
)
CLOSE = b"<rpc><close-session/></rpc>"


def env_for(role="admin"):
    env = dict(os.environ)
    env["DANOS_NETCONF_ROLE"] = role
    env.setdefault("TMPDIR", "/tmp")
    return env


class Session:
    def __init__(self, binary, daemon_socket, env):
        self.proc = subprocess.Popen(
            [binary, "--daemon", daemon_socket],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )
        self.buf = b""
        self.hello = self.read_eom()
        assert b"<hello" in self.hello, self.hello
        self.write(HELLO_10 + EOM)

    def _fill(self, timeout=10):
        deadline = time.time() + timeout
        while True:
            left = deadline - time.time()
            if left <= 0:
                raise AssertionError("timed out waiting for frame")
            ready, _, _ = select.select([self.proc.stdout], [], [], left)
            if ready:
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

    def rpc(self, body):
        self.write(body + EOM)
        return self.read_eom()

    def close(self):
        self.write(CLOSE + EOM)
        reply = self.read_eom()
        assert b"<ok/>" in reply, reply
        assert self.proc.wait(timeout=10) == 0

    def abort(self):
        """Simulate an SSH channel disconnect without close-session."""
        self.proc.kill()
        self.proc.wait(timeout=10)

    def finish(self):
        assert self.proc.wait(timeout=10) == 0


def raw_connect(path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(path)
    return s


def wait_socket(path, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
                s.connect(path)
            return
        except OSError:
            time.sleep(0.05)
    raise AssertionError("daemon socket did not appear: %s" % path)


def main():
    if len(sys.argv) != 3:
        print("usage: test_netconf_daemon.py <danos-netconfd> "
              "<danos-netconf-subsystem>", file=sys.stderr)
        return 2
    daemon, subsystem = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix="danos-ncd-")
    sock = os.path.join(tmp, "netconf.sock")
    startup = os.path.join(tmp, "startup.db")
    proc = subprocess.Popen(
        [daemon, "--socket", sock, "--startup", startup],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        env=env_for(),
    )
    try:
        wait_socket(sock)
        env = env_for()

        a = Session(subsystem, sock, env)
        b = Session(subsystem, sock, env)

        # Locks are global: a second session is refused.
        assert b"<ok/>" in a.rpc(
            b"<rpc><lock><target><candidate/></target></lock></rpc>")
        reply = b.rpc(b"<rpc><lock><target><candidate/></target></lock></rpc>")
        assert b"lock-denied" in reply, reply
        reply = b.rpc(
            b"<rpc><edit-config><target><candidate/></target><config>"
            b"<interfaces/></config></edit-config></rpc>")
        assert b"in-use" in reply, reply
        assert b"<ok/>" in a.rpc(
            b"<rpc><unlock><target><candidate/></target></unlock></rpc>")

        # A persist token issued by A can be confirmed by B, even while A
        # still holds the running datastore lock.
        assert b"<ok/>" in a.rpc(
            b"<rpc><lock><target><running/></target></lock></rpc>")
        assert b"<ok/>" in a.rpc(
            b"<rpc><commit><confirmed/><confirm-timeout>60</confirm-timeout>"
            b"<persist>x-token</persist></commit></rpc>")
        reply = b.rpc(b"<rpc><commit><persist-id>wrong</persist-id></commit></rpc>")
        assert b"access-denied" in reply, reply
        assert b"<ok/>" in b.rpc(
            b"<rpc><commit><persist-id>x-token</persist-id></commit></rpc>")
        # The token is single-use.
        reply = b.rpc(
            b"<rpc><commit><persist-id>x-token</persist-id></commit></rpc>")
        assert b"invalid-value" in reply, reply
        assert b"<ok/>" in a.rpc(
            b"<rpc><unlock><target><running/></target></unlock></rpc>")

        # A non-persisted commit belongs to its issuing session only.
        assert b"<ok/>" in a.rpc(
            b"<rpc><commit><confirmed/><confirm-timeout>60</confirm-timeout>"
            b"</commit></rpc>")
        reply = b.rpc(b"<rpc><cancel-commit/></rpc>")
        assert b"access-denied" in reply, reply
        assert b"<ok/>" in a.rpc(b"<rpc><cancel-commit/></rpc>")

        # The timer runs in the daemon, not in either proxy process.
        assert b"<ok/>" in a.rpc(
            b"<rpc><commit><confirmed/><confirm-timeout>1</confirm-timeout>"
            b"<persist>timed</persist></commit></rpc>")
        time.sleep(1.6)
        reply = b.rpc(
            b"<rpc><commit><persist-id>timed</persist-id></commit></rpc>")
        assert b"invalid-value" in reply, reply

        # Closing the issuing session releases its lock for another session.
        assert b"<ok/>" in a.rpc(
            b"<rpc><lock><target><candidate/></target></lock></rpc>")
        a.close()
        assert b"<ok/>" in b.rpc(
            b"<rpc><lock><target><candidate/></target></lock></rpc>")

        # Closing the issuing session rolls a non-persisted commit back.
        assert b"<ok/>" in b.rpc(
            b"<rpc><commit><confirmed/><confirm-timeout>60</confirm-timeout>"
            b"</commit></rpc>")
        b.abort()
        c = Session(subsystem, sock, env)
        deadline = time.time() + 5
        while True:
            reply = c.rpc(b"<rpc><cancel-commit/></rpc>")
            if b"invalid-value" in reply:
                break
            assert time.time() < deadline, reply
            time.sleep(0.05)
        c.close()

        print("[PASS] test_netconf_daemon: shared locks, persist tokens, "
              "timer and session-close rollback")

        # Malformed and missing identity preambles are refused without
        # disturbing the daemon.
        with raw_connect(sock) as s:
            s.sendall(b"not-a-preamble\n")
            assert s.recv(1) == b""
        with raw_connect(sock) as s:
            s.shutdown(socket.SHUT_WR)
            assert s.recv(1) == b""

        # A client that never speaks NETCONF base 1.0 is disconnected.
        d = Session.__new__(Session)
        d.proc = subprocess.Popen(
            [subsystem, "--daemon", sock],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=env)
        d.buf = b""
        d.read_eom()  # server hello
        d.write(b"<hello><capabilities/></hello>" + EOM)
        assert d.proc.wait(timeout=10) == 0
        d.proc.stdout.close()
        d.proc.stderr.close()

        # SIGTERM with a live session tears it down cleanly.
        live = Session(subsystem, sock, env)
        proc.send_signal(signal.SIGTERM)
        assert proc.wait(timeout=10) == 0
        live.proc.wait(timeout=10)
        proc = None
    finally:
        rc = 0
        err = ""
        if proc is not None:
            proc.send_signal(signal.SIGTERM)
            try:
                rc = proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                rc = proc.wait()
            err = proc.stderr.read().decode("utf-8", "replace")
        shutil.rmtree(tmp, ignore_errors=True)
    if rc != 0:
        print("daemon exit %d: %s" % (rc, err), file=sys.stderr)
        return 1
    print("=== test_netconf_daemon: ALL PASSED ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
