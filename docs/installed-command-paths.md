# Installed command paths

The runtime package installs every executable command into `/usr/bin`, which is
on the default `PATH` for root and non-root login shells:

| Component | Installed command | Package path |
|---|---|---|
| Control-plane daemon | `danos-mgrd` | `/usr/bin/danos-mgrd` |
| Shared NETCONF daemon | `danos-netconfd` | `/usr/bin/danos-netconfd` |
| OpenSSH NETCONF subsystem | `danos-netconf-subsystem` | `/usr/bin/danos-netconf-subsystem` |

`danos-fib` is a library component (ZAPI parser/mapper); the repository does not
build or install a standalone `danos-fib` executable. The management CLI is a
library used by the daemons; there is no installed `danos-cli` executable. These
two names therefore have no PATH entry to fix.

The NETCONF binaries previously landed in `/usr/sbin`, which non-root login
shells normally exclude from `PATH`. CMake now installs them with
`CMAKE_INSTALL_BINDIR`, the systemd unit starts `/usr/bin/danos-netconfd`, and
`debian/danos-open.install` matches the new paths.

## Verification

`danos-test/installer/verify_command_paths.sh` checks a staged or installed root:

```sh
DESTDIR=/tmp/stage cmake --install build --prefix /usr
danos-test/installer/verify_command_paths.sh /tmp/stage
```

It uses `PATH=/usr/bin:/bin` and requires `command -v` to resolve all three
commands. A freshly installed guest must additionally be checked after login and
reboot; the Debian cloud image used for the local attempt did not consume the
NoCloud seed (`cloud-init` ran only the local stage), so the guest-level evidence
is still outstanding. The staged package layout and `/usr/bin` resolution pass.
