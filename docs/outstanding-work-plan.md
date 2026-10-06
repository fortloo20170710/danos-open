# Outstanding work plan

Derived from the v1.0 architecture review and the 2026-10-04 hardening pass
(`docs/security-and-correctness-hardening-2026-10-04.md`). Ordered by
dependency, not by severity: several items below are blocked on the two items
at the top.

Status legend: `[ ]` open · `[~]` in progress · `[x]` done · `[!]` blocked

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
metrics still require isolation/TLS proxy; NETCONF-over-SSH, online rotation
and CRL/OCSP are not included. Certificates/roles/CA reload on restart.
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

### [~] 3. Re-run the VPP live lane against real VPP

The first real runtime smoke is now complete against
`danos-vpp-runtime-recover:local` (`vpp v26.10-rc0~545-gad99177fe`). It passed
socket registration/message-table parsing (839 entries), `control_ping`,
interface admin-up, a two-path IPv4 route add/delete and stats-segment query.
This exposed and corrected two additional incompatibilities missed by the mock:
fixed 64-byte socket client/message-table names and the runtime's 32-bit
interface flags. Earlier mock-only results remain unverified.

Still required before closing this item: assert the created route in VPP FIB
before deletion, exercise VRF table programming and NH withdraw/re-add, then
run the corrected backend through the full QEMU FRR/VPP topology and recovery
gate. The loopback smoke is not packet-forwarding, DPDK, or throughput evidence.

2026-10-07: the [real control runtime gate](vpp-control-runtime-gate.md) now
asserts default/VRF 777 FIB after add/repeat/NH withdraw/restore/delete, including
table deletion. Those control assertions are complete; full current-build
QEMU FRR/VPP topology/recovery and physical packet/performance remain open.

Read-only [runtime capture tooling](vpp-runtime-capture.md) now collects
version/interface/FIB/load-balance/error/runtime snapshots, bounded commands,
raw output hashes, source identity and optional exact ISO/log attachments.
CAPTURED is not acceptance PASS. The legacy run_vpp_verify.sh no longer labels
kernel/mock tests or socket existence as real VPP/DPDK acceptance. Actual
FIB/table/NH and topology gates remain open; API frame evidence is item 11.

`docs/project-status.md` marks the physical I211 forwarding/ECMP result as FAIL
already, for independent reasons (single carrier, 1000/1000 loss).

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
- `danos_state_diff_desired_programmed()` is stubbed and `diff_cb` is an empty
  stub — drift detection is reported, not repaired per-type
- OPER does not exist: there is no path from backend state to an OPER view, so
  `DANOS_ERR_VERIFY_FAIL` is unreachable

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

- Set now applies delete → replace → update in one staging transaction.
  Interface replacement resets omitted modeled fields, preserves identity,
  and route ECMP resize retires obsolete next hops. Late failure rolls back
  interfaces and complete route graphs together.
- Get returns concrete keyed object paths (route prefix plus VRF), correctly
  filters VRF entries, and no longer uses fixed collection-size caps.
  Supported forms and deliberate exclusions are documented in
  [Set/Get semantics](gnmi-set-get-semantics.md).

Still open:
- `updates_only`, `use_models`, `encoding` are ignored; no bytes/decimal
  `TypedValue`
- STREAM still polls on a 200 ms tick rather than subscribing to store events,
  so latency and CPU cost scale with stream count

### [ ] 8. NETCONF has no transport

Message layer only (~250 LOC): no socket, no SSH, no RFC 6242 framing. It
advertises `candidate`, `running`, `rollback-on-error` capabilities that are not
implemented. XML parsing is `strstr`, so two `<interface>` blocks yield only the
first. `session-id` is hardcoded to 1.

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
- a production C coverage workflow and fresh-run collector now exist; initial
  local baseline is 77.5% lines / 55.5% branches with CTest 41/41. Remote job
  execution is still blocked by GitHub billing; assertion quality and a
  coverage non-regression policy remain open (see coverage-baseline.md).
- other conformance cases are still thin: `conf_vrf_crud`,
  `conf_acl_crud` and `conf_qos_crud`
  exercise the calls but assert almost nothing about the results

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
versions plus wheel hashes. This does not pin the other workflows, apt, VPP
or container inputs and does not close SBOM/signing/history items.

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
