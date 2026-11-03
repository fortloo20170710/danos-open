#!/usr/bin/env python3
"""Cross-uid NETCONF authorization check.

The daemon derives the RBAC role from the kernel peer uid, so a viewer uid
that claims `admin` in the socket preamble must still be denied a mutating
RPC, while an admin uid is allowed.  Needs root to create the second uid and
to make the test socket reachable; prints [SKIP] otherwise.

usage: test_netconf_cross_uid.py <danos-netconfd> <danos-netconf-subsystem>
"""

import grp
import os
import pwd
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

EOM = b"]]>]]>"
HELLO = (
    b'<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"><capabilities>'
    b"<capability>urn:ietf:params:netconf:base:1.0</capability></capabilities></hello>"
)
LOCK = b"<rpc><lock><target><candidate/></target></lock></rpc>"


def skip(msg):
    print("[SKIP] " + msg)
    return 0


def wait_socket(path, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
                s.connect(path)
            return True
        except OSError:
            time.sleep(0.05)
    return False


def recv_eom(fd):
    buf = b""
    while EOM not in buf:
        data = os.read(fd, 4096)
        if not data:
            raise AssertionError("peer closed before a complete frame")
        buf += data
    return buf.split(EOM, 1)


def raw_login(path, user, role):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(10)
    s.connect(path)
    s.sendall(b"DANOS-NETCONF/1 %s %s\n" % (user.encode(), role.encode()))
    hello, rest = recv_eom(s.fileno())
    assert b"<hello" in hello, hello
    s.sendall(HELLO + EOM)
    return s, rest


def raw_rpc(s, buf, body):
    s.sendall(body + EOM)
    while EOM not in buf:
        data = os.read(s.fileno(), 4096)
        if not data:
            raise AssertionError("daemon closed before a reply")
        buf += data
    return buf.split(EOM, 1)


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    daemon = os.path.abspath(sys.argv[1])
    subsystem = os.path.abspath(sys.argv[2])

    if os.geteuid() != 0:
        return skip("needs root")
    setpriv = shutil.which("setpriv")
    if not setpriv:
        return skip("setpriv not installed")
    try:
        nobody = pwd.getpwnam("nobody")
        nogroup = grp.getgrgid(nobody.pw_gid)
    except KeyError:
        return skip("no nobody user/group")

    tmp = tempfile.mkdtemp(prefix="danos-xuid-")
    os.chmod(tmp, 0o755)
    sock = os.path.join(tmp, "netconf.sock")
    startup = os.path.join(tmp, "startup.db")
    role_map = os.path.join(tmp, "netconf-roles")
    with open(role_map, "w", encoding="utf-8") as fh:
        fh.write("root admin\nnobody viewer\n")
    os.chmod(role_map, 0o644)

    # setpriv cannot traverse a private build directory; use a copy.
    sub = os.path.join(tmp, "danos-netconf-subsystem")
    shutil.copy2(subsystem, sub)
    os.chmod(sub, 0o755)

    env = dict(os.environ)
    env["DANOS_NETCONF_ROLE_MAP"] = role_map
    proc = subprocess.Popen(
        [daemon, "--socket", sock, "--startup", startup,
         "--socket-mode", "0666"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
    try:
        if not wait_socket(sock):
            print("daemon did not create its socket", file=sys.stderr)
            return 1

        # Root is mapped to admin: a mutating RPC is allowed.
        rs, rbuf = raw_login(sock, "root", "admin")
        reply, rbuf = raw_rpc(
            rs, rbuf, LOCK)
        assert b"<ok/>" in reply, reply
        reply, rbuf = raw_rpc(
            rs, rbuf,
            b"<rpc><unlock><target><candidate/></target></unlock></rpc>")
        assert b"<ok/>" in reply, reply
        rs.close()

        # nobody is mapped to viewer but claims admin in the preamble.  The
        # daemon must use the peer uid policy and deny the lock.
        env2 = dict(os.environ)
        env2.update({
            "LOGNAME": "nobody",
            "USER": "nobody",
            "DANOS_NETCONF_ROLE": "admin",
            "DANOS_NETCONF_ROLE_MAP": role_map,
        })
        cmd = [setpriv, "--reuid", str(nobody.pw_uid),
               "--regid", str(nogroup.gr_gid), "--clear-groups",
               sub, "--daemon", sock]
        viewer = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=env2)
        try:
            ready, _, _ = select.select([viewer.stdout], [], [], 10)
            assert ready, "no server hello for the viewer session"
            hello, rest = recv_eom(viewer.stdout.fileno())
            assert b"<hello" in hello, hello
            viewer.stdin.write(HELLO + EOM)
            viewer.stdin.flush()
            viewer.stdin.write(LOCK + EOM)
            viewer.stdin.flush()
            while EOM not in rest:
                data = os.read(viewer.stdout.fileno(), 4096)
                assert data, "viewer session closed before a reply"
                rest += data
            reply, _ = rest.split(EOM, 1)
            assert b"access-denied" in reply, reply
        finally:
            viewer.kill()
            viewer.wait(timeout=10)

        print("[PASS] cross-uid RBAC: root admin allowed, nobody admin claim "
              "mapped to viewer and denied")
        return 0
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            rc = proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            rc = proc.wait()
        if rc != 0:
            err = proc.stderr.read().decode("utf-8", "replace")
            print("daemon exit %d: %s" % (rc, err), file=sys.stderr)
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
