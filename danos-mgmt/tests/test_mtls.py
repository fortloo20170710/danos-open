"""Real TLS/gRPC acceptance. Ephemeral CA and certificates, no third-party modules."""
import contextlib
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
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
    assert field < 16 and len(value) < 128
    return bytes(((field << 3) | 2, len(value))) + value


def exact(s, n):
    out = b""
    while len(out) < n:
        chunk = s.recv(n - len(out))
        if not chunk:
            raise ConnectionError("peer closed")
        out += chunk
    return out


def rpc(s, method, stream=1, authorization=None, responses=None):
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
def daemon(root, config):
    p = port()
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("DANOS_TLS_", "DANOS_AUTHZ_")) and k != "DANOS_ALLOW_INSECURE"}
    env.update(config)
    env["DANOS_AUDIT_LOG"] = str(root / "audit.log")
    with tempfile.TemporaryFile() as log:
        proc = subprocess.Popen([BINARY, "--no-vpp", "--port", str(p), "--metrics-port", str(port()),
                                 "--wal", str(root / (str(p) + ".wal"))], env=env,
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


def main(root):
    for ca in ("ca", "foreign"):
        run("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
            "-keyout", str(root / (ca + ".key")), "-out", str(root / (ca + ".pem")),
            "-subj", "/CN=" + ca, "-addext", "basicConstraints=critical,CA:TRUE")
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
    with daemon(root, config) as p:
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
        main(Path(directory))
