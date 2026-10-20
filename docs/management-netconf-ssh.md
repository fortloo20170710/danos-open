# NETCONF over SSH

DANOS-Open serves NETCONF (RFC 6241) through the OpenSSH server. OpenSSH owns
the transport, encryption and user authentication; DANOS provides a NETCONF
subsystem process that speaks RFC 6242 framing on its stdin/stdout.

## Components

| Piece | Role |
|-------|------|
| `sshd` | SSH transport, host keys, user authentication |
| `danos-netconf-subsystem` | Per-session process: `<hello>`, framing, RPC dispatch |
| `danos-mgmt` (`netconf.c`) | Message layer: `get`, `get-config`, `edit-config` |
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
| `urn:danos:yang:1.0` | supported |

Supported RPCs: `get`, `get-config` (running), `edit-config` (running,
interfaces subtree), `close-session`.

`commit`, `discard-changes`, `lock`, `unlock` and `kill-session` return an
`rpc-error` with `operation-not-supported`. The server advertises no candidate
datastore, no rollback-on-error and no validate capability, so clients do not
negotiate features the server lacks.

`edit-config` applies every `<interface>` block in the request as one
transaction. Interface names and other text in replies are XML-escaped.

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

- One running datastore; candidate and startup are not implemented.
- No RPC-level locking.
- `edit-config` covers interface `mtu` and `enabled`. Other subtrees are not
  modeled by the server yet.
- The subsystem trusts `sshd` for identity; it does not re-verify the peer.
- Replies are built in memory, so a very large `get-config` is bounded by the
  8 MiB message limit rather than streamed.
