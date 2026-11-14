# Security Policy

## Reporting a vulnerability

DANOS-Open is a network operating system control plane. Report
suspected vulnerabilities **privately** via GitHub
"Report a vulnerability" (Security Advisories) on this repository —
do not open public issues for exploitable findings.

Include: affected component (e.g. gnmi_grpc, vpp_netlink, mgrd),
reproduction steps or PoC, and the release/tag affected.

## Scope notes

- mgrd requires configured mTLS on TCP by default; see
  [management-mtls](docs/management-mtls.md). `DANOS_ALLOW_INSECURE=1`
  explicitly permits plaintext only for isolated laboratory runs. Invalid TLS
  configuration must not fall back to plaintext. Standalone test servers are
  laboratory fixtures, not deployment defaults.
- mgrd requires elevated privileges for kernel programming; run it
  in a container or dedicated namespace.

## Supported versions

Latest tagged release only (see git tags).
