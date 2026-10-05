# Management gNMI mTLS deployment

P0-1 provides native TLS for the `danos-mgrd` gNMI listener. Production startup
requires all four configuration variables below. Missing or invalid TLS
configuration is fatal; it never falls back to plaintext, including when the
laboratory switch is present.

| Variable | Required value |
| --- | --- |
| `DANOS_TLS_CERT` | PEM server certificate and intermediate chain |
| `DANOS_TLS_KEY` | Matching PEM private key, regular file, mode 0600 or stricter |
| `DANOS_TLS_CLIENT_CA` | PEM trust bundle for client certificates |
| `DANOS_TLS_ROLE_FILE` | Certificate SHA256 fingerprint to explicit role map |

OpenSSL 3 development libraries are required to build the management module.
Debian/Ubuntu native build dependencies: `libssl-dev openssl python3`.
The deployment container installs `libssl3t64`; the laboratory LIVE builder
copies `libssl.so.3` and `libcrypto.so.3` into the initramfs.

## Identity and authorization

Role-file format is one **64-character hex SHA256 of the DER leaf certificate**
and one role per line. Roles are `admin`, `operator`, or `viewer`;
blank lines and whole-line `#` comments are allowed. The table must not be
group/other writable. Empty tables, duplicate fingerprints, malformed hex,
unknown roles, and trailing fields are rejected. Maximum: 256 identities.

Obtain a fingerprint without colon separators:

```sh
openssl x509 -in client.pem -outform DER | sha256sum
```

Copy only the 64-character hash into the role file, followed by the role.
Prefer viewer unless write access is required. A CA-valid certificate absent
from this table cannot open a gNMI session. CN/subject names are not role
assignments. Bearer headers and `DANOS_AUTHZ_DEFAULT_ROLE` cannot elevate an
mTLS session's role. The existing RBAC checks apply to every non-Capabilities
RPC; Get and Subscribe are reads, Set is a write. Viewer writes return gRPC
`PERMISSION_DENIED` (7). Even Capabilities requires a verified, mapped
certificate to complete the TLS transport handshake.

TLS 1.2 and 1.3 are supported; earlier versions are disabled. TLS 1.2 permits
only ECDHE/AEAD cipher suites. HTTP/2 ALPN `h2` is mandatory. Client chain,
validity dates and client-auth certificate purpose are verified by OpenSSL.
No certificate, an unknown CA, an expired certificate, wrong EKU, a missing
identity mapping, plaintext, or a missing/wrong ALPN fails closed. TLS session
resumption/tickets and renegotiation are disabled, so each connection performs
fresh certificate validation. Handshake/read/write operations time out after
10 seconds; at most 128 connection workers are admitted. Stop shuts down and
joins all gNMI connection workers before releasing TLS configuration.

## Service configuration

Place the following values in `/etc/danos/mgrd.env` (protect the file and its
parent directory; use actual certificate paths):

```ini
DANOS_TLS_CERT=/etc/danos/tls/server.pem
DANOS_TLS_KEY=/etc/danos/tls/server.key
DANOS_TLS_CLIENT_CA=/etc/danos/tls/client-ca.pem
DANOS_TLS_ROLE_FILE=/etc/danos/tls/client-roles
DANOS_AUTHZ_REQUIRE_AUTH=1
DANOS_AUDIT_LOG=/var/log/danos/audit.log
```

The systemd unit reads this environment file. The OCI image needs the same
variables and a read-only certificate-directory mount. Clients must verify
the server CA and DNS/IP SAN and present their certificate/key; do not use
insecure or skip-verification client flags. No production keys or CA are
embedded into this repository or the test ISO.

Certificate/role/CA rotation takes effect on daemon restart. Explicitly
remove a compromised fingerprint and restart to revoke its sessions.
Automatic CRL/OCSP checking, online rotation and external identity-directory
integration are not included in this P0-1 implementation.

## Laboratory compatibility and scope

`DANOS_ALLOW_INSECURE=1` explicitly allows h2c **only when no TLS settings are
present**. A warning is emitted. The existing LIVE, kernel and release-smoke
scripts set it for their isolated regression topologies. These ISO images
remain laboratory artifacts, not secure production deployment images.

This change secures the gNMI listener, not every project endpoint. Prometheus
`/metrics` remains plaintext: restrict it using a management network/firewall
or an authenticated TLS proxy. NETCONF over SSH remains a separate outstanding
deployment feature; no new NETCONF network listener is introduced here.

## Repeatable acceptance

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build -R gnmi_mtls --output-on-failure
ctest --test-dir build --output-on-failure
```

`gnmi_mtls` launches the actual daemon and generates ephemeral CAs and keys.
It tests TLS 1.2/1.3, admin/operator/viewer Get/Set/Subscribe with actual interface
mutation/readback, 24 sessions on 12 concurrent workers, certificate-role
precedence over bearer headers, rejected credential/purpose/ALPN cases,
server hostname verification, permission/configuration failures, explicit
laboratory startup, and shutdown with an idle TLS connection. It needs no
VPP runtime, root privileges, persistent PKI, or third-party Python modules.
Source references: [OpenSSL certificate verification](https://docs.openssl.org/3.6/man3/SSL_CTX_set_verify/)
and [OpenSSL ALPN selection](https://docs.openssl.org/3.3/man3/SSL_CTX_set_alpn_select_cb/).
