# NETCONF over SSH

DANOS-Open serves NETCONF (RFC 6241) through the OpenSSH server. OpenSSH owns
the transport, encryption and user authentication; DANOS provides a NETCONF
subsystem process that speaks RFC 6242 framing on its stdin/stdout.

## Components

| Piece | Role |
|-------|------|
| `sshd` | SSH transport, host keys, user authentication |
| `danos-netconfd` | Shared daemon: one process hosting every NETCONF session and the datastores |
| `danos-netconf-subsystem` | Per-session SSH process: identity preamble and byte relay to the daemon; in-process fallback without one |
| `danos-mgmt` (`netconf.c`) | Message layer: datastore selection and RPC handling |
| `danos-mgmt` (`netconf_datastore.c`) | Running/candidate/startup state, locks, confirmed-commit timer, startup file |
| `danos-mgmt` (`netconf_session.c`) | Transport-independent session engine: hello, framing, dispatch |
| `danos-mgmt` (`netconf_framing.c`) | RFC 6242 framing: EOM and chunked |

OpenSSH starts one subsystem process per SSH session. The recommended
deployment runs `danos-netconfd`; the subsystem connects to its Unix socket,
sends the SSH user plus `DANOS_NETCONF_ROLE`, and relays the NETCONF bytes
unchanged. One daemon therefore owns running/candidate/startup, the lock table
and the confirmed-commit timer for all sessions. Without a daemon the
subsystem runs the same session engine in-process (the previous laboratory
behaviour).

## Server configuration

Install the daemon, subsystem and systemd unit (`danos-netconfd.service`,
which creates `/run/danos` and `/var/lib/danos`) and add the subsystem to
`sshd_config`. Start `danos-netconfd` with the same startup path before
accepting sessions:

```
systemctl enable --now danos-netconfd
# equivalent manual form:
danos-netconfd --socket /run/danos/netconf.sock \
  --startup /var/lib/danos/netconf-startup.db
```

```
Port 830
HostKey /etc/ssh/ssh_host_ed25519_key
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no

# Map the SSH user's DANOS role. admin can edit; operator can read and edit;
# viewer can read. Without this the deployment default applies
# (DANOS_AUTHZ_DEFAULT_ROLE, ADMIN when unset).
SetEnv DANOS_NETCONF_ROLE=admin

Subsystem netconf /usr/local/sbin/danos-netconf-subsystem --daemon /run/danos/netconf.sock
```

### Socket trust boundary and RBAC

The Unix socket is a local authorization boundary, not just an audit channel.
The daemon authenticates the claimed user against `SO_PEERCRED` and fails
closed: an unavailable peer credential, an unresolvable user, or a uid that
does not match the claimed name rejects the connection before the NETCONF
hello.

The RBAC role is **not** taken from the client. The subsystem's
`DANOS_NETCONF_ROLE` (and the role field in the socket preamble) is audit
context only. The daemon derives the effective role from the peer uid:

- default policy: uid 0 is `admin`, every other uid is `viewer`;
- `DANOS_NETCONF_ROLE_MAP` (default `/etc/danos/netconf-roles`) overrides the
  default with one `<user-or-uid> <role>` mapping per line. Unknown users
  remain `viewer`; unknown role names are ignored.

```
# /etc/danos/netconf-roles
root admin
netadmin operator
1001 viewer
```

Keep the mapping file root-owned and not group/other writable; it grants
authorization to local uids.

The packaged unit uses a root-owned, root-only `/run/danos`, so only root
subsystems can connect. To serve non-root SSH users, provision a dedicated
group, set `RuntimeDirectoryMode=0750` with that group in a unit override, and
add the subsystem users to it. Do not make the socket world-writable: any
local process that can connect is authenticated by uid, but a world-writable
socket still widens the attack surface. The subsystem process is spawned by
`sshd` as the authenticated user, so the daemon does not see the password or
key.

Restart `sshd` after the change. A client connects with the subsystem name:

```
ssh -s netconf user@router
```

Use key-only authentication. The subsystem never sees the password or key; it
trusts the `LOGNAME`/`USER` value and the `DANOS_NETCONF_ROLE` set by `sshd`.

## Authorization and audit

`sshd` authenticates the user. The subsystem records that user as the session
identity and maps `DANOS_NETCONF_ROLE` through RBAC:

- `admin` and `operator` may run `edit-config`.
- `viewer` may run `get` and `get-config`.
- An unknown role name is reported on stderr and the deployment default applies.

Every RPC decision is evaluated by `danos_authz_check_identity`, so permitted
and denied operations both reach the audit log with the SSH user attached.

## Capabilities and RPCs

The server hello advertises only behavior that exists:

| Capability | Status |
|------------|--------|
| `urn:ietf:params:netconf:base:1.0` | supported (end-of-message framing) |
| `urn:ietf:params:netconf:base:1.1` | supported (chunked framing) |
| `urn:ietf:params:netconf:capability:writable-running:1.0` | supported |
| `urn:ietf:params:netconf:capability:candidate:1.0` | supported |
| `urn:ietf:params:netconf:capability:startup:1.0` | supported |
| `urn:ietf:params:netconf:capability:validate:1.0` | supported |
| `urn:ietf:params:netconf:capability:confirmed-commit:1.0` | supported |
| `urn:danos:yang:1.0` | supported |

Supported RPCs: `get`, `get-config`, `edit-config`, `commit`, `validate`,
`cancel-commit`, `discard-changes`, `lock`, `unlock`, `copy-config`,
`delete-config` and `close-session`.

`kill-session` returns an `rpc-error` with `operation-not-supported`. The
server advertises no rollback-on-error capability.

`edit-config` applies every `<interface>` block in the request as one
transaction. Interface names and other text in replies are XML-escaped.

## Configuration datastores

| Datastore | Storage | Writable through |
|-----------|---------|------------------|
| running | process object store, DPA transactions | `edit-config`, `commit`, `copy-config` |
| candidate | snapshot of running, created on first use | `edit-config`, `copy-config`, `discard-changes` |
| startup | checksummed file, loaded at subsystem start | `copy-config`, `delete-config` |

The candidate is a full snapshot taken from running before the first access.
Edits accumulate in the snapshot; `get-config` with `<source><candidate/>`
reads it, and `commit` publishes the difference to running in one DPA
transaction (creates, updates and deletes). `discard-changes` drops the
snapshot, after which the next read or edit starts from running again.

The startup datastore lives in a versioned file with a CRC32 per record. The
default path is `/var/lib/danos/netconf-startup.db`; set
`DANOS_NETCONF_STARTUP`, or pass `--startup <path>` to the subsystem, to
override it. Writes use a temporary file, `fsync` and `rename`, so a crash
leaves either the old or the new snapshot. A missing
file is an empty startup datastore. A corrupt file is reported as
`invalid-value`; `delete-config` on startup replaces it with a valid empty
file.

`copy-config` supports the named datastores among running, candidate and
startup. An inline `<config>` source and `<url>` sources are not implemented.

## validate

`<validate>` accepts either a named datastore or an inline `<config>`:

```xml
<rpc><validate><source><candidate/></source></validate></rpc>
<rpc><validate><config>...</config></validate></rpc>
```

For a named datastore the server checks every stored interface payload. For an
inline config it resolves each `<interface>` block against the running store
and applies `mtu` and `enabled` to a copy, so unknown interfaces and invalid
leaf values fail without changing anything. A request with neither `<source>`
nor `<config>` returns `invalid-value`.

## Confirmed commit

`<commit><confirmed/></commit>` publishes the candidate and keeps a baseline
snapshot of running. Unless a confirming `<commit/>` arrives first, running is
restored from that baseline when the timer expires. The default timeout is 600
seconds; `<confirm-timeout>` sets a different positive value.

`<persist>` attaches a token to the pending commit. The commit then survives
the issuing session; `<commit><persist-id>token</persist-id></commit>` or
`<cancel-commit><persist-id>token</persist-id></cancel-commit>` confirms or
reverts it. Without `<persist>`, closing the issuing session rolls the commit
back immediately. A token that does not match returns `access-denied`.
`<persist-id>` is refused together with `<confirmed/>`, and `<persist>`,
`<confirm-timeout>` require `<confirmed/>`.

With `danos-netconfd` the pending confirmed commit, its rollback baseline and
its timer live in the daemon process, so a `<persist>` token issued by one SSH
session can be confirmed or cancelled by a later, different SSH session, even
after the issuing session closes. A confirming `<commit><persist-id>` is
accepted by token ownership even while the issuing session still holds a
datastore lock, because it publishes no new candidate state. Closing a session
without a token still rolls its commit back immediately.

The timer and baseline are process-local: a daemon restart loses a pending
confirmed commit. Rollback-after-reboot therefore remains unqualified, because
the in-memory running store is rebuilt and there is no durable pending-commit
record. Keep `<persist>` out of a release gate that requires RFC 6241 reboot
recovery until the running store itself is durable. Candidate locks follow
RFC 6241 7.5 for both plain and confirmed commits; a cross-session `persist-id`
confirmation skips the lock check only when the candidate is empty (there is
no queued edit to publish).

Locks follow RFC 6241: a lock belongs to one session, blocks writes to that
datastore by other sessions (`in-use`), refuses a second lock
(`lock-denied`), and is released by the owner's `unlock` or `close-session`.

`edit-config` still covers only the interface `mtu` and `enabled` leaves, for
interfaces that already exist in the selected datastore.

### Process model

`danos-netconfd` hosts every session in one process: candidate edits, locks,
the running store and a pending confirmed commit are shared, which is what
RFC 6241 requires. The daemon serialises RPC handling across sessions and
keeps the confirmed-commit timer on its own thread. Session threads are
independent; a disconnect releases that session's locks and rolls back its
non-persisted confirmed commit.

The subsystem still falls back to an in-process session engine when no socket
is reachable and no daemon path was configured explicitly. That mode is
single-session and is kept for laboratory use and for tests that do not start
a daemon; it does not provide cross-session state.

## Framing limits

- Base 1.0 messages end with `]]>]]>`.
- Base 1.1 is used only when the client advertises it; the server then sends
  chunked replies and accepts chunked RPCs.
- Maximum assembled message: 8 MiB.
- Maximum declared chunk: 4 MiB.
- Chunk sizes are strict hexadecimal; a zero-size chunk, a missing `#`, a
  missing LF or an oversized declaration terminates the session.

These are local policy bounds; RFC 6242 defines no limit.

## Verification

Local (stdio, no SSH):

```
ctest -R 'netconf'           # netconf, netconf_framing, netconf_subsystem
```

The `netconf_subsystem` test drives the binary over pipes in both framings,
including pipeline bytes after the hello delimiter, chunked reassembly and
malformed-chunk rejection.

With a real `sshd` (root, or a user permitted to run `sshd`):

```
sudo bash danos-test/integration/run_netconf_ssh.sh \
  build/danos-mgmt/danos-netconf-subsystem \
  build/danos-mgmt/danos-netconfd
```

Passing the daemon binary also runs the cross-session client check: two
concurrent SSH sessions must share candidate locks, a persist token must be
cancellable from the other session, and a non-persisted commit must be
session-owned. Omitting the daemon argument exercises the in-process
single-session fallback.

The script generates a throwaway host key, client key, `authorized_keys` and
`sshd_config`, starts `sshd` on loopback port 2222, and runs
`danos-test/integration/netconf_ssh_client.py` for base 1.0 and base 1.1. The
Interop Gate workflow runs the same script in the `netconf-ssh` job.

## Known limits

- Candidate state, locks and pending confirmed-commit state are shared between
  SSH sessions while `danos-netconfd` runs. A daemon restart loses a pending
  confirmed commit and its rollback baseline; durable pending-commit recovery
  across reboot remains open (see "Confirmed commit").
- The daemon socket is a local trust boundary. Run it in a root-owned
  directory and do not expose it to unprivileged local writers. Roles come
  from the uid policy, never from the client preamble.
- A silent client is bounded by a 5 s preamble timeout and a 10 s hello
  timeout, with a 128-connection cap, but there is no idle-session timeout
  after the hello. A second daemon is refused by a lock file, and the socket
  is unlinked only when its inode still belongs to that instance.
- `copy-config` does not accept an inline `<config>` source or `<url>`.
- `edit-config` covers interface `mtu` and `enabled`. Other subtrees are not
  modeled by the server yet.
- The subsystem trusts `sshd` for identity; it does not re-verify the peer.
- Replies are built in memory, so a very large `get-config` is bounded by the
  8 MiB message limit rather than streamed.
