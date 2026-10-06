# gNMI Set/Get semantics

## Supported contract (2026-10-06)

Set applies deletes, then replaces, then updates, preserving order within
each list. All supported mutations share one DPA staging transaction.
Later operations see the candidate overlay. Validation, response-capacity,
conflict or commit failure aborts the entire candidate; desired-state objects
are published only on successful commit. Backend reconciliation remains
asynchronous: this is desired-state atomicity, not simultaneous hardware
programming or a full repeatable-read isolation guarantee.

The ordering and replacement distinction follow the
[OpenConfig gNMI specification](https://github.com/openconfig/reference/blob/master/rpc/gnmi/gnmi-specification.md)
and [protocol definition](https://github.com/openconfig/gnmi/blob/master/proto/gnmi/gnmi.proto).
Prefix and relative path are combined once; conflicting origins and excessive
depth are rejected. Results report the resolved path and DELETE/REPLACE/UPDATE.

### Interface writes

Supported paths are keyed interface entries, their config containers and
existing writable leaves. Entry/config replace resets omitted modeled config:
MTU 1500, enabled true, IPv4/IPv6 address absent. Update merges specified
fields. Leaf replace changes only that leaf. Name and ifindex are immutable;
an existing named interface keeps its ID. Unmodeled/runtime fields such as
link state are preserved rather than invented or reset.

Entry JSON uses the existing flat representation, not arbitrary nested YANG
JSON: name, ifindex, mtu, enabled (or admin_up), ipv4-address, ipv6-address.
Unknown/duplicate fields, malformed JSON, inconsistent identities and
read-only writes fail the request. Config/leaf deletes reset supported defaults;
keyed entry delete removes the object. Missing entry delete is idempotent.

### Routes

IPv4 static route entries accept gateway or gateways, oif, vrf and prefix.
Update preserves existing path/oif fields when omitted; replace requires
gateway(s), defaults oif to zero and resets route metric/distance defaults.
The route, next-hop group and next hops are staged together; shrinking ECMP
retires excess next hops. Deletion removes the complete helper-owned graph.

Canonical identity is /routes/route[prefix=X][vrf=N]. Either protobuf map
order is accepted. Legacy prefix-only requests default to VRF 0 unless JSON
explicitly supplies vrf; keyed and JSON VRF must agree. A prefix-only Get may
match multiple VRFs and returns a distinct two-key path for each.

### Get

List/subtree responses identify actual interface names, VRF IDs and route
prefix/VRF pairs instead of repeating the queried collection path. Entry
queries filter the requested object, including VRFs. IPv6 route prefixes are
formatted as actual addresses. Dynamic snapshots replace the old fixed
interface/VRF/route collection arrays; response overflow is an error, not a
successful truncated collection.

## Explicit limits

- Whole-list/root replace and union_replace are rejected, not silently ignored.
- At most two keys per PathElem; duplicate/oversized keys are rejected rather
  than truncated into another object's identity.
- The current DPA engine binds the default object store. Set against a different
  explicitly bound desired store is rejected; Get uses its bound store.
- Existing flat route JSON reports its existing summary representation, not a
  lossless full ECMP/YANG round-trip. Non-static routes are not writable here.
- Subscribe notifications, updates_only, use_models, encoding selection and
  event-driven STREAM remain separate outstanding work. This is not a claim
  of complete gNMI conformance.

## Regression evidence

gnmi_set_semantics covers merge versus replace defaults, stable IDs, reversed
wire-field ordering, prefix composition, read-your-writes, late validation and
response-buffer rollback, ECMP shrink/rollback, cross-VRF keys, concrete Get
paths and a collection exceeding the old 32-route cap. gnmi_grpc exercises all
40 reported operations (16 updates, 16 replaces, 8 deletes) over HTTP/2.
The existing mTLS daemon, route-composite and frontend tests remain required.
Debug, ASan/UBSan/LSan and TSan each pass the full 40-test suite. Collection
coverage also includes 24 VRFs and keyed VRF filtering. Remote GitHub CI is
not counted as PASS while its account billing lock prevents jobs from starting.
