# Outstanding work plan

Derived from the v1.0 architecture review and the 2026-10-04 hardening pass
(`docs/security-and-correctness-hardening-2026-10-04.md`). Ordered by
dependency, not by severity: several items below are blocked on the two items
at the top.

Status legend: `[ ]` open · `[~]` in progress · `[x]` done · `[!]` blocked

---

## P0 — blocks any real deployment

### [ ] 1. TLS transport and authentication on the management plane

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

### [ ] 2. Transaction engine must actually provide atomicity

All 30+ DPA CRUD entry points begin with `(void)tx;`. The transaction is
decorative: objects are written and fsynced immediately, before any commit.
Consequences:

- A gNMI `Set` with 20 updates applies them one at a time, each independently
  durable. A failure at update 13 leaves 12 applied and rolls back nothing.
- `NETCONF` advertises `candidate` and `rollback-on-error` capabilities that
  do not exist.
- The state machine, `prepare`/`validate`/`commit`/`verify` sequence and the
  `DANOS_ERR_TX_CONFLICT` code all exist but are unreachable.

ADR-0005 specifies the intended design. Needs a staging area plus a single
commit-time swap, and per-object version counters for conflict detection.

Note the recorder fix from this pass: transaction records are now correctly
retired, so this work no longer sits on top of a leak.

---

## P1 — correctness of the claims the project makes

### [ ] 3. Re-run the VPP live lane against real VPP

The VPP wire-encoding fixes (string representation, message-table walk,
`admin_up_down` width) mean all previously recorded VPP protocol results are
unverified: they were produced against `mock_vpp_server.c`, which encoded
strings the same wrong way, and the tests asserted those wrong layouts.

Until a live lane runs against real VPP 26.10, the following must not be
claimed as verified:
- VPP binary-API handshake and message-table lookup
- interface admin-up/down
- VRF table programming
- route add/withdraw, ECMP, FIB programming
- any recorded VPP forwarding or ECMP traffic result

`docs/project-status.md` marks the physical I211 forwarding/ECMP result as FAIL
already, for independent reasons (single carrier, 1000/1000 loss).

### [ ] 4. Capability registry must be wired, or deleted

`capability_registry.c` is 38 lines with no header, no callers, and an
unbounded `for (;;)` in its registration path. `danos_vpp_capability_register()`
is defined but never called, so the registry is empty at runtime.

The capability *table* was corrected in this pass to state only what the VPP
backend can program, so wiring it now would be truthful. Completing this needs:
- a header and a real registry with bounded slot allocation
- call `danos_vpp_capability_register()` from `danos-vpp` install
- implement backend selection over capability + load (ADR-0007)
- keep `type_skipped()` and the capability table in agreement — ideally derive
  one from the other

### [ ] 5. Reconciler must be driven by the daemon

Three gaps in `backend_ops.c`, all of which make the advertised
DESIRED → PROGRAMMED → OPER model non-functional:
- `danos_programming_sweep()` is never called by the daemon, so deleted config
  is never withdrawn from the dataplane
- `danos_state_diff_desired_programmed()` is stubbed and `diff_cb` is an empty
  stub; production initialises the reconciler with `NULL` (no state store)
- OPER does not exist — there is no path from backend state to an OPER view

Also unwired: `max_retries`, `backoff_ms`, `antiflap_*` are read from config
and never referenced, so a missing next-hop retries every 50 ms indefinitely.

### [!] 6. HA and CoPP are duplicated and disconnected

`danos-ha` (BFD, VRRP, supervisor — 630 LOC) and `danos-security` (CoPP policer,
RBAC, audit) are each built as standalone libraries with only their own test
linking them. Neither is in `danos-mgrd`'s link line. Blocked on item 1 for
the security half.

ADR-0006 delegates BFD to FRR's `bfdd` while `danos-ha/bfd.c` implements it
again — the duplication needs resolving one way or the other.

---

## P2 — protocol compliance gaps

### [ ] 7. gNMI `Subscribe` and `Set` semantics

- POLL mode does not set `sync_response` (violates the response oneof) and
  leaves the stream open indefinitely
- STREAM mode polls every 200 ms and diffs a cheap hash that misses VRF-name and
  route-prefix changes
- `replace` is decoded but never applied
- `Get` echoes the request path for each update instead of the per-object path
- `updates_only`, `use_models`, `encoding` are ignored; no bytes/decimal
  `TypedValue`

### [ ] 8. NETCONF has no transport

Message layer only (~250 LOC): no socket, no SSH, no RFC 6242 framing. It
advertises `candidate`, `running`, `rollback-on-error` capabilities that are not
implemented. XML parsing is `strstr`, so two `<interface>` blocks yield only the
first. `session-id` is hardcoded to 1.

### [ ] 9. FRR nexthop types and protocol mapping

In `zapi_mapper.c`:
- nexthop types 4 (IPv6), 5 (blackhole) and 6 (have-nh-v6) are unparsed — ZAPI
  route messages with these are silently dropped
- LABEL / EVPN (0x40) / SEG6-LOCAL (0x10) / SEG6 (0x20) nexthop flags are not
  parsed, so those routes are mis-decoded rather than cleanly rejected
- `map_frr_protocol()` disagrees with FRR's `route_types.h` (connected/ISIS
  becoming UNSPEC); delete keys on `route.protocol`, so the asymmetry leaks FIB
  entries
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

### [ ] 11. Golden-bytes tests against real upstream definitions

The single highest-leverage item for preventing a repeat of §4. Every VPP
encoder should be tested against bytes captured from a real VPP instance or
generated from VPP's `.api` files, and every ZAPI frame against bytes recorded
from a real FRR 10.3 zebra. Today both mocks reimplement the client's
assumptions, which is precisely why three wire defects and a handshake bug
survived a suite that passes 35/35.

### [ ] 12. Test-suite honesty

- `conformance_cases.c` treats `NOT_SUPPORTED` as a pass, so the v0.1 "11/11" is
  vacuous for unimplemented features
- `run_v016_release_gate.sh` records an environment-restricted lane as
  `ENVIRONMENT-OPEN` without incrementing `failures`, so a release can proceed
  with a hardware lane never run
- no code coverage measurement anywhere in CI

### [ ] 13. Build and repository hygiene

- `build-asan/` — 57 compiled artefacts — is tracked in git
- a 41 MB `.docx` is tracked
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

### [ ] 15. gRPC transport-layer defects

Not yet triaged in detail. Known issues in `gnmi_grpc.c`:
- connection-level and stream-level flow-control windows share one
  `stream_window[stream % 64]` array; `SETTINGS_INITIAL_WINDOW_SIZE` overwrites
  all 64 slots; `WINDOW_UPDATE` is read but never sent
- the parked-frame queue is LIFO, so multi-frame gRPC messages are reassembled
  out of order, and it is unbounded
- the global subscription registry is unsynchronised between the event-callback
  and HTTP handler threads; `unsubscribe` frees state that a live subscriber may
  still be using
- event-bus callbacks run while the registry read lock is held