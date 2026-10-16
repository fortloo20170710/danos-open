# Management gNMI mTLS deployment

P0-1 provides native TLS for the `danos-mgrd` gNMI listener. Production startup
requires all four configuration variables below. Missing or invalid TLS
configuration is fatal; it never falls back to plaintext, including when the
laboratory switch is present.

Current delivery: r14 now contains the explicit VPP programming backend
option described below. Its installed binary passes the full TLS/RBAC suite
and ECMP FIB/WAL recovery in QEMU. See `iso-installation-acceptance.md` for
exact artifact identities. References to an unshipped source candidate or
r13 below describe the earlier qualification stages.

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

## Local peer credentials and identity audit (2026-10-15)

The daemon can serve a local Unix socket instead of the TCP gNMI listener:

```ini
DANOS_GNMI_UNIX_SOCKET=/run/danos/gnmi.sock
DANOS_AUTHZ_PEERCRED=1
DANOS_AUDIT_LOG=/var/log/danos/audit.log
```

Create the socket directory before startup, owned by the daemon user and
not writable by other users. Grant the intended local client group directory
traversal; the socket is created with mode 0660 and the daemon's group.
Existing paths are never removed at startup. Normal shutdown removes the
socket; after an unclean exit, the administrator must remove the stale socket.
This mode opens no TCP gNMI listener and needs neither TLS settings nor
`DANOS_ALLOW_INSECURE`. Combining Unix mode with TLS settings, or omitting
`DANOS_AUTHZ_PEERCRED=1`, fails startup. Metrics retains its separate listener.

Linux `SO_PEERCRED` supplies the UID/PID from the accepted Unix connection.
UID 0 maps to admin; every other UID maps to viewer independently of
`DANOS_AUTHZ_DEFAULT_ROLE`. Bearer headers cannot override this local identity.
Peer credentials cannot authenticate TCP connections. When a credential source
is configured, Capabilities also requires authentication.

For mTLS, authorization audit records retain the full 64-character certificate
fingerprint, the mapped role in `obj_id`, and the RPC method in `diff`.
Local records identify `uid:N pid:N`. gRPC Set records transaction begin and
commit/abort with the same caller identity and transaction ID. Concurrent
connection workers keep independent identities. Certificate authentication
continues to use the existing trust chain, validity, purpose, ALPN and explicit
fingerprint mapping checks described above.

Verification at source base `2afd999` plus these changes: build succeeded and
full CTest **78/78 passed**. `gnmi_mtls` exercised the actual local daemon with
ephemeral PKI over TCP and a real Unix socket, including TLS 1.2/1.3, rejected
credentials, role enforcement, full certificate audit attribution, transaction
attribution, local bearer-override rejection, socket permissions/cleanup,
existing-path preservation, and TCP peercred rejection. The local process ran
as a non-root user, so socket-level tests verified viewer mapping; authz unit
tests additionally verified UID 0 admin mapping. This run does not qualify a
new ISO or deploy PKI to physical routers.

## Service configuration

### Explicit programming backend (source candidate, 2026-10-12)

The daemon defaults to netlink programming, independently of optional VPP
API/stat connectivity. `--backend vpp` explicitly installs the existing VPP
adapter and does not subsequently overwrite it with netlink. If the requested
VPP API cannot connect, startup fails before WAL recovery or listeners;
there is no silent netlink fallback. Unknown backend names and combining
`--backend vpp` with `--no-vpp` are rejected. `--backend netlink` preserves
the existing behaviour; MGRD_NETLINK_REAL controls only that backend.

This option is in the new source candidate, **not in the delivered r13 ISO**.
Do not enable an old physical binary with these arguments. A later image must
be rebuilt and separately installation-qualified. After provisioning PKI on
that updated image, a systemd drop-in can clear ExecStart and replace it with
`/usr/bin/danos-mgrd --backend vpp --port 59200 --metrics-port 59201 --wal
/var/lib/danos/mgrd.wal`. Preserve the environment-file and service protections;
do not enable services on the physical lab until deployment is explicitly scoped.

Candidate activation/read-only qualification: an independent COW guest passed
TLS role-based operational queries and state-write rejection, with actual VPP
connection and `programming pipeline active (vpp)` required in daemon output.
The candidate ran from a read-only seed, without replacing installed mgrd.
Candidate SHA256: `bca8c8fcd70d8210b2fe349058b28f385764b2607145b5f39cf4b7157630c6e0`;
test script: `96bc9fdddd3c12140b7bd0d53b3ebc63af0242776b55e516210b9da055c87fe3`.
Evidence: `build/install-r13-20261012/candidate-vpp-backend-result.json` and
`candidate-vpp-backend.serial.log` (SHA256
`a5b919af7e6c9324a7ab776639e53f3ddde6fe3f78d284c74c31e6a60bb2db0d`).
Coldboot/reboot/persistence and CTest 68/68 pass. This is backend activation,
not route add/withdraw, populated-ledger/FIB readback or physical performance.

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

### Candidate resolved ECMP and persisted-route recovery

2026-10-12: `--management-vpp-ecmp-recovery` now qualifies the separate
candidate in a disposable QEMU child. Admin-mTLS replace drives
`198.18.254.0/24` through two gateways, one gateway, and two gateways again.
Independent exact-prefix FIB blocks require resolved 2/1/2 buckets, the
expected gateway set and no drop DPO, alongside parsed main ledger match
and backend `vpp`. A guest-only loopback with synthetic static neighbors
provides resolution; this is not a packet distribution test.

With mgrd stopped, VPP is restarted (PID 660 -> 839 in this run) and exact
test-route absence is checked. The same-index loopback/neighbor fixture is
recreated by the test, **not recovered by mgrd**. A fresh mgrd with the same
WAL recreates the ECMP route and ledger. After authenticated withdrawal,
another fresh mgrd keeps the route absent. Coldboot and a genuine guest
reboot/persistence check also PASS. This is not live reconnect while mgrd
stays running, interface/neighbor recovery, FRR restart or physical performance.

Evidence: `build/install-r13-20261012/candidate-ecmp-recovery-result.json`
and `candidate-ecmp-recovery.serial.log` (SHA256
`97987fb0df83a68fc0d944808c877ed589ea5e479550a042a762a8af0a9c65ac`).
Executed script SHA256:
`8759d6130e72e32c2a378762f73f43446bfd125440c79f63ef2134b99e2b96df`;
candidate remains `bca8c8fcd70d8210b2fe349058b28f385764b2607145b5f39cf4b7157630c6e0`.
The first attempt failed its precondition because prefix-only CLI deletion
did not clear the API-created route; it is not an accepted recovery run.
The revised test uses actual VPP process replacement and verifies absence
before restart replay, preventing retained FIB from masquerading as recovery.
Guest smoke failures now produce explicit FAIL markers for immediate
verifier failure rather than waiting for the success-marker timeout.

### Authenticated route lifecycle on the explicit VPP candidate

2026-10-12: isolated QEMU COW qualification now includes two actual admin-mTLS
Set add/delete cycles for `198.18.253.0/24`, gateway `198.18.0.2`, VPP oif 1,
default VRF. Each ADD waits for top-level desired/recorded/matches flags and
the typed recorded-backend `vpp` value; each WITHDRAW waits for desired and
recorded to be false. An independent `vppctl show ip fib` confirms exact
prefix presence/absence in every phase, including initial absence. This
qualifies programming/readback, not adjacency resolution or packet transit.

Evidence: `build/install-r13-20261012/candidate-mtls-route-result.json` and
`candidate-mtls-route.serial.log`, SHA256
`16f74fcbf1c624831a019a409bc3b10b4a50d950de67b0d56b59f48540a271c6`.
Executed script SHA256:
`f44c71715c0cb70633838edc0a610bd8b6dde40c6a5ef0c5c54f49d21fac61c9`.
Candidate binary remains `bca8c8fcd70d8210b2fe349058b28f385764b2607145b5f39cf4b7157630c6e0`.
It runs from a separate seed, not the unchanged installed r13 binary.
Coldboot and genuine reboot/persistence also PASS after route removal;
this is not route persistence/replay across a daemon/VPP restart.

The first route run's PASS is retracted: substring matching could accept
retry.recorded=false while the main ledger was still recorded=true. Only
the strict JSON top-level parsing rerun above is authoritative. Regression
assertions now reject this case and retry.matches masking a main mismatch.
The protobuf fixture also supports multibyte lengths for larger route requests.
Use `--management-vpp-route-lifecycle` with an explicit candidate and smoke
script in the QEMU verifier; never invoke this mutating mode on physical
equipment without a separately scoped topology. ECMP/restart replay and
physical deployment/performance remain separate qualification work.

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
