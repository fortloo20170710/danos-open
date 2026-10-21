# NETCONF over SSH

DANOS-Open serves NETCONF (RFC 6241) through the OpenSSH server. OpenSSH owns
the transport, encryption and user authentication; DANOS provides a NETCONF
subsystem process that speaks RFC 6242 framing on its stdin/stdout.

## Components

| Piece | Role |
|-------|------|
| `sshd` | SSH transport, host keys, user authentication |
| `danos-netconf-subsystem` | Per-session process: `<hello>`, framing, RPC dispatch |
| `danos-mgmt` (`netconf.c`) | Message layer: datastore selection and RPC handling |
| `danos-mgmt` (`netconf_datastore.c`) | Running/candidate/startup state, locks, startup file |
| `danos-mgmt` (`netconf_framing.c`) | RFC 6242 framing: EOM and chunked |

OpenSSH starts one subsystem process per SSH session. The process reads RPCs
from stdin, writes replies to stdout and exits after `close-session`.

## Server configuration

Install the subsystem (default `${CMAKE_INSTALL_SBINDIR}/danos-netconf-subsystem`)
and add it to `sshd_config`:

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

Subsystem netconf /usr/local/sbin/danos-netconf-subsystem
```

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

The timer runs in the subsystem process, so it bounds a session's confirmed
commit while that process lives. Although the current in-process handler has
tests for persist tokens, the SSH deployment starts a separate process for
each session: a persisted commit cannot be confirmed/cancelled from a later
SSH session, and neither its timer nor rollback baseline survives process
exit/reboot. The advertised capability therefore remains `confirmed-commit:1.0`
and **confirmed commit is not qualified for production SSH use**. Do not rely
on `<persist>` or `<persist-id>` until the shared NETCONF daemon and durable
rollback state are implemented. RFC 6241 requires persistent tokens to work
across sessions and requires rollback after a reboot before timeout.

Locks follow RFC 6241: a lock belongs to one session, blocks writes to that
datastore by other sessions (`in-use`), refuses a second lock
(`lock-denied`), and is released by the owner's `unlock` or `close-session`.

`edit-config` still covers only the interface `mtu` and `enabled` leaves, for
interfaces that already exist in the selected datastore.

### Process model

OpenSSH starts one subsystem process per SSH session. The running, candidate
and startup datastores and the lock table therefore live inside that process:
two concurrent SSH sessions do not see each other's candidate edits or locks,
and `commit` updates only the committing session's in-memory running store.
The startup file is shared, so `copy-config` to startup is visible to later
sessions. A single NETCONF daemon that hosts all sessions would make candidate,
locks and confirmed-commit state global; the datastore module is written for
that host and does not depend on the transport.

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
  build/danos-mgmt/danos-netconf-subsystem
```

The script generates a throwaway host key, client key, `authorized_keys` and
`sshd_config`, starts `sshd` on loopback port 2222, and runs
`danos-test/integration/netconf_ssh_client.py` for base 1.0 and base 1.1. The
Interop Gate workflow runs the same script in the `netconf-ssh` job.

## Known limits

- Candidate state, locks and pending confirmed-commit state are process-local
  because the SSH subsystem runs one process per session (see "Process
  model"). Confirmed commit is not release-qualified over SSH; persistent
  cross-session tokens and rollback recovery across process restart remain
  open.
- `copy-config` does not accept an inline `<config>` source or `<url>`.
- `edit-config` covers interface `mtu` and `enabled`. Other subtrees are not
  modeled by the server yet.
- The subsystem trusts `sshd` for identity; it does not re-verify the peer.
- Replies are built in memory, so a very large `get-config` is bounded by the
  8 MiB message limit rather than streamed.
