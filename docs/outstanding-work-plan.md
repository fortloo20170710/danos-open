# Outstanding work plan

Derived from the v1.0 architecture review and the 2026-10-04 hardening pass
(`docs/security-and-correctness-hardening-2026-10-04.md`). Ordered by
dependency, not by severity: several items below are blocked on the two items
at the top.

Status legend: `[ ]` open · `[~]` in progress · `[x]` done · `[!]` blocked

---

## P0 — blocks any real deployment

### [~] 1. TLS transport and authentication on the management plane

Done so far: the enforcement layer exists and is wired. `danos-security` is
linked into `danos-mgrd`, `danos-mgmt/src/authz/` performs one RBAC check per
northbound RPC (gRPC returns `grpc-status 7`, NETCONF returns
`access-denied`), every authorization decision and transaction lifecycle event
goes to an append-only audit log, and the daemon initialises both before
accepting a request. `DANOS_AUTHZ_DEFAULT_ROLE` can lock the daemon to
operator/viewer before TLS exists. Covered by `authz_test`.

Authentication now exists. `danos_authz_configure()` accepts a bearer-token
file (RFC 6750 `Bearer`, constant-time compare, must be mode 0600 or a
world-readable token is refused) and/or `SO_PEERCRED` on a unix socket
(uid 0 → admin, any other uid → the configured default role). gNMI
authenticates per RPC from the `authorization` header and answers
`grpc-status 16` when it is missing or wrong; NETCONF captures the credential
from `<hello>` and rejects the session. mgrd refuses to start if a configured
token file cannot be used, and `DANOS_AUTHZ_REQUIRE_AUTH` makes a missing
credential source fatal instead of defaulting to open.

What remains: there is **no transport security**, so a bearer token crosses
the wire in cleartext and must not be used on an untrusted network. mTLS is the
remaining work, and it is why the token path exists behind a switch rather than
being the only option. `danos_gnmi_grpc.h` still describes the listener as
plaintext h2c; that remains true.

The daemon serves plaintext h2c on `INADDR_ANY:57400` with no authentication
and no authorization. `danos_gnmi_grpc.h` states this outright. Every peer that
can reach the port has full read/write over the config and routing tables, and
can install a gNMI `Subscribe` stream.

`danos-security` (RBAC, audit, CoPP — 608 LOC) exists, is unit-tested, and is
**not linked into `danos-mgrd`**. It has no callers anywhere in the daemon, so
the repository currently ships a security module that provides no security.

The memory-safety defects that would have compounded this exposure are fixed.
What remains is the absence of transport security and identity.

Needs an identity-source decision before implementation:
- mTLS with a client CA (closest to the northbound model; needs cert management)
- bearer token in `authorization` (the header is currently parsed but ignored)
- unix-socket peer credentials (`SO_PEERCRED`) for local-only operation

Suggested sequence: decide identity source → add TLS to the gRPC accept path →
link `danos-security` → enforce `danos_rbac_check` at every handler entry →
audit-log mutations.

### [~] 2. Transaction engine must actually provide atomicity

All 30+ DPA CRUD entry points begin with `(void)tx;`. The transaction is
decorative: objects are written and fsynced immediately, before any commit.
Consequences:

- A gNMI `Set` with 20 updates applies them one at a time, each independently
  durable. A failure at update 13 leaves 12 applied and rolls back nothing.
- `NETCONF` advertises `candidate` and `rollback-on-error` capabilities that
  do not exist.
- The state machine, `prepare`/`validate`/`commit`/`verify` sequence and the
  `DANOS_ERR_TX_CONFLICT` code all exist but are unreachable.

ADR-0005 specifies the intended design.

Done: the atomicity *primitive* now exists. `danos_object_apply_batch()`
applies a set of mutations under a single store write-lock acquisition, so a
concurrent reader or the reconciler sees either none or all of a change set -
not a route installed before the interface it depends on. `store_entry` and
`remove_entry` were factored into `*_locked` helpers so the batch path and the
single-object paths share one implementation. Covered by
`test_object_batch_atomic` (end state, mixed write+delete, oversized refusal).

Still open - **this item is not done**, and the batch primitive is not yet on
the commit path:
- DPA CRUD still writes straight through to the store; nothing stages
- `danos_tx_commit` still applies nothing, so behaviour is unchanged
- no read-your-writes overlay, so a transaction cannot observe its own staged
  state
- no per-object version counters, so `DANOS_ERR_TX_CONFLICT` is unreachable
- the batch is all-or-nothing only for the lock window: a mutation that fails
  midway leaves the earlier ones applied. Pre-validation is the caller's job.

The next step is to have the CRUD entry points stage into the transaction and
apply via `apply_batch` at commit. That is a behavioural change to every
write path and should land as its own series.

#### Findings from an attempted implementation

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

A third attempt should start by writing a test that pins the intended
transaction semantics (including what an abort must guarantee), fix the callers
to it, and only then turn staging on. Turning it on first is what both attempts
did, and it is why both had to be reverted.

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

Still open:
- `replace` is decoded but never applied
- `Get` echoes the request path for each update instead of the per-object path
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
- no code coverage measurement anywhere in CI
- other conformance cases are still thin: `conf_vrf_crud`, `conf_route_crud`,
  `conf_nh_crud`, `conf_nhgroup_crud`, `conf_acl_crud` and `conf_qos_crud`
  exercise the calls but assert almost nothing about the results

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
