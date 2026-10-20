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
  SAMPLE intervals, redundant-update suppression, heartbeat support and
  ON_CHANGE delivery over the path-scoped change detector.
- Virtual QEMU e1000 and VMware VMXNET3 evidence validation, baseline reporting,
  explicit frame-size metadata for new QEMU soak runs, and VM CPU capture.
- Real PCI preflight checks that reject virtual or ambiguous NIC identities.
- libFuzzer targets for gNMI protobuf decoding, HPACK, HTTP/2 frame headers and
  the mTLS role map (with a seed corpus), wired into the Interop workflow.
- A documented per-connection resource budget for the gNMI listener
  (`docs/management-resource-limits.md`), covering admission, frame, queue and
  message limits.
- A line-coverage non-regression gate (`--fail-under-line`) enforced by the
  Coverage workflow at 79%, with a refreshed 2026-10-20 baseline.
- A root `Dockerfile` that builds and tests the tree from source, then ships a
  minimal `danos-mgrd` runtime image with a metrics health check.
- Pinned GitHub Actions commit SHAs and a checked-in `tools/gnmic` Go module so
  CI verifies `go.sum` instead of resolving dependencies at run time.

### Fixed

- WAL replay now distinguishes a torn tail from corruption and reports callback
  failures; checkpoints write and fsync a snapshot before rename; `danos-mgrd`
  refuses startup on corrupt recovery unless an operator override is set.
- Audit records escape quotes and control characters and the audit file is
  created owner-only (`0600`).
- The metrics server caps concurrent connections, applies read/write timeouts
  and binds to loopback by default. Metric registries are lock-protected.
- Route programming is idempotent through `NLM_F_REPLACE`, IPv6 address
  operations fail explicitly as unsupported, and netlink ACKs are validated.
- The VPP binary-API reader sizes its window from the frame length prefix.
- The VPP binary-API context, message table and stat table are serialised by a
  recursive mutex, so reconnect, transaction and diagnostic paths can run from
  several daemon threads safely.
- HTTP/2 sends honour the peer's `SETTINGS_MAX_FRAME_SIZE`; a full parked-frame
  queue closes the connection with GOAWAY `ENHANCE_YOUR_CALM` instead of
  dropping frames; and a writer blocked on flow control no longer re-parks its
  own frames and spins without reading the socket.
- Bearer-token authentication requires an explicit role
  (`DANOS_AUTHZ_TOKEN_ROLE`, or `DANOS_AUTHZ_DEFAULT_ROLE` as fallback);
  configuring a token without one fails startup, and an unknown default role
  falls back to viewer instead of admin.
- gNMI protobuf decoding supports lengths beyond 16 KiB, and HTTP/2 frame
  header parsing is shared with the fuzz target.
- `danos_tx_rollback` rejects states where it cannot undo programmed state and
  the DPA contract documents post-commit rollback limits.
- Backend sweep, retry-slot lookup and anti-flap parking are locked and
  permanent parking survives sweeps.
- The systemd unit applies capability bounding, filesystem and syscall
  sandboxing.
- The legacy REST gNMI server is linked only into tests, keeping it out of the
  daemon and the fuzz surface.
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
