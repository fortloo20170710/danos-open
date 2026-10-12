"""Real TLS/gRPC acceptance. Ephemeral CA and certificates, no third-party modules."""
import contextlib
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
import re
from pathlib import Path
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time

BINARY = sys.argv[1]
OPENSSL = sys.argv[2]


def run(*args):
    return subprocess.check_output([OPENSSL, *args], stderr=subprocess.STDOUT)


def port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def certificate(root, name, ca="ca", days="2", purpose="clientAuth"):
    key, csr, cert = (str(root / (name + ext)) for ext in (".key", ".csr", ".pem"))
    run("req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", key,
        "-out", csr, "-subj", "/CN=" + name, "-addext", "extendedKeyUsage=" + purpose,
        "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1")
    if days == "-1":
        (root / "index").touch()
        (root / "serial").write_text("01\n")
        config = root / "ca.conf"
        config.write_text(f"[ca]\ndefault_ca=local\n[local]\ndatabase={root}/index\n"
                          f"serial={root}/serial\nnew_certs_dir={root}\n"
                          f"certificate={root}/{ca}.pem\nprivate_key={root}/{ca}.key\n"
                          "default_md=sha256\npolicy=policy\ncopy_extensions=copy\n"
                          "[policy]\ncommonName=supplied\n")
        run("ca", "-batch", "-config", str(config), "-in", csr, "-out", cert,
            "-startdate", "20200101000000Z", "-enddate", "20200102000000Z", "-notext")
    else:
        run("x509", "-req", "-in", csr, "-CA", str(root / (ca + ".pem")),
            "-CAkey", str(root / (ca + ".key")), "-set_serial", str(time.time_ns()),
            "-days", days, "-copy_extensions", "copy", "-out", cert)
    os.chmod(key, 0o600)
    return cert, key


def frame(kind, flags, stream, payload=b""):
    return len(payload).to_bytes(3, "big") + bytes((kind, flags)) + struct.pack("!I", stream) + payload


def literal(name, value):
    n, v = name.encode(), value.encode()
    assert len(n) < 127 and len(v) < 127
    return b"\x00" + bytes((len(n),)) + n + bytes((len(v),)) + v


def blob(field, value):
    assert field < 16
    length = len(value)
    encoded = bytearray()
    while length >= 128:
        encoded.append((length & 127) | 128)
        length >>= 7
    encoded.append(length)
    return bytes(((field << 3) | 2,)) + encoded + value


def route_lifecycle(root, p):
    """Isolated guest only: actual authenticated Set and independent VPP CLI."""
    prefix = "198.18.253.0/24"
    def fib():
        output = subprocess.check_output(["/usr/bin/vppctl", "-s", "/run/vpp/cli.sock", "show", "ip", "fib"],
                                         stderr=subprocess.STDOUT, timeout=10).decode()
        assert not any(error in output.lower() for error in ("unknown input", "failed to connect")), output
        return output
    assert not re.search(r"^198\.18\.253\.0/24\s*$", fib(), re.M), "test route already exists"
    key = blob(1, b"prefix") + blob(2, prefix.encode())
    path = blob(3, blob(1, b"routes")) + blob(3, blob(1, b"route") + blob(2, key))
    # Core route_key: FNV-1a of vrf, 16 address bytes, prefix length and static protocol.
    ident = 14695981039346656037
    for value in (0, 198, 18, 253, 0, *([0] * 12), 24, 2):
        ident = ((ident ^ value) * 1099511628211) & ((1 << 64) - 1)
    keys = blob(2, blob(1, b"type") + blob(2, b"4")) + blob(2, blob(1, b"id") + blob(2, str(ident).encode()))
    state = blob(3, blob(1, b"dataplane")) + blob(3, blob(1, b"object") + keys) + blob(3, blob(1, b"state"))
    with connect(root, p, "admin") as s:
        s.sendall(b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" + frame(4, 0, 0))
        stream = 1
        def call(method, request, response=None):
            nonlocal stream
            status = rpc(s, method, stream, responses=response, request_override=request)
            stream += 2
            assert status == "0", (method, status)
        for cycle in range(2):
            call("Set", blob(4, blob(1, path) + blob(3, blob(11, b'{"gateway":"198.18.0.2","oif":1}'))))
            for installed in (True, False):
                if not installed:
                    call("Set", blob(2, path))
                deadline = time.monotonic() + 15
                while True:
                    response = []
                    call("Get", blob(2, state), response)
                    payload = b"".join(response)
                    output = fib()
                    present = bool(re.search(r"^198\.18\.253\.0/24\s*$", output, re.M))
                    matched = route_state_converged(payload, installed)
                    if matched and present == installed:
                        print(f"VPP ROUTE cycle={cycle + 1} phase={'ADD' if installed else 'WITHDRAW'} id={ident}")
                        print("STATE", payload.hex())
                        print("INDEPENDENT FIB", output)
                        break
                    assert time.monotonic() < deadline, (installed, payload, output)
                    time.sleep(.2)
    print("mTLS VPP ROUTE PASS: two add/withdraw cycles, ledger attribution and independent FIB; no packet/performance qualification")


def route_state_converged(payload, installed):
    """Compare top-level ledger flags; retry flags must never satisfy them."""
    text = payload.decode(errors="replace")
    start = text.find('{"desired":')
    if start < 0:
        return False
    state, _ = json.JSONDecoder().raw_decode(text[start:])
    if installed:
        return (state.get("desired") is True and state.get("recorded") is True and
                state.get("matches") is True and blob(1, b"vpp") in payload)
    return state.get("desired") is False and state.get("recorded") is False


def ecmp_recovery(root, config):
    """Disposable guest only; synthetic adjacencies qualify FIB, not traffic."""
    def cli(*args):
        output = subprocess.check_output(["/usr/bin/vppctl", "-s", "/run/vpp/cli.sock", *args],
                                         stderr=subprocess.STDOUT, timeout=10).decode()
        assert not any(marker in output.lower() for marker in ("unknown input", "failed", "error")), output
        return output
    prefix = "198.18.254.0/24"
    assert prefix not in cli("show", "ip", "fib")
    name = cli("create", "loopback", "interface").strip()
    assert re.fullmatch(r"loop\d+", name), name
    interfaces = cli("show", "interface")
    match = re.search(r"^" + re.escape(name) + r"\s+(\d+)\s", interfaces, re.M)
    assert match, interfaces
    oif = int(match[1])
    cli("set", "interface", "state", name, "up")
    cli("set", "interface", "ip", "address", name, "198.18.0.1/24")
    for suffix in (2, 3):
        cli("set", "ip", "neighbor", name, f"198.18.0.{suffix}", f"02:00:00:00:00:0{suffix}")
    key = blob(1, b"prefix") + blob(2, prefix.encode())
    path = blob(3, blob(1, b"routes")) + blob(3, blob(1, b"route") + blob(2, key))
    ident = 14695981039346656037
    for value in (0, 198, 18, 254, 0, *([0] * 12), 24, 2):
        ident = ((ident ^ value) * 1099511628211) & ((1 << 64) - 1)
    keys = blob(2, blob(1, b"type") + blob(2, b"4")) + blob(2, blob(1, b"id") + blob(2, str(ident).encode()))
    state = blob(3, blob(1, b"dataplane")) + blob(3, blob(1, b"object") + keys) + blob(3, blob(1, b"state"))
    wal = root / "ecmp-recovery.wal"
    for stage in ("INITIAL", "REPLAY", "DELETED-RESTART"):
        with daemon(root, config, real_vpp=True, program_vpp=True, wal=wal) as p:
            with connect(root, p, "admin") as s:
                s.sendall(b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" + frame(4, 0, 0))
                stream = 1
                def call(method, request, response=None):
                    nonlocal stream
                    status = rpc(s, method, stream, responses=response, request_override=request)
                    stream += 2
                    assert status == "0", (method, status)
                def verify(phase, count):
                    deadline = time.monotonic() + 15
                    while True:
                        response = []
                        call("Get", blob(2, state), response)
                        payload = b"".join(response)
                        output = cli("show", "ip", "fib")
                        block = re.search(r"^198\.18\.254\.0/24\s*\n(.*?)(?=^\d+\.\d+\.\d+\.\d+/\d+\s*$|\Z)", output, re.M | re.S)
                        correct = block is None if count == 0 else (
                            block is not None and f"buckets:{count}" in block[1] and
                            "198.18.0.2" in block[1] and
                            ("198.18.0.3" in block[1]) == (count == 2) and "dpo-drop" not in block[1])
                        if correct and route_state_converged(payload, count != 0):
                            print("ECMP RECOVERY", stage, phase, "buckets", count, "object", ident)
                            print("STATE", payload.hex())
                            print("INDEPENDENT FIB", output)
                            return
                        assert time.monotonic() < deadline, (phase, payload, output)
                        time.sleep(.2)
                if stage == "INITIAL":
                    for phase, gateways in (("ADD", ["198.18.0.2", "198.18.0.3"]),
                                             ("PATH-REMOVE", ["198.18.0.2"]),
                                             ("PATH-RESTORE", ["198.18.0.2", "198.18.0.3"])):
                        value = json.dumps({"gateways": gateways, "oif": oif}, separators=(",", ":")).encode()
                        call("Set", blob(3, blob(1, path) + blob(3, blob(11, value))))
                        verify(phase, len(gateways))
                elif stage == "REPLAY":
                    verify("WAL-REPLAY", 2)
                    call("Set", blob(2, path))
                    verify("WITHDRAW", 0)
                else:
                    verify("NO-RESURRECTION", 0)
        if stage == "INITIAL":
            assert wal.stat().st_size > 0
            # A prefix-only CLI delete did not clear this API-created route.
            # Restart the disposable runtime to prove no retained API FIB.
            old_pid = subprocess.check_output(["systemctl", "show", "-p", "MainPID", "--value", "vpp"], timeout=10).strip()
            assert old_pid not in (b"", b"0")
            subprocess.run(["systemctl", "restart", "vpp"], check=True, timeout=30)
            new_pid = subprocess.check_output(["systemctl", "show", "-p", "MainPID", "--value", "vpp"], timeout=10).strip()
            assert new_pid not in (b"", b"0", old_pid)
            deadline = time.monotonic() + 15
            while not Path("/run/vpp/cli.sock").exists():
                assert time.monotonic() < deadline
                time.sleep(.2)
            assert prefix not in cli("show", "ip", "fib")
            rebuilt = cli("create", "loopback", "interface").strip()
            assert rebuilt == name
            assert re.search(r"^" + re.escape(name) + r"\s+" + str(oif) + r"\s", cli("show", "interface"), re.M)
            cli("set", "interface", "state", name, "up")
            cli("set", "interface", "ip", "address", name, "198.18.0.1/24")
            for suffix in (2, 3):
                cli("set", "ip", "neighbor", name, f"198.18.0.{suffix}", f"02:00:00:00:00:0{suffix}")
            assert prefix not in cli("show", "ip", "fib")
            print("REPLAY PRECONDITION: mgrd stopped; VPP PID", old_pid.decode(), "->", new_pid.decode(), "; test route absent")
    print("mTLS ECMP RECOVERY PASS: 2/1/2 resolved buckets, WAL replay into fresh VPP and no resurrection; synthetic neighbors, no packet/performance claim")


def exact(s, n):
    out = b""
    while len(out) < n:
        chunk = s.recv(n - len(out))
        if not chunk:
            raise ConnectionError("peer closed")
        out += chunk
    return out


def rpc(s, method, stream=1, authorization=None, responses=None, request_override=None):
    headers = b"\x83\x87" + literal(":path", "/gnmi.gNMI/" + method)
    headers += literal("content-type", "application/grpc")
    if authorization:
        headers += literal("authorization", authorization)
    # GetRequest.path = Path.elem = PathElem.name = interfaces.
    request = b"\x12\x0e\x1a\x0c\x0a\x0ainterfaces" if method == "Get" else b""
    if method == "Subscribe":
        request = b"\x0a\x14\x12\x10\x0a\x0e\x1a\x0c\x0a\x0ainterfaces\x28\x01"
    if method == "Set":
        key = blob(1, b"name") + blob(2, b"mtls0")
        path = blob(3, blob(1, b"interfaces")) + blob(3, blob(1, b"interface") + blob(2, key))
        request = blob(4, blob(1, path) + blob(3, blob(11, b'{"mtu":1500}')))
    if request_override is not None:
        request = request_override
    s.sendall(frame(1, 4, stream, headers) +
              frame(0, 1, stream, b"\0" + len(request).to_bytes(4, "big") + request))
    while True:
        h = exact(s, 9)
        payload = exact(s, int.from_bytes(h[:3], "big"))
        if h[3] == 4 and not h[4] & 1:
            s.sendall(frame(4, 1, 0))
        if h[3] == 0 and int.from_bytes(h[5:], "big") == stream and responses is not None:
            responses.append(payload)
        if int.from_bytes(h[5:], "big") == stream and h[3] == 1 and h[4] & 1:
            # Server's trailers use literal (non-Huffman) HPACK fields.
            marker = b"grpc-status"
            pos = payload.index(marker) + len(marker)
            n = payload[pos]
            return payload[pos + 1:pos + 1 + n].decode()


@contextlib.contextmanager
def daemon(root, config, real_vpp=False, program_vpp=False, wal=None):
    p = port()
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("DANOS_TLS_", "DANOS_AUTHZ_")) and k != "DANOS_ALLOW_INSECURE"}
    env.update(config)
    env["DANOS_AUDIT_LOG"] = str(root / "audit.log")
    with tempfile.TemporaryFile() as log:
        proc = subprocess.Popen([BINARY, *(["--backend", "vpp"] if program_vpp else ([] if real_vpp else ["--no-vpp"])), "--port", str(p), "--metrics-port", str(port()),
                                 "--wal", str(wal or root / (str(p) + ".wal"))], env=env,
                                stdout=log, stderr=log)
        try:
            for _ in range(150):
                if proc.poll() is not None:
                    log.seek(0)
                    raise AssertionError("mgrd exited: " + log.read().decode())
                try:
                    with socket.create_connection(("127.0.0.1", p), timeout=0.1):
                        break
                except OSError:
                    time.sleep(0.02)
            else:
                raise AssertionError("mgrd listener timeout")
            yield p
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=15)
            log.seek(0)
            output = log.read().decode()
            assert proc.returncode == 0, output
            assert "AddressSanitizer" not in output and "ThreadSanitizer" not in output, output
            if real_vpp:
                assert "VPP backend connected" in output, output
                print("Installed runtime: VPP backend connected (not FIB readback)")
            if program_vpp:
                assert "programming pipeline active (vpp)" in output, output
                assert "programming pipeline active (netlink" not in output, output
                print("Candidate runtime: explicit VPP programming backend active (not FIB readback)")


def connect(root, p, name, version=None, alpn="h2", hostname="localhost"):
    ctx = ssl.create_default_context(cafile=str(root / "ca.pem"))
    if version:
        ctx.minimum_version = ctx.maximum_version = version
    if alpn:
        ctx.set_alpn_protocols([alpn])
    if name:
        ctx.load_cert_chain(str(root / (name + ".pem")), str(root / (name + ".key")))
    raw = socket.create_connection(("127.0.0.1", p), timeout=3)
    try:
        return ctx.wrap_socket(raw, server_hostname=hostname)
    except BaseException:
        raw.close()
        raise


def rejected(root, p, name, **kwargs):
    try:
        with connect(root, p, name, **kwargs) as s:
            s.sendall(b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" + frame(4, 0, 0))
            rpc(s, "Capabilities")
    except TimeoutError:
        raise AssertionError("invalid client timed out instead of being rejected")
    except (OSError, ConnectionError):
        return
    raise AssertionError("accepted invalid client: " + str(name) + str(kwargs))


def main(root, real_vpp_readonly=False, program_vpp=False, routes=False):
    # Regression: retry metadata must not masquerade as ledger convergence.
    assert not route_state_converged(b'{"desired":false,"recorded":true,"retry":{"recorded":false}}', False)
    assert not route_state_converged(b'{"desired":true,"recorded":true,"matches":false,"retry":{"matches":true}}' + blob(1, b"vpp"), True)
    assert route_state_converged(b'{"desired":false,"recorded":false}', False)
    assert blob(1, b"x" * 128)[:3] == b"\x0a\x80\x01"
    for ca in ("ca", "foreign"):
        run("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
            "-keyout", str(root / (ca + ".key")), "-out", str(root / (ca + ".pem")),
            "-subj", "/CN=" + ca, "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    certificate(root, "server", purpose="serverAuth")
    for name in ("admin", "operator", "viewer", "unmapped"):
        certificate(root, name)
    certificate(root, "expired", days="-1")
    certificate(root, "rogue", ca="foreign")
    certificate(root, "wrong-purpose", purpose="serverAuth")
    roles = root / "roles"
    lines = []
    for name in ("admin", "operator", "viewer", "expired", "rogue", "wrong-purpose"):
        der = ssl.PEM_cert_to_DER_cert((root / (name + ".pem")).read_text())
        lines.append(hashlib.sha256(der).hexdigest() + " " + (name if name in ("admin", "operator", "viewer") else "admin"))
    roles.write_text("\n".join(lines) + "\n")
    os.chmod(roles, 0o600)
    config = {"DANOS_TLS_CERT": str(root / "server.pem"), "DANOS_TLS_KEY": str(root / "server.key"),
              "DANOS_TLS_CLIENT_CA": str(root / "ca.pem"), "DANOS_TLS_ROLE_FILE": str(roles),
              "DANOS_AUTHZ_REQUIRE_AUTH": "1"}
    if "--vpp-ecmp-recovery" in sys.argv[3:]:
        ecmp_recovery(root, config)
        return
    with daemon(root, config, real_vpp=real_vpp_readonly, program_vpp=program_vpp) as p:
        if routes:
            route_lifecycle(root, p)
            return
        if real_vpp_readonly:
            keys = blob(2, blob(1, b"type") + blob(2, b"4")) + blob(2, blob(1, b"id") + blob(2, b"99"))
            path = blob(3, blob(1, b"dataplane")) + blob(3, blob(1, b"object") + keys) + blob(3, blob(1, b"state"))
            for version in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3):
                for role in ("admin", "operator", "viewer"):
                    with connect(root, p, role, version=version) as s:
                        s.sendall(b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" + frame(4, 0, 0))
                        response = []
                        assert rpc(s, "Get", 1, responses=response, request_override=blob(2, path)) == "0"
                        assert b'"desired":false' in b"".join(response)
                        assert b"recorded-backend" in b"".join(response)
                        owner = path + blob(3, blob(1, b"recorded-backend"))
                        assert rpc(s, "Get", 3, request_override=blob(2, owner)) == "0"
                        for stream, operation in ((5, 4), (7, 3), (9, 2)):
                            change = blob(operation, path) if operation == 2 else blob(operation, blob(1, path) + blob(3, blob(11, b'{}')))
                            assert rpc(s, "Set", stream, request_override=change) == ("7" if role == "viewer" else "3")
            print("mTLS VPP READONLY PASS: TLS 1.2/1.3, mapped roles, state Get and Set rejection; no route programming qualification")
            return
        for version in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3):
            for role in ("admin", "operator", "viewer"):
                with connect(root, p, role, version=version) as s:
                    assert s.selected_alpn_protocol() == "h2"
                    s.sendall(b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" + frame(4, 0, 0))
                    assert rpc(s, "Capabilities", 1) == "0"
                    assert rpc(s, "Get", 3) == "0"
                    assert rpc(s, "Set", 5) == ("7" if role == "viewer" else "0")
                    # Bearer headers cannot replace/elevate the certificate role.
                    assert rpc(s, "Set", 7, "Bearer ignored") == ("7" if role == "viewer" else "0")
                    assert rpc(s, "Subscribe", 9) == "0"
                    response = []
                    assert rpc(s, "Get", 11, responses=response) == "0"
                    assert b'"mtu":1500' in b"".join(response)
                    object_keys = (blob(2, blob(1, b"type") + blob(2, b"4")) +
                                   blob(2, blob(1, b"id") + blob(2, b"99")))
                    state_path = (blob(3, blob(1, b"dataplane")) +
                                  blob(3, blob(1, b"object") + object_keys) +
                                  blob(3, blob(1, b"state")))
                    response = []
                    assert rpc(s, "Get", 13, responses=response,
                               request_override=blob(2, state_path)) == "0"
                    assert b'"retry"' in b"".join(response)
                    assert b'"desired":false' in b"".join(response)
                    assert b"recorded-backend" in b"".join(response)
                    for stream, operation in ((15, 4), (17, 3), (19, 2)):
                        change = (blob(operation, state_path) if operation == 2 else
                                  blob(operation, blob(1, state_path) + blob(3, blob(11, b'{}'))))
                        assert rpc(s, "Set", stream, request_override=change) == (
                            "7" if role == "viewer" else "3")
                    owner_path = state_path + blob(3, blob(1, b"recorded-backend"))
                    assert rpc(s, "Get", 21, request_override=blob(2, owner_path)) == "0"
                    owner_change = blob(4, blob(1, owner_path) + blob(3, blob(11, b'"forged"')))
                    assert rpc(s, "Set", 23, request_override=owner_change) == (
                        "7" if role == "viewer" else "3")
        def concurrent_client(_):
            with connect(root, p, "viewer") as s:
                s.sendall(b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" + frame(4, 0, 0))
                assert rpc(s, "Get", 1) == "0"
                assert rpc(s, "Set", 3) == "7"
        with ThreadPoolExecutor(max_workers=12) as pool:
            list(pool.map(concurrent_client, range(24)))
        for name in (None, "expired", "rogue", "wrong-purpose", "unmapped"):
            rejected(root, p, name)
        rejected(root, p, "admin", alpn="http/1.1")
        rejected(root, p, "admin", alpn=None)
        rejected(root, p, "admin", hostname="wrong-host.invalid")
        with socket.create_connection(("127.0.0.1", p), timeout=3) as plain:
            plain.sendall(b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n")
            try:
                assert plain.recv(9) == b""
            except ConnectionResetError:
                pass
        # Stop with a live idle session; shutdown must wake/join TLS workers.
        idle = connect(root, p, "admin")
    idle.close()
    audit = (root / "audit.log").read_text()
    assert "mTLS certificate accepted" in audit and "certificate/identity/ALPN rejected" in audit

    # Bad configurations are fatal even when lab mode was requested.
    for changes in ({"DANOS_TLS_CLIENT_CA": str(root / "missing")},
                    {"DANOS_TLS_KEY": str(root / "viewer.key")},
                    {"DANOS_TLS_ROLE_FILE": str(root / "missing")},
                    {"DANOS_TLS_CERT": ""}):
        env = dict(os.environ, **config, DANOS_ALLOW_INSECURE="1")
        env.update(changes)
        result = subprocess.run([BINARY, "--no-vpp"], env=env, capture_output=True, timeout=5)
        assert result.returncode != 0
    clean = {k: v for k, v in os.environ.items() if not k.startswith(("DANOS_TLS_", "DANOS_AUTHZ_")) and k != "DANOS_ALLOW_INSECURE"}
    for arguments in (("--backend", "unknown"), ("--backend", "vpp", "--no-vpp"),
                      ("--no-vpp", "--backend", "vpp")):
        result = subprocess.run([BINARY, *arguments], env=dict(clean, **config), capture_output=True, timeout=5)
        assert result.returncode == 2 and b"invalid backend arguments" in result.stderr
    result = subprocess.run([BINARY, "--backend", "vpp", "--vpp-sock", str(root / "absent-api.sock")],
                            env=dict(clean, **config), capture_output=True, timeout=5)
    assert result.returncode != 0 and b"refusing fallback" in result.stderr
    assert b"gNMI (" not in result.stdout
    result = subprocess.run([BINARY, "--no-vpp"], env=clean, capture_output=True, timeout=5)
    assert result.returncode != 0 and b"mTLS required" in result.stderr
    # Permission mistakes and ambiguous/malformed authorization tables fail closed.
    key = root / "server.key"
    os.chmod(key, 0o644)
    result = subprocess.run([BINARY, "--no-vpp"], env=dict(clean, **config), capture_output=True, timeout=5)
    assert result.returncode != 0
    os.chmod(key, 0o600)
    for content in ("", lines[0] + "\n" + lines[0] + "\n", "z" * 64 + " admin\n",
                    lines[0].split()[0] + " unknown-role\n"):
        roles.write_text(content)
        result = subprocess.run([BINARY, "--no-vpp"], env=dict(clean, **config), capture_output=True, timeout=5)
        assert result.returncode != 0
    with daemon(root, {"DANOS_ALLOW_INSECURE": "1"}) as p:
        with socket.create_connection(("127.0.0.1", p), timeout=3) as s:
            s.sendall(b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" + frame(4, 0, 0))
            assert rpc(s, "Capabilities") == "0"
    print("mTLS PASS: TLS 1.2/1.3, identity RBAC, negative credentials/ALPN, fail-closed startup, lab opt-in, worker shutdown")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="danos-mtls-") as directory:
        program_vpp = "--vpp-programming-readonly" in sys.argv[3:]
        routes = "--vpp-route-lifecycle" in sys.argv[3:]
        main(Path(directory), real_vpp_readonly=routes or program_vpp or "--vpp-readonly" in sys.argv[3:],
             program_vpp=program_vpp or routes, routes=routes)
