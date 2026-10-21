# Outstanding work plan

### 2026-10-21 NETCONF consolidation and virtual-evidence preflight

- Consolidated and pushed the NETCONF `validate` / confirmed-commit
  prototype (`8a58aa7`) and ephemeral gNMI test-port fix (`3228741`). The
  latter's GitHub CI, Coverage and Interop Gate workflows all completed
  successfully. NETCONF confirmed-commit remains a prototype: session-local
  state means cross-session/reboot persistence and rollback are not yet
  production-qualified.
- Added a virtual-lane preflight so a cleaned/missing QEMU archive is recorded
  as structured `ENVIRONMENT-OPEN`, like missing VMware evidence, instead of
  being misreported as a product/test `FAIL`. Malformed or failing evidence
  that is actually present still goes through the strict result verifier.
- Added a regression test that exercises both missing lanes and verifies the
  aggregate JSON preserves `ENVIRONMENT-OPEN` and never claims physical
  performance acceptance. Release build and CTest passed 84/84 after one
  transient `gnmi_grpc` SIGPIPE; the isolated test and full rerun both passed.
  The deterministic backend contract passed all three stages (DPA 11/11,
  programming pipeline, composite Route/NH/NHGroup).
- Current workspace no longer contains the archived QEMU topology or ISO
  artifacts, and VMware `vmmon`/`vmnet` plus a fresh paired guest run remain
  unavailable. Revalidation therefore correctly stays `ENVIRONMENT-OPEN`;
  physical PCI performance and live R2/R3 recovery remain open as well.

### 2026-10-20 hardening, coverage and repository hygiene

- Applied the P0–P2 review findings in grouped commits: WAL replay health and
  checkpoint snapshots, fail-closed recovery startup, audit escaping and file
  mode, metrics-server limits, systemd sandboxing, netlink idempotence, VPP
  frame-window sizing, backend sweep/retry locking, multi-byte protobuf
  lengths, honest rollback semantics, legacy REST gNMI isolation and HTTP/2
  frame-header extraction.
- Added libFuzzer targets for gNMI protobuf, HPACK and HTTP/2 frame headers;
  the Interop workflow builds and smoke-runs them. Local GCC syntax checks
  pass; clang is still unavailable in this workspace.
- Fresh coverage run on the same tree: lines 7028/8756 (80.3%), functions
  611/690 (88.6%), branches 4117/7003 (58.8%), CTest 81/81. The Coverage
  workflow now enforces a 79% line floor through
  `run_coverage.py --fail-under-line`; both the pass and fail paths were
  exercised locally.
- The root `Dockerfile` was repaired and verified end to end: its builder stage
  passes CTest 81/81 and the runtime image answers `/metrics`; two container
  gaps (missing `openssl` CLI and missing `git`/`.git` for the installer
  payload test) were fixed.
- GitHub Actions are pinned by commit SHA, gnmic is built from the checked-in
  `tools/gnmic` module with `go.sum`, and local build directories are ignored.
  The testpmd draft is now tracked under `tools/perf-testpmd/` with an explicit
  not-acceptance-ready note; its technical gaps below remain open.
- Verification on this tree: clean Release build 81/81 CTest, ASan/UBSan build
  81/81 CTest, and the source-image builder 81/81.
- Follow-up hardening (2026-10-20, second pass): the HTTP/2 writer now honours
  the peer's `SETTINGS_MAX_FRAME_SIZE`, fails the connection with GOAWAY
  `ENHANCE_YOUR_CALM` when the parked-frame queue fills, and reads the wire
  directly while blocked so a parked frame cannot starve WINDOW_UPDATE
  processing. Regression tests cover the frame-size ceiling and queue
  overflow. The VPP API context is protected by a recursive mutex and has a
  multi-threaded mock test (TSan-clean). Bearer tokens now require an explicit
  role and fail closed without one. A TLS role-map fuzz target and corpus were
  added, and the gNMI per-connection resource budget is documented. Dated
  acceptance and plan evidence was moved to `docs/archive/`; the active pages
  now carry current state only. Local CTest 81/81 passes.
- gNMI ON_CHANGE (2026-10-20, third pass): STREAM subscriptions may now select
  ON_CHANGE per subscription. The decoder records the per-path mode, rejects
  ON_CHANGE outside STREAM and rejects `sample_interval`,
  `suppress_redundant` and `heartbeat_interval` on ON_CHANGE paths. Delivery
  reuses the existing path-scoped change detector, which the connection
  services once per 100 ms poll cycle; the initial snapshot/sync contract is
  unchanged. Tests cover the wire decode, option rejection and an end-to-end
  mutation on an `updates_only` ON_CHANGE stream.
- Per-object deltas (2026-10-20, fourth pass): target-defined and ON_CHANGE
  STREAM notifications now carry only objects that appeared, changed or were
  removed since the previous notification, using concrete keyed paths
  (`interfaces/interface[name=...]`) and `Notification.delete` for removals.
  Full notifications use the same concrete paths instead of repeating the
  wildcard subscription path. Baselines hold type, id, content hash, key and
  VRF per object and are capped at 4,096 per subscription; SAMPLE/heartbeat
  delivery is unchanged. Tests assert one update per change, an added object
  alone, and a delete on removal. A GOAWAY-then-close race is also fixed: the
  server drains unread inbound data before closing so an RST cannot discard
  the GOAWAY.
- OPER readback (2026-10-20, fifth pass): `danos_backend_ops_t` has an
  optional `readback` hook. The programming pipeline maintains a
  process-wide OPER snapshot, refreshed on every pass (including objects
  already in sync) and cleared on failure, withdrawal, backend restart and
  explicit reset. The netlink adapter reads one link through `RTM_GETLINK`
  (`IFLA_OPERSTATE`, `IFLA_MTU`; `IFF_RUNNING` fallback) and the mock mirrors
  that. gNMI `/interfaces/.../state` reads and the CLI OPER column use the
  OPER copy and fall back to the desired object when the backend has no
  reading. VPP has no interface-dump binding, so it contributes no OPER copy.
  Tests cover carrier-only drift with an unchanged desired object, the absent
  route readback, reset/rebuild, and gNMI leaf and state-entry reads.

### 2026-10-19 pushed verification and next physical/VMware gates

- R2 reboot management recheck passed 5/5 host pings to `192.168.1.244` with
  the expected P3 MAC. The source-generated physical profiles had a safety
  defect: they shut down P3 as an unused port. The renderer now explicitly
  keeps management P3 up and excludes it from shutdown for all four roles;
  seven focused profile tests and full CTest 81/81 pass. This does not rewrite
  existing on-box profiles or qualify transit. Before updating a deployed
  profile, inspect its hash and use an identity-checked serial session.

- `1c93ab5` is pushed to `origin/main`; CI `39249186784`, Coverage
  `39249186827` and Interop Gate `39249186792` all pass. Local CTest is
  81/81 and the deterministic backend contract gate passes DPA 11/11 plus
  programming-pipeline and composite Route/NH/NHGroup lifecycle checks.
- QEMU evidence is split by identity: the strict accepted long-soak topology
  passes 4,000/4,000 with buckets 3,000/1,000; the newer topology passes
  FRR/VPP lifecycle and an 800-probe failover window but is not a 1,000-per-
  flow soak. The standardized QEMU result leaves PCI performance OPEN and
  leaves frame size/Mbps/CPU blank. See the acceptance matrix for hashes.
- Physical prerequisites are not satisfied: host route lookup for
  `30.30.30.2` selects Wi-Fi, not R2 P3; do not send test traffic until the
  temporary route is restored and verified. R2 P3 itself and R1/R3/R4 P3
  management addresses are reachable. The UART is still held by another
  process; coordinate its release before collecting router counters or
  changing the R2 P1/R3 P4 path. Do not touch that session.
- VMware execution still requires authorized loading of the kernel-matched
  `vmmon` and `vmnet` modules. The clean DANOS/peer ISO pair is built, but no
  VM runtime or fresh serial evidence exists.
- Next sequence: restore/verify the host test route; when UART is free,
  inspect R2 P1 and R3 P4 identity/carrier/ARP, then run the bounded physical
  traffic window and FRR route add/withdraw/recovery checks; separately load
  VMware modules and run the strict VMXNET3 pair verifier. Only a properly
  correlated independent-generator/receiver measurement can close PCI
  performance. The testpmd draft is now tracked under
  `tools/perf-testpmd/`; keep it out of acceptance until
  its VPP transit attribution, counters, elapsed-time PPS and CPU sampling
  are corrected and tested.

### 2026-10-19 VMware runner requalification

- Rebuilt the polling-only DANOS VMXNET3 image and a matching Linux peer LIVE
  image from clean pushed commit `963aea4`. The DANOS profile embeds both
  VMXNET3 BDFs, independent `/24` networks, VPP 26.10 and polling-only mode;
  the peer profile sends finite 1,000-packet probes on both networks. Hashes
  are in `docs/v0.16-acceptance-matrix.md`.
- Do not reuse the October 2 serial logs for these images: their boot commit
  and ISO identity differ. A fresh VMware run is not yet possible because
  `vmmon` and `vmnet` are not loaded and the current sudoers policy rejects
  module insertion. The module files' vermagic matches the running kernel.
  After authorized module load, boot the isolated pair, retain new serial
  logs, and run the strict VMXNET3 verifier; keep the historical 2,000-packet
  result separate.
- This does not close sustained PCI/DPDK performance or the physical R2
  P1↔R3 P4/zserv recovery gap.

### 2026-10-19 QEMU recovery follow-up

- Clean-source QEMU image `2bc6842` now passes the full two-FRR/VPP topology
  gate, including BGP add/withdraw, OSPF, dual-path ECMP, next-hop withdraw/
  restore, VPP restart/replay and FRR/zserv restart with zebra re-registration
  and positive backend replay. Failover received 785/800 probes (1.88%, under
  the explicit 15% window budget); this remains functional evidence, not a
  throughput qualification. Exact identities and hashes are in
  `docs/v0.16-acceptance-matrix.md`.
- The first gate attempt raced FRR's service PASS marker against the later
  zebra reconnect. `verify_frr_vpp_topology.sh` now waits up to 90 seconds by
  default for the strict recovery evidence validator, then emits its final
  diagnostic if the evidence never arrives. The full gate passes on the
  captured run; CTest passes 81/81 with the new gate regression check.
- Still open: physical R2 P1/P4 route/neighbor recovery and FRR/DPA integration,
  VMware replay with its clean rc1 artifacts, and accepted real-PCI sustained
  performance. Do not promote QEMU functional results into those lanes.

### 2026-10-19 current execution checkpoint

- Verification source checkpoint is `bc55bee`; this status synchronization is
  committed on the same main line. The verification details below refer to
  that source checkpoint, not to the documentation-only follow-up commit.
  Parallel testpmd/release files remain uncommitted in the shared worktree and
  are excluded from this checkpoint and release evidence. Fresh `build-test`
  CTest passed 81/81 and the three-stage deterministic backend contract gate
  passed (DPA conformance 11/11, programming pipeline, composite
  Route/NH/NHGroup lifecycle).
- GitHub CI, Coverage and Interop Gate all passed on `bc55bee` (runs
  `39242721964`, `39242721925`, `39242721951`), including x86_64/aarch64,
  DPA conformance, hosted VPP V1–V5, gnmic I1–I9, K4/K5 and sanitizer/fuzz
  jobs. These hosted checks do not close the physical performance or unified
  release-gate items below.
- R2 P3 (`GigabitEthernet3/0/0`, MAC `00:1f:7a:69:f5:ee`) was restored to
  `192.168.1.244/24` after reboot by starting the inactive VPP service and
  applying the runtime CLI config. On a later boot, VPP was active and the
  address was present before commands from this session; host ping passed
  5/5. Earlier commands by other actors were not audited, so automatic
  persistence/replay is unproven; this is not transit/performance evidence.
- The same R2 post-boot audit found its P1 interface/FIB route present, but no
  ARP neighbor at `10.10.0.1`; two VPP probe windows returned 0/5 each, with
  P1 drop +10. A warm-neighbor R2 P3→R3 P3 VPP check passed 5/5, isolating the
  remaining failure to the P1/P4 path. Next recovery check is R3 P4
  admin/link/address and the R2.P1↔R3.P4 cable; no config/cabling was changed.
  Until then, do not count the post-reboot inter-router path as recovered.
- R2 FRR 10.3 and `/run/frr/zserv.api` are active, but its route table is empty
  and bgpd/ospfd/mgrd are inactive; the VPP static lab route is not dynamic
  FRR evidence. Full FRR→DPA→VPP restart replay remains open and needs a
  controlled topology/daemon configuration after R3 P4 reachability is fixed.
- Highest-priority remaining lane is a trustworthy real-PCI measurement. The
  shared-worktree testpmd draft is not acceptance-ready: it does not establish
  VPP transit or ECMP attribution, parses a nonstandard stats format, infers
  PPS from configured sleep, and hard-codes CPU; it also passes the command
  file incorrectly instead of using DPDK's `--cmdline-file`, and quotes a
  10x-inflated 1 Gbps minimum-frame rate. Keep performance OPEN until source
  TX, DUT ingress/egress, independent receiver, monotonic duration, CPU
  samples, errors/loss, and artifact identity are correlated. See the official
  [DPDK 24.07 application guide](https://doc.dpdk.org/guides-24.07/testpmd_app_ug/run_app.html).
- The virtual evidence wrapper now emits a schema-valid `ENVIRONMENT-OPEN`
  result with null VMware metrics when its original ISO or serial evidence is
  missing, preserves the legacy VMware PASS artifact, and fails closed for an
  invalid present artifact. The 2026-10-19 default-path replay validated the
  archived QEMU 4,000/4,000 functional result but could not revalidate VMware
  because the rc1 ISO is absent. Focused report tests passed and CTest passed
  81/81. See `docs/v0.16-performance-baselines.md` for hashes. This improves
  evidence classification only; VMware, PCI performance, and release gates
  remain open.
- Next safe implementation step: replace the draft methodology with a
  verifier-backed, independently observed measurement flow, without stopping
  VPP or changing physical cabling until a coordinated maintenance window is
  explicitly underway. Then repeat uniform-r14/recovery and production-PKI
  gates; only after those and performance disposition revisit v0.16.0 release.

Derived from the v1.0 architecture review and the 2026-10-04 hardening pass
(`docs/security-and-correctness-hardening-2026-10-04.md`). Ordered by
dependency, not by severity: several items below are blocked on the two items
at the top.

Status legend: `[ ]` open · `[~]` in progress · `[x]` done · `[!]` blocked

### Current delivery and work inventory — 2026-10-15

Source checkpoint `af41ef6`: gNMI STREAM disconnect/reconnect regression;
CTest 78/78 PASS. GitHub CI, Coverage and Interop Gate all pass
(`38840678456`, `38840678472`, `38840678449`), including K4/K5, real VPP
V1–V5, gnmic I1–I9, sanitizers and concurrency. The current physical and
release evidence is summarized in `docs/v0.16-acceptance-matrix.md`. This is
still `v0.16.0-rc1`, not a final-release PASS.

Execution priority, as confirmed on 2026-10-15:

1. **Physical performance qualification (highest priority, environment-open).**
   Prepare a qualified traffic source and independent receiver, then measure
   sustained 64-byte PCI DPDK throughput, loss, latency, CPU and per-flow/ECMP
   bucket distribution. R3 preflight and bounded packet delivery are not
   performance qualification. R1–R4 and the host are user-reported on a
   five-port router via P3. Host `.243`↔R2 P3 `.244` passed bounded ping, and
   host `.243`↔R1 P3 `.245` and `.243`↔R3 P3 `.246` passed 5/5 in both
   directions; both port identities and 1 Gbps/full duplex carriers match.
   R4 P3 `.247` now also passes host ping 5/5 each way. R1/R3/R4 addresses
   `.245`–`.247` were applied at runtime; R2 `.244` has been observed after a
   later boot, but the source is untraced and automatic replay is unproven.
   Treat all four as persistence-unqualified. R2/R4 report r9 (R4 build commit `3f1691c`),
   while R1/R3 report r14. The host route to the test destinations currently
   selects R2 P3 `.244` on `enp4s0`; the bounded four-node 16,384-frame
   multi-flow run passed with independent receiver deltas and zero observed
   DUT/receiver drop deltas. This closes functional path-use only. Keep
   performance `ENVIRONMENT-OPEN` until a qualified sustained traffic source
   provides validated loss, latency, CPU and throughput measurements. Do not
   silently remove the operator-managed host route. In the same physical
   qualification track, roll R2/R4 to r14,
   complete identity-bound four-node checks, test live VPP reconnect/recovery
   with mgrd active, and deploy/verify production PKI/mTLS before enabling
   physical management.
2. **gNMI Subscribe semantics / user experience (in progress).** The committed
   implementation now honors STREAM `updates_only`, emits the required
   initial `sync_response`, applies JSON versus JSON_IETF TypedValue selection,
   and removes an unconditional sleep after event/timed waits. Protocol tests
   cover initial snapshot, updates-only and both encodings. STREAM consumes
   HTTP/2 RST_STREAM and only reads a frame after verifying the full frame is
   available, preventing partial-frame stalls. Its test cancels the
   subscription and proves a subsequent Capabilities RPC succeeds on the same
   connection. A new regression also closes a live STREAM connection and
   verifies a fresh connection can complete Capabilities, guarding against a
   stuck per-connection handler. CTest 78/78 and all three remote workflows
   pass at `af41ef6`.
   The blocking subscription loop has been replaced by connection-owned active
   subscription state. The connection enters incremental mode as soon as any
   request is in flight, not only after a subscription is active. It polls
   complete HTTP/2 frames without blocking on partial frames, handles resets,
   dispatches additional RPCs, and services subscription updates. Initial and
   subsequent RPC HEADERS/CONTINUATION and DATA are assembled incrementally
   in bounded per-stream state. Regressions leave both the first Subscribe
   request and a later stream's header block/gRPC prefix incomplete while a
   Capabilities RPC on another stream completes, then resume and verify sync.
   Two active
   STREAMs, cancellation and reconnect are covered. A single DATA frame may
   contain multiple gRPC messages: after dispatch, the server schedules any
   complete retained message without waiting for another peer frame. A POLL
   regression packs the initial SubscribeRequest and a Poll request into one
   DATA frame and verifies both notifications. The legacy blocking gRPC
   request-body reader has been removed. The first-request interleaving and
   all existing gNMI gRPC tests pass, including 20 consecutive targeted runs;
   full CTest is 78/78. RST_STREAM now also cancels a separate request with an
   incomplete gRPC prefix, after which the same connection remains usable.
   Broader reset/cancellation stress remains before treating the transport as
   fully hardened.
   Per-path SAMPLE is supported for homogeneous STREAM subscription lists:
   omitted/zero intervals select 1 s, intervals below the 100 ms service
   cadence are rounded up, and values above 24 h are rejected. Sampling sends
   the initial snapshot/sync and then periodic snapshots; store changes do not
   trigger early samples. `suppress_redundant` compares serialized update
   content excluding notification timestamps and skips unchanged periodic
   samples. A heartbeat emits an unchanged update on its configured deadline;
   heartbeat is valid only with suppress_redundant, and both options require
   SAMPLE. ON_CHANGE is accepted as a STREAM sub-mode and shares the
   path-scoped change detector; mixed per-path contracts and `use_models`
   remain explicitly rejected. End-to-end tests cover a changed value waiting
   for a 250 ms sample interval, an unchanged value emitted by a 300 ms
   heartbeat, and an ON_CHANGE subscription that notifies after a store
   mutation. Full CTest is 78/78 and targeted gNMI proto/gRPC tests pass 10
   consecutive runs. CI, Coverage and Interop Gate pass at `85dc16e`,
   including ASan/UBSan, TSan, gnmic I1-I9, real VPP V1-V5 and K4/K5.
   Per-object deltas (only changed leaves in a notification) remain the next
   subscription-semantic gap.
3. **OPER readback / control-plane-data-plane truth.** Interface readback is
   implemented: netlink `RTM_GETLINK` -> OPER snapshot -> gNMI/CLI, with the
   desired object as fallback. Still open: route and VRF readback (and drift
   repair driven by it), a VPP interface dump so VPP objects produce an OPER
   copy, and turning OPER differences into verification failures. The
   reentrant diff callback fix is done; the reconciler's diff callback still
   only counts differences and is not hardware readback.
4. **NETCONF SSH transport.** Done. OpenSSH provides transport and
   authentication; `danos-netconf-subsystem` implements RFC 6242 end-of-message
   and chunked framing, negotiates base 1.1, and maps the SSH user plus
   `DANOS_NETCONF_ROLE` into RBAC. Advertised capabilities match behavior
   (`kill-session` answers `operation-not-supported`), and `edit-config`
   applies every interface block atomically. Datastores include candidate and
   startup, and `validate` is implemented. Confirmed commit has an in-process
   timeout/session-close prototype only; persistent token confirmation across
   SSH sessions and restart rollback remain open because each subsystem is a
   separate process. Keep it out of release acceptance. See
   `docs/management-netconf-ssh.md`. The Interop workflow runs the subsystem
   over a loopback `sshd`.
5. **Supply-chain and repository hygiene.** Pin remaining dependencies and
   base images, add SBOM and artifact provenance/signing, and decide separately
   whether to rewrite history to remove the tracked large document.

After these priorities, re-evaluate HA/CoPP integration, mechanically
generated upstream fixtures and other architecture gaps against project scope;
they are not allowed to displace the ordered list above without a new decision.
Re-run the final release gate after applicable acceptance criteria pass, then
decide whether v0.16.0 can be released.

Deferred scope: BFD, VLAN, VXLAN and EVPN stay frozen until the v0.16 release
decision; OVS/P4/platform remain intentionally unstarted. Older notes below
are chronological evidence, not a competing current task list. In particular,
any old CTest counts or statements that callbacks are stubs are superseded by
this checkpoint and the 2026-10-13 project-status entry.

---

## P0 — blocks any real deployment

### [x] 1. Native gNMI mTLS transport and identity authorization

The daemon requires `DANOS_TLS_CERT`, `DANOS_TLS_KEY`,
`DANOS_TLS_CLIENT_CA` and `DANOS_TLS_ROLE_FILE` for production startup.
OpenSSL 3 checks the certificate chain, validity and client-auth purpose;
TLS 1.2/1.3 and HTTP/2 ALPN `h2` are supported. Each leaf-certificate
SHA256 fingerprint maps to an explicit admin/operator/viewer role. A valid
CA signature alone does not grant access, unknown identities are refused,
and bearer/default-role settings cannot elevate a certificate's role.
Get/Subscribe are reads; viewer Set is rejected with gRPC status 7.

Partial/invalid configuration is fatal and cannot fall back to plaintext.
Private keys must have private permissions and role files must not be
group/other writable. Existing bearer and Unix peer-credential APIs remain
for laboratory/local use; `DANOS_ALLOW_INSECURE=1` explicitly opts into
laboratory h2c only when no TLS settings are present. LIVE/kernel/smoke
scripts declare that choice rather than silently weakening production.

`gnmi_mtls` starts the actual daemon with temporary PKI and covers both
supported TLS versions, role-based Get/Set/Subscribe, certificate role
precedence, concurrent sessions, absent/expired/untrusted/wrong-purpose and
unmapped credentials, ALPN/hostname checks, fail-closed configuration,
laboratory opt-in and joined-worker shutdown. Acceptance also fixed the
audit ring and reconciler/metrics shutdown races exposed by TSan.

Deployment instructions and explicit limits: [management mTLS](management-mtls.md).
Local acceptance: Debug, ASan/UBSan/LSan and TSan each pass CTest 39/39.
This closes gNMI's transport/identity gap, **not all deployment security**:
metrics still require isolation/TLS proxy; online certificate rotation and
CRL/OCSP are not included. NETCONF transport is separate and covered by
[management NETCONF over SSH](management-netconf-ssh.md). Certificates/roles/CA
reload on restart.
No production certificate or private key is committed.

### [x] 2. Transaction engine provides staged atomicity

All public DPA CRUD operations now use the transaction passed by the caller.
Create/update/delete mutations remain private until commit; reads consult the
transaction overlay first (including route dumps), so read-your-writes works
without exposing candidate state to other transactions. `CREATE`, `UPDATE`,
and `DELETE` have distinct existence rules. Abort and failed conflict checks
discard the complete staged set.

Commit compares the captured base payloads while holding the object-store
write lock, preallocates replacement storage, writes the transaction records
and a WAL commit marker and fsyncs before publishing the in-memory batch. A
conflict or allocation/WAL failure leaves the object store unchanged. WAL
recovery applies only complete marked transactions; the legacy immediate
object-registry API remains available for internal recovery paths. Existing
callers that relied on `begin -> mutate -> commit` were migrated to the
documented prepare/validate/commit lifecycle rather than weakening the state
machine.

Coverage: private visibility, read-your-writes, create/update/delete abort,
same-key conflict, multi-object all-or-nothing conflict, route-dump overlay,
restart recovery, and an uncommitted transaction WAL record. Full CTest
acceptance is recorded in the current project status.

#### Findings from the two reverted attempts (historical)

Two attempts were made and both were reverted after the test count went red
(12/38 and 13/38). Nothing from either is in the tree. They established the
following, so a third attempt starts from here rather than from scratch:

- **The 51 CRUD entry points are uniform.** Every one is
  `(void)tx;` followed by a single
  `danos_object_create/update/delete(get_default_store(), TYPE, id, ...)`.
  Thin `crud_create/crud_update/crud_delete` wrappers plus a textual rewrite
  converts them; the `tx` parameter is already threaded through and only has
  to stop being discarded.
- **Staging requires a read overlay, not just a write overlay.** Reads are 12
  sites of the same shape. Without `crud_read` consulting the staged set, the
  very common `create` then `read` inside one transaction fails, because nothing
  is in the store until commit. This was the main cause of the failures.
- **Existence probing must pass a real `out_size`.** The store signals "it
  exists, and here is the size" by returning `DANOS_ERR_INVALID_ARG` when
  `out == NULL`; a NULL `out_size` is a plain argument error. Passing
  `NULL, NULL` silently makes every object look absent.
- **Do not collapse the three operations into a boolean.** A `bool remove`
  turns create and update into one upsert and loses the EXISTS / NOT_FOUND
  distinction that callers depend on. Model the operation as an enum
  (`CREATE` / `UPDATE` / `DELETE`) so each keeps its own existence rule, and so
  that a staged create whose payload allocation fails cannot be published as a
  delete.
- **A staged delete does not make the object present.** The overlay lookup has
  to report `remove` and the read path must treat it as absent.
- **`commit` must tolerate being called from `OPEN`.** Many call sites do
  `begin -> mutate -> commit` and today work precisely because writes bypass the
  transaction. Once they stage, a `commit` that refuses would leave the staged
  set unpublished and silently drop the caller's changes. Either run the
  skipped phases inside commit, or fix all 61 call sites; do not leave it
  strict and assume the callers were updated.
- **The read overlay must be consulted *before* the store.** The first attempt
  checked the store first and fell back to the overlay only when the object was
  absent - which means a staged *update* to an object that already exists reads
  back the stale stored value. That is the same invisibility staging exists to
  remove, so it has to be: "if this transaction staged something for this
  object, that is the truth for it".
- **Staging changes abort semantics, and that is what actually breaks the
  suite.** Today a delete applied the moment it was called, so a later abort
  did not undo it and cross-test state looked as callers expected. With
  staging, an aborted delete leaves the object in the store. Tests that create
  in one transaction and assert in another - or that abort and then expect the
  change to have happened - now see different state, which is the `EXISTS`
  failure the second attempt ended on.
- **So the real cost is not the 51 rewrites, it is the implicit contract.** The
  suite passes today partly because writes are immediate; staging makes that
  contract explicit and breaks every caller that leaned on it. Those callers
  have to be found and given the lifecycle they were implicitly assuming, and
  that inventory is the actual work - not the mechanical rewrite.

The successful implementation followed that sequence: pin abort and visibility
semantics first, migrate dependent callers, then enable staging and WAL commit
markers. The findings above remain as historical context for why the earlier
attempts failed.

Note the recorder fix from the earlier pass: transaction records are now
correctly retired, so this work no longer sits on top of a leak.

---

## P1 — correctness of the claims the project makes

### [x] 3. VPP 26.10 runtime and QEMU FRR/VPP functional lane

The real runtime gate runs against `danos-vpp-runtime-recover:local`
(`vpp v26.10-rc0~545-gad99177fe`). The [real control runtime gate](vpp-control-runtime-gate.md)
covers API registration, stats, default/VRF 777 FIB, add/repeat/NH
withdraw/restore/delete and table deletion. The clean-source QEMU FRR/VPP
topology then passed the full runtime verifier on one identity-bound boot:
actual packets on both paths, BGP and OSPF, ZAPI add/withdraw, route restore,
VPP and FRR/zserv restarts, and post-restart reachability. The 1000-packet-per-
flow ECMP soak and next-hop failover/restore also passed. Evidence and exact
digests are in the acceptance matrix.

This closes the VPP runtime/QEMU functional portion of item 3. It does not close
real PCI DPDK performance, line rate, the physical I211 runtime (the latest
passive serial reads were empty), or prove that QEMU PPS represents hardware.
Those remain distinct environment-dependent acceptance lanes and continue to
block the unwaived unified release gate.

Read-only [runtime capture tooling](vpp-runtime-capture.md) now collects
version/interface/FIB/load-balance/error/runtime snapshots, bounded commands,
raw output hashes, source identity and optional exact ISO/log attachments.
CAPTURED is not acceptance PASS. The legacy run_vpp_verify.sh no longer labels
kernel/mock tests or socket existence as real VPP/DPDK acceptance. The QEMU
FIB/table/NH and topology functional gates described above are closed for the
tested identity; physical PCI behavior remains open. API frame evidence is
item 11.

Historical physical I211 runs include a failed two-port traffic attempt; the
latest physical state is UNKNOWN because serial capture received no bytes.
Do not treat either as current hardware PASS or as a substitute for PCI
performance qualification.

### [x] 4. Capability registry must be wired, or deleted

`capability_registry.c` is 38 lines with no header, no callers, and an
unbounded `for (;;)` in its registration path. `danos_vpp_capability_register()`
is defined but never called, so the registry is empty at runtime.

Done: added `capability_registry.h` and `vpp_capability.h` so the entry points
are declared at all, and the VPP adapter now registers its capability table at
install time. The scan is bounded by `DANOS_MAX_BACKENDS` instead of relying on
`danos_backend_get_info()` eventually failing. A regression test asserts
interface/VRF/route are advertised and EVPN/MPLS/ACL/tunnel/multicast are not,
so the registry cannot be wired to a lying table - verified to fail both when
the register call is removed and when the table claims EVPN.

Still open: backend *selection* (picking among several registered backends by
capability and load, per ADR-0007) is not implemented - the registry answers
questions, nothing consults it to choose. And `type_skipped()` is still a
separate hand-maintained list rather than being derived from the table; the
regression test is what keeps the two in agreement.

### [~] 5. Reconciler must be driven by the daemon

Done so far: `danos_reconciler_run_once()` now invokes the tombstone sweep,
so deleting config actually withdraws it from the dataplane (previously only
reachable from tests). `max_retries`, `backoff_*` and `antiflap_*` are handed
to the programming pass and enforced through a bounded per-object retry table:
exponential backoff capped at `backoff_max_ms`, an attempt limit, and
anti-flap parking, so a route whose next hop has not been learned no longer
retries every 50 ms forever. `total_runs` is counted in both reconciler modes
(it was only counted when a state store existed, which the daemon does not
use), and `reconcile_trigger()` marks state dirty so the next pass runs
immediately. Two regression tests cover withdrawal and the retry limit.

Still open:
- desired/programmed key differences can now be snapshotted under the store
  lock and callbacks are invoked after unlock (commit `b7bff99`), including a
  reentrant-mutation regression. This does not yet establish that every drift
  is translated into per-type repair operations or verified against backend
  readback; those end-to-end behaviors remain open.
- OPER exists for interfaces (netlink readback), but the reconciler does not
  consume it for drift repair, and routes/VRFs have no readback binding, so
  `DANOS_ERR_VERIFY_FAIL` is still unreachable

### [!] 6. HA and CoPP are duplicated and disconnected

`danos-ha` (BFD, VRRP, supervisor — 630 LOC) and `danos-security` (CoPP policer,
RBAC, audit) are each built as standalone libraries with only their own test
linking them. Neither is in `danos-mgrd`'s link line. Blocked on item 1 for
the security half.

ADR-0006 delegates BFD to FRR's `bfdd` while `danos-ha/bfd.c` implements it
again — the duplication needs resolving one way or the other.

---

## P2 — protocol compliance gaps

### [~] 7. gNMI `Subscribe` and `Set` semantics

Done:
- POLL is now a real mode. Streams are tracked per stream id; the initial
  Subscribe sends the set and then the sync_response as **separate** messages
  (SubscribeResponse is a oneof, so the previous single combined message was
  invalid), and a Poll on a known stream answers with a notification only. A
  Poll before Subscribe is refused rather than treated as a new subscription.
  Previously every message on a POLL stream was handled as a fresh Subscribe,
  so each Poll re-sent the entire initial set.
- the STREAM change detector now hashes the raw object bytes (type and id
  folded in) streamed from the store. It hashed only interface mtu and
  ifindex, so a change to an interface name, admin state or address, or to a
  VRF name or a route prefix, metric or next hop, left the hash identical and
  subscribers were never told. It also copied each type into a fixed array
  (ifaces[64], vrfs[16], routes[32]), so a larger store silently lost objects.
- An in-progress follow-up hashes the serialized value selected by each
  subscription instead of comparing every STREAM to the global store hash.
  This suppresses cross-path wakeups (for example, a VRF mutation does not
  trigger an interfaces-only STREAM) while preserving relevant notifications.
  The path-scoped regression and full CTest pass 78/78; remote gates and commit
  are pending. ON_CHANGE now uses this detector for STREAM subscriptions, and
  change notifications carry only the objects that appeared, changed or were
  removed, identified by concrete keyed paths.

- Set now applies delete → replace → update in one staging transaction.
  Interface replacement resets omitted modeled fields, preserves identity,
  and route ECMP resize retires obsolete next hops. Late failure rolls back
  interfaces and complete route graphs together.
- Get returns concrete keyed object paths (route prefix plus VRF), correctly
  filters VRF entries, and no longer uses fixed collection-size caps.
  Supported forms and deliberate exclusions are documented in
  [Set/Get semantics](gnmi-set-get-semantics.md).

Still open:
- `use_models` is not decoded/applied. ON_CHANGE is supported for STREAM
  subscriptions over the path-scoped change detector; its SAMPLE-only options
  are rejected. Unsupported request options must be explicitly rejected rather
  than silently treated as event-driven STREAM.
- SAMPLE and heartbeat notifications still carry the complete subscribed set;
  only change-driven streams emit per-object deltas.
- Broader cancellation/reset and subscription lifecycle behavior need
  regression coverage beyond the new RST_STREAM/same-connection case. The
  current bounded 200 ms wait is a liveness fallback
  around store-event wakeups, not the former unconditional polling loop.
- BYTES/PROTO/ASCII encodings and scalar/decimal TypedValues remain outside
  the advertised encoding set; JSON and JSON_IETF are the supported forms.

### [x] 8. NETCONF has no transport

Done. `danos-netconf-subsystem` runs as an OpenSSH `Subsystem netconf` and
speaks RFC 6242 framing on stdio. Base 1.0 end-of-message and base 1.1 chunked
framing are both implemented, with an 8 MiB message and 4 MiB chunk bound; the
server sends its hello first and switches to chunked only when the client
advertises base 1.1. Session IDs are generated per connection instead of
hardcoded. The hello advertises base 1.0/1.1, writable-running, candidate,
startup and `urn:danos:yang:1.0`; `validate`, confirmed commit and
`kill-session` return `operation-not-supported`. `edit-config` applies all
`<interface>` blocks in one transaction. The SSH user is the authenticated
identity, mapped to RBAC by `DANOS_NETCONF_ROLE`.

The candidate and startup datastores followed: candidate edits are published
by `commit` in one DPA transaction, `discard-changes` drops them, `lock` and
`unlock` track per-session ownership, `copy-config` moves data among the three
datastores, and startup is a checksummed, atomically replaced file. `validate`
checks a named datastore or an inline config, and confirmed commit rolls back
on timeout, `cancel-commit`, or session close unless a persist token keeps it
pending. Candidate state, locks and pending confirmed commits are
process-local because each SSH session is its own subsystem process; hosting
NETCONF in one daemon remains open.

Remaining scope for `edit-config` is the modeled subtree: it covers only the
interface `mtu`/`enabled` leaves, on interfaces that already exist.

### [x] 9. FRR nexthop types and protocol mapping

Done, against upstream FRR 10.3 rather than by inspection:

- nexthop types now follow `enum nexthop_types_t` (lib/nexthop.h): 1 IFINDEX,
  2 IPV4, 3 IPV4_IFINDEX, 4 IPV6, 5 IPV6_IFINDEX, 6 BLACKHOLE.
  `zserv_encode_nexthop()` writes the trailing ifindex for *both* IPv4 forms
  and both IPv6 forms; the decoder read it only for type 3, so every type-2
  nexthop - the common case for an IPv4 route with a gateway - left four
  bytes unread and desynchronised every nexthop after it. Types 4-6 were
  skipped without consuming their payload. An unknown type is now refused
  rather than decoded as garbage. Address width follows the nexthop type, not
  the route family.
- route protocols now follow `lib/route_types.txt`, the canonical registry
  whose declaration order is the numeric value. The old table mapped 8 to ISIS
  and 11 to OSPF; 8 is OSPF6 and 11 is PIM. ISIS (9) and OSPF (7) were not
  mapped at all, so those routes decoded as UNSPEC and were dropped.
  RIP/RIPNG/PIM now map to UNSPEC explicitly instead of to a nearby protocol.
- the nexthop flag bits are named from `lib/zclient.h`; SEG6 is 0x10 and
  SEG6LOCAL 0x20.

Also done:
- nexthops carrying LABEL, EVPN, SEG6 or SEG6LOCAL are now **refused** with
  NOT_SUPPORTED. They were consumed for length but never interpreted, so an
  EVPN or SR-TE route was installed as an ordinary IPv4 route - silently
  forwarding where it should not, and contradicting the capability table.
- the gateway is copied at the nexthop type's width, not the route family's,
  so a v6 nexthop inside an IPv4 route no longer takes 4 bytes of a 16-byte
  address
- a blackhole nexthop now sets `DANOS_ROUTE_FLAG_BLACKHOLE` instead of becoming
  a nexthop with no gateway inside an NHGroup

Note the file carries **two ZAPI route dialects**: `zapi_map_route` decodes a
simplified layout used by the mock path, while `zapi_dispatch_frr` decodes the
native FRR layout and is what real FRR traffic takes. The nexthop fixes are in
the native path. The simplified dialect has no flags field, so it cannot carry
this information and the two will keep differing until it is retired.

Still open:
- `zapi_client` registration does not request ZAPI_SERVICE_IPV4 via
  `zebra_register_zclient`

### [ ] 10. Missing modules named in the architecture diagram

- `danos-ovs`, `danos-p4`, `danos-platform` are empty directories containing
  only a README. The diagram's "OVS (secondary)" and "P4 (programmable)"
  backends do not exist.
- `danos-models` contains no `.yang` files. The YANG/OpenConfig layer is a
  hand-written C enum plus a `strcmp` table; the generated
  `model_paths_gen.inc` is unreachable because its key convention
  (`"interface[name]"`) does not match the decoder's (`name="interface"` plus a
  separate `key_name`).
- RESTCONF does not exist (only mentioned in `docs/archive/`).
- No web UI.
- Hardware acceleration layer (NIC offload, SmartNIC/DPU, FPGA) has no code.

---

## P3 — engineering hygiene

### [~] 11. Golden-bytes tests against real upstream definitions

The single highest-leverage item for preventing a repeat of §4. Every VPP
encoder should be tested against bytes captured from a real VPP instance or
generated from VPP's `.api` files, and every ZAPI frame against bytes recorded
from a real FRR 10.3 zebra. Today both mocks reimplement the client's
assumptions, which is precisely why three wire defects and a handshake bug
survived a suite that passed 35/35.

This has now been got wrong in both directions, which is the argument for
doing it properly. The `.api` declarations alone are not sufficient: they
describe `string name [64]`, but the two legacy socket control messages are
exchanged as raw structs with a fixed 64-byte name and no length prefix, which
is only visible in `socket_client.c` and `socket_api.c`. Conversely the
`.api` declaration *is* authoritative for regular API messages, where
`admin_up_down` really is a `u8` and strings really are length-prefixed with
no padding. Both facts are recorded in
`docs/security-and-correctness-hardening-2026-10-04.md` §4.5.

Done for both protocols. `danos-fib/tests/test_zapi_golden.c` and
`danos-vpp/tests/test_vpp_golden.c` replace literal payloads with encoders
transcribed from upstream (`lib/nexthop.h`, `zebra/zapi_msg.c`,
`lib/zclient.h`, `lib/route_types.txt`, `api_types.h`, `interface.api`,
`socket_client.c`, `socket_api.c`), so the production decoders have to agree
with an implementation written from the specification. Citing the upstream
source in the file keeps the transcription auditable.

Both were verified against the bugs they exist to prevent: restoring the
type-3-only ifindex read fails the ZAPI per-type test, renumbering a nexthop
type fails the constant check, writing `admin_up_down` as a u32 fails the VPP
set_flags test, and adding field-relative padding to the string codec fails the
string test.

Still open: the fixtures are transcribed by hand from upstream source, not
generated mechanically from it. A generator that reads `route_types.txt` and
the `.api` files would remove the transcription step, which is the remaining
human-error surface. And none of this substitutes for item 3 - a live run
against a real VPP and FRR.

### [~] 12. Test-suite honesty

Done:
- the two vacuous conformance cases now assert. `conf_iface_crud` runs the full
  lifecycle, reads the object back, checks its contents, checks a duplicate
  create is refused with `EXISTS`, and checks the delete takes effect -
  verified to fail when the read-back expectation is disturbed.
  `conf_capability_query` asserts a definite, stable answer rather than
  discarding the status.
- `run_v016_release_gate.sh` no longer lets an environment-restricted lane pass
  silently. An open lane blocks the release by default; the policy lives in
  `release_gate_policy.sh` so it is unit-tested without running every lane, and
  `DANOS_RELEASE_ALLOW_OPEN_LANES=1` waives it deliberately with the waiver
  recorded in the result file. Only the exact value 1 waives.

Still open:
- the production C coverage workflow and fresh-run collector exist; the
  2026-10-20 baseline is 80.3% lines / 88.6% functions / 58.8% branches with
  CTest 81/81. The workflow now enforces a 79% line floor; raising the floor
  needs a new baseline. Remote job execution is still blocked by GitHub
  billing, and assertion quality remains open (see coverage-baseline.md).
2026-10-08: `conf_vrf_crud`, `conf_acl_crud` and `conf_qos_crud` now exercise
and assert readback, duplicate, update and deletion semantics. This closes the
listed thin-case gap but does not address the separate storage-vs-backend
scope limitation.

2026-10-07: Route/NH/NHGroup creation-only cases were replaced by full
storage lifecycle/visibility/abort/payload checks. Pipeline tests now pin
dependency recovery after retry exhaustion, mock VPP replay/withdraw/restore
and restart epoch retry reset. The listed VRF/ACL/QoS gaps remain open.

### [~] 13. Build and repository hygiene

Done: `build-asan/` (57 compiled artefacts, 34 MB) is untracked and ignored -
`.gitignore` covered `build/`, `build-tsan/` and `build-fuzz/` but not
`build-asan/`, which is how it slipped in. `tools/gen-model-paths/gen-model-paths`,
a compiled Go binary committed alongside its own `main.go` source, is likewise
untracked and ignored.

Still open:
- a 41 MB `.docx` is tracked. Removing it from *history* is a rewrite, so it
  needs an explicit decision rather than a drive-by `git rm`
- no SBOM, no artefact signing
- CI installs VPP from the floating `noble` FDio repo; container base images use
  floating tags and `apt-get` installs are unpinned

Partial progress: the new [coverage lane](coverage-baseline.md) pins its GitHub
actions to upstream commit SHAs and its gcovr/transitive Python packages to
versions plus wheel hashes. All other workflows now pin their actions by commit
SHA, gnmic builds from `tools/gnmic` with a checked-in `go.sum`, and
`build-test/`, `build-coverage/` and `build_testpmd/` are ignored. apt, VPP
packages, container base images, SBOM and signing remain open.

### [ ] 14. Architecture diagram accuracy

The diagram matches the intent, not the implementation. Three specific
inversions, in priority order:

1. **Capability Model** and **Transaction Engine** are drawn as two of DANOS
   Core's four components. One is unwired dead code, the other has no atomicity.
   These are the project's self-declared core IP (architecture spec §19).
2. **Control Plane (FRR)** lists PIM, LDP, EVPN and SR-TE. None has decode
   support in `danos-fib`. EVPN/SR are promoted to design principle #4 while
   the capability table also advertised EVPN support that did not exist.
3. **DPA row** omits Interface — the only object that works end to end — while
   listing seven that `type_skipped()` skips. **Data Plane Backends** omits the
   netlink backend, which is the one that passed the real kernel ping, while
   listing two empty directories.

Suggested: add a status marker to every box (implemented / partial / interface
skeleton / empty) with `docs/project-status.md` as the single source of truth,
and move Hardware Acceleration to a dashed optional layer.

### [x] 15. gRPC transport-layer defects

All fixed:
- per-stream windows keyed by real stream id instead of `stream % 64`, and the
  slot released when a stream ends
- `SETTINGS_INITIAL_WINDOW_SIZE` applied as the RFC 7540 §6.9.2 delta rather
  than overwriting live windows
- `WINDOW_UPDATE` now returned as inbound DATA is consumed; previously any
  request body over 64 KiB deadlocked the connection
- parked-frame queue made FIFO and bounded at 64 entries
- subscription registry and its queues guarded by a lock, with the ordering
  against the event-bus lock documented; `unsubscribe` detaches, releases the
  lock, then calls into the bus so the free is safe

## Historical checkpoints

Dated checkpoints from 2026-10-13 and earlier are kept in
[archive/outstanding-work-plan-history.md](archive/outstanding-work-plan-history.md).

