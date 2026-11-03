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
sends the SSH user plus the requested role as audit context, and relays the
NETCONF bytes unchanged. One daemon therefore owns running/candidate/startup, the lock table
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

# Authorization is resolved by the daemon from the peer uid via
# /etc/danos/netconf-roles.  DANOS_NETCONF_ROLE (SetEnv) is only audit context;
# it does not grant a role.
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
  default with one `<user-or-uid> <role>` or `@<group> <role>` mapping per
  line. Rules are evaluated in file order and the first match wins. Unknown
  users remain `viewer`; unknown role names are ignored.

```
# /etc/danos/netconf-roles
root admin
netadmin operator
@netops operator
1001 viewer
```

Keep the mapping file root-owned and not group/other writable; it grants
authorization to local uids and groups. Group matching includes the primary
group and supplementary groups from NSS.

The packaged unit uses a root-owned, root-only `/run/danos`, so only root
subsystems can connect. To serve non-root SSH users, create a dedicated group,
add those users to it, and make the daemon's service group that group:

```
groupadd danos-netconf
usermod -aG danos-netconf alice
systemctl edit danos-netconfd
# [Service]
# Group=danos-netconf
```

The unit already sets `RuntimeDirectoryMode=0750`, so `/run/danos` becomes
`root:danos-netconf` and the daemon creates the socket as
`root:danos-netconf 0660`. Alternatively pass
`--socket-group=danos-netconf` on `ExecStart` (the socket is chowned after
bind); `--socket-mode` can override the 0660 mode, but do not make the socket
world-writable. Any local process that can connect is still authenticated by
uid and mapped through the role policy.

Restart `sshd` after the change. A client connects with the subsystem name:

```
ssh -s netconf user@router
```

Use key-only authentication. The subsystem never sees the password or key; it
trusts the `LOGNAME`/`USER` value set by `sshd`. The requested
`DANOS_NETCONF_ROLE` is forwarded only as audit context and is ignored for
authorization.

## Authorization and audit

`sshd` authenticates the user. The subsystem forwards the user name and the
requested role as audit context. The daemon ignores the requested role and
resolves the effective role from the peer uid through the policy above:

- `admin` and `operator` may run `edit-config`.
- `viewer` may run `get` and `get-config`.

If the requested role differs from the resolved role, the daemon logs the
mismatch and uses the policy result. Every RPC decision is evaluated by
`danos_authz_check_identity`, so permitted and denied operations both reach the
audit log with the SSH user attached.

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
| running | object store + checksummed `<startup>.running` snapshot, DPA transactions | `edit-config`, `commit`, `copy-config` |
| candidate | snapshot of running, created on first use | `edit-config`, `copy-config`, `discard-changes` |
| pending confirmed | checksummed file `<startup>.confirmed`, rolled back at next startup | `commit` (arm), confirm/cancel/timeout (remove) |

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

The committed running configuration is durable. Every successful plain
commit, confirmed-commit publish, `copy-config` to running, direct
`edit-config` to running, and rollback writes a checksummed running snapshot
(`<startup>.running`, or `DANOS_NETCONF_RUNNING`). A restart loads it before
accepting sessions; a write failure fails the commit instead of acknowledging
state that would not survive a restart.

A corrupt or unreadable running snapshot is **fail-closed**: the daemon refuses
to start rather than silently serving an empty configuration. Operators must
repair/remove the file, or set
`DANOS_NETCONF_ALLOW_CORRUPT_RUNNING=1` to explicitly start empty (the next
successful commit replaces the damaged file). A missing file is an empty
running configuration, not an error.

The pending commit is durable too. When a confirmed commit is armed, the daemon
writes a checksummed file containing the rollback baseline, deadline, session
and persist token next to the startup datastore (`<startup>.confirmed`, or
`DANOS_NETCONF_CONFIRMED`). The file is written with a temporary file, fsync
and rename; a confirming or cancelling commit removes it. If the daemon (or
the whole device) restarts with the file present, `netconf_ds_init()` restores
the baseline before any session can connect. RFC 6241 8.4 therefore holds:
an unconfirmed commit is rolled back across reboot, and a persist token only
survives session close, not reboot. A failed state write fails the commit and
restores the baseline instead of reporting an armed commit that is not
durable.

Candidate locks follow RFC 6241 7.5 for both plain and confirmed commits; a
cross-session `persist-id` confirmation skips the lock check only when the
candidate is empty (there is no queued edit to publish).

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

The cross-uid check (root) starts a daemon with a role map that grants `root`
admin and `nobody` viewer, then confirms that a `nobody` session claiming
`admin` is denied a mutating RPC. The daemon test itself skips the check when
run unprivileged. The script generates a throwaway host key, client key,
`authorized_keys` and
`sshd_config`, starts `sshd` on loopback port 2222, and runs
`danos-test/integration/netconf_ssh_client.py` for base 1.0 and base 1.1. The
Interop Gate workflow runs the same script in the `netconf-ssh` job.

## Known limits

- Candidate state and locks are shared between SSH sessions while
  `danos-netconfd` runs. The committed running configuration and a pending
  confirmed-commit rollback baseline are durable across restart/reboot in
  checksummed files next to the startup datastore. A damaged running file
  refuses startup unless `DANOS_NETCONF_ALLOW_CORRUPT_RUNNING=1` is set. This
  is the NETCONF subsystem's own snapshot; mgrd's WAL remains the system
  control-plane WAL.
- The daemon socket is a local trust boundary. Run it in a root-owned
  directory and do not expose it to unprivileged local writers. Roles come
  from the uid policy, never from the client preamble.
- A silent client is bounded by a 5 s preamble timeout and a 10 s hello
  timeout, with a 128-connection cap. After the authenticated client hello,
  an idle session is closed after the configured idle timeout (default
  300 s; see below). A second daemon is refused by a lock file, and the
  socket is unlinked only when its inode still belongs to that instance.
- `copy-config` does not accept an inline `<config>` source or `<url>`.
- `edit-config` covers interface `mtu` and `enabled`. Other subtrees are not
  modeled by the server yet.
- The subsystem trusts `sshd` for identity; it does not re-verify the peer.

## Idle sessions

After the client `<hello>` completes, a session with no further bytes for the
idle timeout is closed. The default is **300 seconds**. Configure it with:

- `danos-netconfd --idle-timeout <seconds>`, or
- `DANOS_NETCONF_IDLE_TIMEOUT=<seconds>` (also honored by the in-process
  subsystem fallback; the CLI takes precedence).

`0` disables the timeout; the accepted maximum is 604800 s (7 days). The
timeout is armed only after authentication/hello, never for the pre-login
preamble or hello read. Closing an idle session follows the same path as any
transport disconnect: datastore locks are released and a non-persisted
confirmed commit owned by that session is rolled back. A confirmed commit
armed with a persist token survives the session close and can still be
confirmed or cancelled by another authenticated session.
- Replies are built in memory, so a very large `get-config` is bounded by the
  8 MiB message limit rather than streamed.
