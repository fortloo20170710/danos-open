# Security and correctness hardening — 2026-10-04

A focused remediation pass over the defects identified in the v1.0 architecture
review. Scope was the P0 class: remotely reachable memory-safety bugs, a
persistence path that silently dropped an entire object class, and binary-API
wire encodings that did not match VPP's real `.api` definitions.

Everything here is covered by automated tests, and every new test was verified
to fail against the unfixed code (by ASan, or by reverting the specific fix and
observing the assertion).

## Verification summary

| Build | Before | After |
|---|---|---|
| Debug (no sanitizer) | 35/35 | 35/35 |
| ASan + UBSan + LSan | 20/35 (15 failures) | **35/35** |
| TSan | 32/35 (3 failures) | **35/35** |

The 15 pre-existing ASan failures were not test regressions; the sanitizer jobs
were already red on `main`. The suite was re-run 8 consecutive times (serial and
parallel) after the changes to confirm stability.

Seven new test functions were added, each pinned to the defect it guards.

## 1. Remotely reachable memory safety

### 1.1 Reconciler ledger stack overflow

`backend_ops.c` copied an object's payload into a fixed 520-byte stack buffer
with `memcpy(rec + 8, e->data, e->data_size)` and no length check. The object
registry accepted any payload size, so any object wider than the buffer smashed
the reconciler thread's stack.

Introduced `DANOS_MAX_OBJECT_BYTES` (1024) in `danos/dpa.h` as the single
authoritative limit. The registry rejects oversized payloads on create *and*
update (leaving any existing payload intact on a rejected update), and the
ledger buffers are sized from the constant rather than a literal.

`danos_nhgroup_t` is 536 bytes — already wider than the previous 512-byte read
buffer. It was only safe because `type_skipped()` returned before the copy.
That was implicit, not enforced.

While rewriting the same function, two further defects were corrected:

- The tombstone sweep applied a blanket `if (lsz < 8 + sizeof(danos_route_t))
  continue;` guard, which skipped the ledger delete for every object type
  smaller than a route and leaked the entry permanently. Each withdraw branch
  now range-checks its own type.
- The sweep shadowed its loop variable with a `danos_iface_t i`.

Test: `test_object_size_bounds` (core).

### 1.2 NETCONF get-config buffer overflow

`netconf.c` accumulated the reply into a file-scope 8 KiB buffer with
`g_nc_off += snprintf(buf + g_nc_off, sizeof - g_nc_off, ...)`. Past capacity,
`snprintf` reports the length it would have written while
`sizeof - g_nc_off` underflowed to a huge `size_t`, so a store holding a few
hundred interfaces wrote past the end of the buffer. The buffer being
file-scope also made it shared across concurrent sessions.

Replaced with a per-call growable heap builder, which removes both the overflow
and the cross-session reentrancy. Interface names are now XML-escaped, so a
name cannot inject markup into the reply.

Tests: `test_netconf_get_config_large` (400 interfaces), `test_netconf_escapes_name`.

### 1.3 gNMI Set response array overflow

`gnmi_set_response_t.result_paths` was sized `GNMI_MAX_UPDATES` (16), but one
`SetRequest` can carry 16 updates plus 16 replaces plus 8 deletes, and the
handler appends one result per applied operation. Each `gnmi_path_t` is
~1.5 KiB, so the overflow was ~12 KiB past the end of a stack struct.

Added `GNMI_MAX_SET_OPS` (40) and a bounds-checked `gnmi_set_response_add()`,
and replaced all six unguarded append sites. The struct also moved off the
connection thread's stack (~65 KiB).

Test: `test_set_max_operations` (24 reported operations).

### 1.4 gNMI error codes returned as response length

`gnmi_handle_get` returns a response *length*, with errors signalled as a
negated `danos_status_t`. Two paths returned the status unnegated, so the
dispatcher read e.g. `2` as "two bytes of body" and answered a Get for a
missing object with `grpc-status 200` and a two-byte garbage payload — silent
corruption reported as success.

Both paths now negate. Note that a *subtree* Get matching nothing is a correct
successful empty result and was deliberately left alone.

Test: `test_get_not_found_status`, asserted at the handler boundary so it pins
the contract rather than HTTP/2 trailer framing.

### 1.5 ZAPI next-hop group overflow

`zapi_mapper.c` read `nh_count` as a wire `u8` (max 255) and wrote that many
entries into `nh_ids[64]`. Added `DANOS_NHGROUP_MAX_NH` (64) and reject an
oversized advertisement; the `zapi_frr_route_t` path is guarded locally too, so
the mapper is correct regardless of how the struct was filled.

Test: `test_map_route_nh_overflow` (200 rejected, 64 accepted). ASan confirms
a stack-buffer-overflow without the guard.

## 2. Defects found while writing the above tests

These were not in the original review; each surfaced from a test that the fix
work made possible to write.

### 2.1 Every transaction leaked its record

`danos_tx_record_free()` had no callers anywhere. Since the management plane
does `begin → commit` and never follows with `verify`, every successful remote
gNMI/CLI operation leaked a `danos_tx_record_t`. In a long-lived daemon driven
by remote input this is unbounded growth.

- `danos_tx_commit` now retires the record, which is the last step for most
  callers.
- The post-commit transitions (`verify`/`abort`/`rollback`) read and write the
  state held in the caller's own `danos_tx_t` rather than the record's copy.
  Every caller owns its `danos_tx_t` on its own stack, so the record lock never
  serialised anything between threads — it only guarded a struct the caller
  already had a copy of. Depending on the record there would now be a read of
  memory another thread could own.
- `danos_tx_release()` was added for callers that want to be explicit, and a
  deadline-based reclaim pass in `danos_tx_record_alloc` bounds the pool even if
  a caller abandons a transaction without reaching a terminal state.

Three existing tests were calling `commit` without `prepare`/`validate` and
ignoring the return value, so commit failed its state precondition and the
record was never retired. They now use `danos_tx_commit_atomic` and assert.

### 2.2 `object_iterate` held the read lock across callbacks

`danos_object_iterate` invoked callbacks with the store rwlock held for
reading. A callback that reads the same store re-enters the lock for read;
POSIX read locks are not reentrant-safe, so if a writer queues between the two
acquisitions the second `rdlock` blocks behind it while the first is still
held. This was reachable today: `danos_programming_sweep()` iterates the ledger
while `sweep_collect()` calls `danos_object_read()` on that same ledger.

Callback invocation also blocked every concurrent CRUD for the duration of a
backend round trip, since the programming pass reaches netlink and a VPP socket.

Iteration now takes a deep-copied snapshot under the lock and invokes callbacks
with the lock released. TSan reported this as a lock-order inversion;
`programming_pipeline` and `route_composite` both went from failing to passing.

### 2.3 gNMI Set crashed the server on any delete

`store_or_default()` returns NULL when no state store is configured, and the
delete paths dereferenced `ss->desired` directly. Any `SetRequest` carrying a
delete killed the connection thread.

The same code also had an inconsistency: update paths wrote `g_default_store`
while delete paths wrote `ss->desired`, so a create-then-delete in one request
operated on two different stores. A single `desired_store()` resolver now backs
every path.

### 2.4 Fixed-capacity collect-then-search lookups

Several handlers copied every object of a type into a fixed array and then
searched it: `ifaces[16]` for deletes, `all[64]` for the ifindex allocator,
`all[1024]` for leaf updates. Once a store held more objects than the array,
the wanted one was absent from the copy and the lookup reported NOT_FOUND even
though the object existed — which aborted the whole `SetRequest`.

The ifindex allocator had a second consequence: past 64 interfaces it computed
a maximum over only a prefix of the store, so it chose an ifindex that was
already taken and the create failed with `EXISTS`.

Added `danos_object_max_id()` (walks the store directly, no capacity limit) and
a streaming `find_iface_by_name()`.

## 3. Persistence: routes were never durable

Route object ids are 64-bit FNV-1a hashes of `(vrf, prefix, protocol)`, so they
exceed 32 bits for essentially every prefix. The WAL header stored `obj_id` as
`uint32_t`, and `danos_persist_on_mutation` dropped anything above
`0xFFFFFFFF` outright. **No route was ever written to the WAL, and none
survived a restart.** QoS bind ids, which set bit 63, were dropped for the same
reason.

Widened `obj_id` to 64 bits end to end — WAL header, record struct, replay
slot, and the `danos_persist_log_op` signature. `WAL_MAGIC` was bumped to
`0x444E4F32` ("DNOS2") so a v1 file is rejected at the magic check and replay
stops rather than misparsing it; there is nothing to lose, since v1 never stored
a route.

The checksum was also `crc32(header) ^ crc32(data)`, which a bit error in the
header and a matching error in the payload can cancel out. It now chains the
payload CRC from the header CRC.

Test: `test_route_persistence`, which first asserts the computed id really does
exceed 32 bits (so the test would fail even with the old truncation) and then
verifies the route round-trips a restart.

## 4. VPP binary API wire encoding

Three encodings did not match VPP's `.api` definitions. Notably, the mock
server reimplemented the *same* assumptions, so the existing tests asserted the
wrong layouts and could not have caught these.

### 4.1 `admin_up_down` width

Correction from the real Debian trixie VPP 26.10 runtime: the request uses
`u32 sw_if_index; u32 flags` (after the standard client/context fields). The
prior analysis incorrectly used the legacy `u8 admin_up_down` form. The one-byte
encoder sent a 15-byte frame where VPP's generated message-size check requires
18 bytes and VPP dropped it as truncated. The encoder now sends the 32-bit
`IF_STATUS_API_FLAG_ADMIN_UP` value.

### 4.2 `string` representation

A VPP `string` is a `u32` length followed by that many bytes, zero-padded to a
4-byte boundary *of the field length*. The codec used a `u8` length with no
padding, which shifted every following field.

Padding is relative to the field, not the offset within the message — an
earlier attempt anchored it to the write position and produced two spurious
pad bytes. `vpp_buf_put_string` and `vpp_rd_string` now agree, and the reader
additionally rejects a declared length that cannot fit in the remaining buffer,
so a `u8`-length field where a `u32` belongs fails loudly instead of
desynchronising silently.

### 4.3 Socket client registration and message-table walk

The legacy socket handshake uses fixed-width fields, not ordinary variable
length API strings: `sockclnt_create` is `u32 context; u8 name[64]`, and each
reply table entry is `u16 index; u8 name[64]`. The earlier dynamic-string
encoding sent a 22-byte request where VPP requires 70 bytes, so the runtime
dropped the registration before replying. The parser also treated table names
as length-prefixed strings. Both now use the fixed wire layout; ordinary VPP
`string` fields elsewhere remain length-prefixed and padded.

### 4.4 Stat segment alignment

The stat-segment reader treated shared memory as if it held aligned C
structs. The mapping is page-aligned, but `directory_offset` and the offset
vector inside it carry no alignment guarantee, so `uint64_t *` dereferences were
undefined behaviour. UBSan trapped it. Entries and counter values are now read
by `memcpy` into aligned locals.

Test: `test_wire_layouts` now asserts the real layouts, and `test_stat_segment`
passes under UBSan.

### 4.5 Which upstream definitions govern which field

Two different mechanisms are in play, and conflating them is what caused this
to be got wrong twice. Both were settled by reading VPP's own code, not the
`.api` declarations.

**Regular API messages** go through vlapi's length-driven encoder:

| Field | Encoding | Evidence |
|---|---|---|
| `sw_interface_set_flags.admin_up_down` | `u8` | `interface.api`: `u32 client_index; u32 context; u32 sw_if_index; u8 admin_up_down;` |
| any `string` field (e.g. `ip_table_add_del.name`, policer `name`) | `u32` length + exactly that many bytes, **no padding** | `vl_api_to_api_string()` in `api_types.h` returns `len + sizeof(u32)` |

An earlier revision padded strings to a 4-byte boundary. That was wrong and the
padding has been removed.

**The two legacy socket control messages** (`sockclnt_create` and
`sockclnt_create_reply`'s message table) do *not* use vlapi's string encoding.
VPP exchanges them as raw structs:

- VPP's own reference client writes the name as a fixed, zero-padded 64-byte
  buffer: `strncpy((char *) mp->name, client_name, sizeof(mp->name) - 1)`
  (`socket_client.c`)
- the server handler reads it as a plain C string: `format(0, "%s%c", mp->name, 0)`
  (`socket_api.c`)
- the reply's message table is filled with
  `strncpy_s((char *)rp->message_table[i].name, 64, hp->key, 64-1)`
  and no length field is ever written

So `string name [64]` in `memclnt.api` describes the *abstract* type; the `[64]`
is a capacity, and on this particular path the wire form is a fixed 64-byte
buffer with no length prefix. Treating it as a length-prefixed string
desynchronises the whole message table.

`mock_vpp_server.c` validates the `sockclnt_create` request strictly (exact
frame length of 70 bytes, context bytes, and the name zero-padded to 64), so a
regression in either the structure or the length fails the suite rather than
passing silently.

**These fixes invalidate part of the earlier mock-only VPP evidence.** A real
runtime smoke was subsequently run against the local Debian trixie image
`vpp v26.10-rc0~545-gad99177fe`. With two VPP loopbacks, the rebuilt client
registered successfully, loaded 839 API message names, completed `control_ping`,
set interface 1 admin-up, added and deleted a two-path IPv4 route, and queried
`/sys/node/vectors` from the stats segment. `vpp_live_test` reported PASS and the
post-check confirmed interface 1 up and the test route absent. The socket test
also now validates the fixed-width request and reply layouts. This is real API
and FIB-programming evidence, but not packet-forwarding, DPDK, or throughput
qualification: this runtime used loopbacks and no DPDK devices.

## 5. Capability table honesty

`vpp_capability.c` advertised IPv6, EVPN (type2/3/5/irb/mh), ACL (ingress/
egress/l4), MPLS, QoS and BFD. The adapter implements ops for three types only
(`iface_up`, `iface_addr_set/del`, `route_add/del`, `vrf_add/del`), routes are
rejected for anything but `DANOS_AF_IPV4`, and `type_skipped()` skips every type
except interface, route and VRF.

An empty capability registry only makes discovery non-functional. A *wrong* one
is worse: it steers capability-based backend selection toward a backend that
silently drops those objects, and it breaks the ADR-0007 contract that adding a
backend means implementing the ops and registering them.

The table now advertises exactly what the backend can program. Note that
`danos_vpp_capability_register()` is still never called, so registration remains
unwired — that is deliberately left to the transaction/capability work rather
than done on top of an unverified registry.

## Files touched

```
danos-core/src/backend/backend_ops.c          danos-mgmt/src/gnmi/gnmi_grpc.c
danos-core/src/object/object_registry.c       danos-mgmt/src/gnmi/gnmi_proto.h
danos-core/src/persist/persist.c              danos-mgmt/src/netconf/netconf.c
danos-core/src/transaction/transaction.c      danos-vpp/src/api/vpp_api.c
danos-core/src/wal/wal.c                      danos-vpp/src/api/vpp_msgs.c
danos-core/include/danos/core/persist.h       danos-vpp/src/api/vpp_stat.c
danos-core/include/danos/core/transaction.h   danos-vpp/src/api/vpp_wire.c
danos-core/include/danos/core/wal.h           danos-vpp/src/capability/vpp_capability.c
danos-core/include/danos/core/object_registry.h
danos-dpa/include/danos/dpa.h                 tests: test_object.c, test_main.c,
danos-fib/src/mapper/zapi_mapper.c              test_persist.c, test_netconf.c,
danos-fib/tests/test_zapi_mapper.c               test_gnmi_grpc.c,
                                                test_zapi_mapper.c,
                                                test_vpp_proto.c, mock_vpp_server.c
```
