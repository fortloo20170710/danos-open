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

Installed r13 qualification (2026-10-12): the QEMU verifier's
`--management-vpp-readonly` mode requires the actual daemon's VPP API
connection success message. TLS 1.2/1.3 and all mapped roles pass absent-object
state/recorded-backend Get and update/replace/delete rejection. No seed or
interface/route Set is issued. Ephemeral PKI/WAL stay in the disposable COW
guest; the original disk and delivery ISO are unchanged. Fresh-kernel reboot
and persistence also pass, with the production service still inactive.

Evidence: `build/install-r13-20261012/management-vpp-readonly-result.json`
and `management-vpp-readonly.serial.log` (SHA256
`e1ffb0c28c464167867eb13bc0521c8eba92f035fced362b9c75c34811cbb28e`).
Test-script SHA256:
`e05b3104b96e4ae6a12989e4ff18b08d03ce4dbcfdab1fe4c9375cca622259f9`.
Scope is guest API connectivity/authenticated read-only transport, not an
active VPP programming backend, populated ledger, FIB readback, physical PKI
or real PCI performance.

### Native read-only dataplane status (2026-10-12)

gNMI Get supports `/dataplane/object[type=N][id=K]/state`, with numeric
DPA type (1..DANOS_OBJ_MAX-1) and uint64 object ID. Both keys are required;
either key order is accepted. This is a native operational extension in
the path registry, not a claim of new OpenConfig/YANG schema coverage.
Use the existing verified mTLS session and a mapped viewer certificate;
no additional listener, credential bypass or plaintext startup is added.
The explicit laboratory h2c switch still has its existing scope.

JSON fields: `desired`, `recorded`, `matches`, and nested `retry` containing
`recorded`, `matches`, `attempts`, `error`, `backoff`, `after_ms`, `flapping`,
`exhausted`. Fields reflect independently sampled desired/dependency,
ledger and retry metadata, not a hardware readback or atomic snapshot.
An absent object returns false presence flags; `retry.error` is meaningful
only when retry metadata is recorded, and may belong to an older candidate
when `retry.matches=false`. The separate `recorded-backend` leaf attributes
the successful in-memory programming record; it is not hardware ownership.

Only targeted Get is qualified here. CONFIG Get omits operational state;
update/replace/delete fail for admin/operator and are permission-denied
for viewer. No STREAM/ON_CHANGE notification guarantees are added.
The query requires the active global desired store used by programming;
an unrelated alternate desired store is rejected, not silently mixed with
the global ledger. Output capacity errors fail rather than truncate JSON.

The read-only `/state/recorded-backend` leaf returns the backend name copied
into the successful programming ledger record. A container state Get includes
that leaf as a separate string-valued update alongside sampled-state JSON;
it is not substituted with the current active backend. Empty string means
absent or unknown (including a name too long for the 63-byte record field).
Backend names remain in-memory history, not hardware ownership attestation
or dynamic backend-migration support. The leaf follows the same read-only
role policy; TLS tests verify leaf reads and rejected forged writes.

Actual TLS 1.2/1.3 tests query this path with all mapped roles and verify
all three Set operation forms are rejected. Invalid key/type/ID and CONFIG
filtering are tested at the gNMI handler boundary. Deployment to the older
R3 physical image remains pending; no real production PKI was provisioned.

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
