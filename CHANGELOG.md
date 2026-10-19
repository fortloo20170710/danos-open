# Changelog

## v0.16.0 — Unreleased

The final tag is withheld until the acceptance matrix and release preconditions
are satisfied. These entries summarize the planned release, not a declaration
of hardware qualification. See [draft release notes](docs/release/RELEASE_NOTES_v0.16.0.md)
and [release preconditions](docs/release/v0.16.0-preconditions.md).

### Added

- Native gNMI mTLS with server certificates, verified client certificates,
  explicit fingerprint-to-role mapping, and authenticated RBAC/audit.
- Unix peer-credential gNMI transport with root/admin and non-root/viewer roles.
- Connection-owned gNMI STREAM subscriptions, incremental HTTP/2/gRPC assembly,
  SAMPLE intervals, redundant-update suppression and heartbeat support.
- Virtual QEMU e1000 and VMware VMXNET3 evidence validation, baseline reporting,
  explicit frame-size metadata for new QEMU soak runs, and VM CPU capture.
- Real PCI preflight checks that reject virtual or ambiguous NIC identities.

### Fixed

- Transaction candidate isolation, atomic staged publication, abort guarantees,
  conflict handling and committed-WAL recovery; caller cleanup on errors.
- Bootstrap transaction error handling and seed/recovery coverage.
- Per-subscription change detection, concurrent streaming/unary requests,
  partial-frame handling and cancellation/reconnect behavior.
- Full certificate identity attribution in authorization and Set audit records.
- Release smoke checks now fail when required gnmic verification is unavailable,
  rather than reporting ALL PASS after skipped interop and state-readback checks.
  Build-local client discovery preserves explicit selection and exports it to
  the interop subprocess; required TLS interop tools may not silently skip.

### Qualification limits

- At the 2026-10-19 release-preparation checkpoint `affb5e1`, local CTest and
  `release_check.sh` pass: gnmic I1–I5/I9 including the TLS front end, daemon
  seed/readback, 131 metrics, forced crash and 3-record WAL replay/readback.
  Exact-checkpoint GitHub CI (`39250438281`), Coverage (`39250438228`) and
  Interop (`39250438748`) also pass.
- Archived QEMU traffic evidence was identity-checked and re-recorded; it is
  functional evidence, not a new traffic run for the release commit. VMware
  historical results likewise do not replace a fresh run on the rebuilt ISO.
- PCI sustained PPS/throughput, timestamped latency, aligned CPU and complete
  ECMP distribution qualification remain open. New virtual throughput/CPU
  instrumentation still needs fresh traffic runs.
- Physical P3 management reachability was verified on R1–R4, but the host test
  route is absent and the UART is occupied; no physical data-plane test was
  run in this checkpoint. Uniform physical rollout, production PKI and live
  FRR/VPP recovery remain open. BFD/VLAN/VXLAN/EVPN/OVS/P4 remain deferred.

## v0.16.0-rc1

Historical candidate notes are retained in
[RELEASE_NOTES_v0.16.0-rc1.md](docs/release/RELEASE_NOTES_v0.16.0-rc1.md).
