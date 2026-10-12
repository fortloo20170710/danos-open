# Upstream DANOS tests adaptation

Reviewed 2026-10-11 at immutable upstream commit
[`1c731e0644acf98cb2c3d0254653a417d41a3667`](https://github.com/i-danos/tests/tree/1c731e0644acf98cb2c3d0254653a417d41a3667).
GitHub identifies i-danos/tests as a fork of danos/tests; this document does
not assume every fork change is an original DANOS release requirement.

## Findings and boundary

The upstream framework separates Robot scripts, reusable keywords, Python
libraries and topology data. Its README specifies Robot Framework,
SSHLibrary and vymgmt. Its BGP suite covers neighbor establishment,
next-hop and route selection attributes. Its DPA suite checks operational
backend/object visibility. These are useful test objectives, not compatible
wire APIs or command syntax for DANOS-Open.

Reviewed sources: [framework README](https://github.com/i-danos/tests/blob/1c731e0644acf98cb2c3d0254653a417d41a3667/Test_Automation/README.md),
[BGP suite](https://github.com/i-danos/tests/blob/1c731e0644acf98cb2c3d0254653a417d41a3667/Test_Automation/script/BGP_DANOS.robot),
[DPA keywords](https://github.com/i-danos/tests/blob/1c731e0644acf98cb2c3d0254653a417d41a3667/Test_Automation/keyword/DPA_DANOS_keywords.robot),
[CLI library](https://github.com/i-danos/tests/blob/1c731e0644acf98cb2c3d0254653a417d41a3667/Test_Automation/library/danos_cli.py),
[management preservation](https://github.com/i-danos/tests/blob/1c731e0644acf98cb2c3d0254653a417d41a3667/Test_Automation/keyword/mgmt_keywords.robot).

Do not directly execute legacy suites on R1–R4: old CLI paths, credentials,
REST and configuration semantics differ. Existing configuration-clear
steps can affect management connectivity. The upstream CLI library disables
SSH host-key checking and includes broad process termination; neither policy
is adopted. No configuration clearing, hardware rebinding, plaintext mgrd
enablement or protocol expansion is authorized by a read-only suite.

Root LICENSE is BSD-3-Clause while several framework files declare
LGPL-2.1-only. No upstream code is copied here; preserve applicable notices
and review per-file licensing before any future copying.

## Initial implementation

The repository already contains `DANOS_OPEN.robot`, a legacy-inspired gNMI
adapter with writes and `--insecure` defaults. It is not a safe default for
physical qualification; it is retained without silently changing its scope.

New original implementation: `danos-test/danos-robot/VPP_READONLY.robot`
and `vpp_readonly.py`. Run locally on the explicit VPP runner, not on the
development host and assume it reaches hardware. It requires an expected
MAC before suite checks. It uses fixed read-only subprocess argv, no shell,
timeouts, nonzero-exit checks and rejection of invalid-command text even
when the CLI exits zero. No upstream software-backend assertions are reused
as VPP capability claims.

Example with Robot Framework 7.3.2 in an isolated environment:

```sh
# R3 only: expected MAC and physical lab route must actually match.
python3 -m robot --variable EXPECTED_MAC:00:1f:7a:69:f7:4c \
  --outputdir ./robot-results danos-test/danos-robot/VPP_READONLY.robot
# R2: do not apply R3's route expectation.
python3 -m robot --exclude r3-only \
  --variable EXPECTED_MAC:00:1f:7a:69:f5:ec \
  --outputdir ./robot-results danos-test/danos-robot/VPP_READONLY.robot
```

The suite checks version identity, physical counters, and (R3 only) the two
resolved ECMP next hops. It does not qualify packets, latency or throughput.
Mock unit tests are deterministic adapter validation, not live-device PASS.
Robot dry-run similarly validates structure only.
Initial validation: four adapter unit tests PASS, full CTest 67/67 PASS,
and Robot 7.3.2 actual dry-run 3/3 PASS after fixing library path/class
discovery. Reports are in `build/robot-vpp-readonly-dryrun/`. No physical
runner Robot execution or configuration change was performed in that initial step.

### Actual UART runner execution

A bounded UART transport now allows host-side Robot to execute only
allowlisted VPP show commands on the physical node. Remote endpoint is
fixed to `/usr/bin/vppctl -s /run/vpp/cli.sock`; Python 3 is required.
It locks the UART, uses 115200 8N1, nonce-delimited JSON, complete-line
decoding, timeouts and a 1 MiB response bound. Mutating commands are
rejected before opening the device. This is not a general shell API;
the UART must not be shared with another operator. Successful command
output is retained in Robot logs.

```sh
python3 -m robot --pythonpath danos-test/danos-robot --exclude r3-only \
  --variable SERIAL_DEVICE:/dev/ttyUSB0 \
  --variable EXPECTED_MAC:00:1f:7a:69:f5:ec \
  --outputdir build/robot-r2-readonly-live \
  danos-test/danos-robot/VPP_READONLY.robot
```

R2 actual run passed 2/2 after required MAC setup, then passed again with
command output logging. Initial fragmented-response and ANSI-prefix
decoding failures were corrected and regression-tested. Reports are in
`build/robot-r2-readonly-live/`. This is live operational-read acceptance,
not forwarding, throughput or ISO qualification. No configuration changes
or packet-generator traffic were performed. Six adapter unit tests pass.

R3 actual UART full suite also passed 3/3 using adapter commit `81663a6`:
MAC `00:1f:7a:69:f7:4c`, runtime `v26.10-rc0~545-gad99177fe`, physical
counter output, and resolved ECMP buckets via 10.20.0.2/GigabitEthernet1/0/0
and 10.30.0.2/GigabitEthernet2/0/0. Evidence is in
`build/robot-r3-readonly-live/{output.xml,log.html,report.html,manifest.json}`.
R3 XML SHA256 is
`5fed9746fdbe42d200b53b85b1f99dffd1e889b3a7d2aa0d8351d0b6bc1b71c8`;
R2 XML SHA256 is
`959cf2fdbeb2185fb3a25a4c013b97e9ba73678732b3fdc31431150bb23fe160`.
The report manifest explicitly leaves installed ISO digest unknown; adapter
commit must not be substituted for the older installed image identity.
Remote `81663a6` CI/Coverage/Interop Gate runs 38150359546/38150359537/
38150359532 all completed success. These do not certify hardware performance.

### DPA contract and route evidence orchestration

Added `DPA_CONFORMANCE.robot`, original orchestration of existing acceptance
implementations rather than duplicate assertions or legacy commands. The
first three cases execute the current local conformance, programming pipeline
and composite route binaries, with explicit timeout and exit/output checks.
They use in-process/mock adapters, not R3's installed backend. Targets were
rebuilt before the passing run. The fourth invokes the existing learned-route
validator without `--run`, checks the clean archived topology/ISO identity
and ADD/WITHDRAW/RESTORE/POSTFRR evidence, and is tagged
`archived-evidence-only`. It neither modifies nor restarts a topology.

```sh
cmake --build build --target conformance_test programming_pipeline_test \
  route_composite_test -j 2
python3 -m robot --outputdir build/robot-dpa-conformance \
  danos-test/danos-robot/DPA_CONFORMANCE.robot
```

Actual Robot result 4/4 PASS; XML SHA256
`5cc0ec38ad4dd002a73e861e9b200265cda295f550d3859d92035bd7c5c04497`.
Reports are in `build/robot-dpa-conformance/`. Current CLI implements
interface/route/VRF views, not legacy `show platform dataplane`. Physical
DPA backend/object state visibility is still a gap; local conformance and
VPP FIB views must not be substituted for desired/programmed ledger views.
Next design a bounded read-only capability/ledger query with authenticated
management transport and explicit ownership, then integrate fresh isolated
route lifecycle orchestration. Do not enable plaintext mgrd on R3 for this.

### Minimal in-process summary — 2026-10-12

Implemented `show dataplane`: active programming backend, separately
registered capabilities, and desired/programmed counts for every DPA type.
TYPE is the public numeric object enumeration; MAX_COUNT=0 means unlimited.
Registration does not imply active selection. Counts are sampled independently,
not an atomic cross-store snapshot. Programmed means local ledger, not hardware
FIB readback. Unknown subarguments such as `clear` are rejected. The query
does not program, clear or enumerate individual objects.

CLI tests seed a desired route, unsupported registered capability and
separately named active backend, check output and unchanged desired/programmed
counts. Full build and CTest 67/67 PASS; updated DPA Robot suite 5/5 PASS.
Updated XML SHA256 is
`e97ecc42b9363d1cf9dda802711e89a99a945b1c385cee7cf840d9aa6c5e4562`;
the earlier 4/4 digest refers to its earlier report. This is local-source
implementation, not deployment to R3's old ISO. Per-object states,
authenticated remote query and physical image qualification remain pending.

### Targeted desired/ledger presence — 2026-10-12

Added `show dataplane object <numeric-type> <numeric-id>` and core API
`danos_programming_object_presence()`. One key is sampled, with programming
mutex and each store's read lock; no table enumeration, mutation, programming,
sweep or ledger initialization is requested by this API. Desired/ledger
reads are not cross-store atomic because CRUD has independent locks.
Invalid type, unsigned-ID overflow and extra arguments fail closed.

The output's `DESIRED` and `LEDGER_RECORDED` flags have deliberately limited
meaning: no/no is absent, yes/no is desired-only, yes/yes has a ledger
record (not necessarily current dependencies or hardware state), no/yes
has a retained record awaiting reconciliation. A recorded object cannot
be assigned to the current active backend merely from these flags.
No failure reason, retry state or per-object backend ownership is inferred.
Tests cover all four combinations and confirm no backend programming calls
occur during queries; test setup explicitly programs/deletes its mock route
to construct the latter states. These fixture mutations are not CLI query
behavior. Physical deployment and authenticated remote API remain pending.

### Sampled dependency and retry details — 2026-10-12

The targeted CLI query now also uses `danos_programming_object_status()`
to compare copied desired bytes and the current sampled dependency digest
with the ledger. `ledger_matches_sampled_desired=yes` means only this
in-process sample matches, not hardware readback. Registry mutations can
occur between independent reads; presence and detailed status are explicitly
separate samples. Programming/retry locks follow programming→retry order.
The query is for management callers, not callbacks already holding the
programming mutex.

Retry metadata now retains the programming-pass error code and exposes
attempts, remaining backoff milliseconds, flapping and exhaustion. It also
reports whether the retry record's desired/dependency digests match the
sampled candidate. A stale historical error must not be reported as the
current candidate's failure; no retry record is not proof of success.
The last error covers recorded programming failure, not every sweep/delete
failure. Policy flags are observed metadata, not commands to retry or reset.

Tests cover initial unprogrammed state, a matching ledger, changed desired
bytes, next-hop-only changes invalidating a route dependency digest, recorded
RETRY/backoff/exhaustion, stale retry metadata after mutation and deletion,
and unchanged mock backend call counts during queries. Full build,
CTest 67/67 and DPA Robot 5/5 PASS. Per-object backend ownership,
authenticated remote transport and deployment remain open.

### Read-only gNMI transport — 2026-10-12

Native targeted Get `/dataplane/object[type=N][id=K]/state` now exposes
the sampled object/dependency/retry state over the existing gNMI listener.
Real TLS 1.2/1.3 tests verify admin/operator/viewer reads and denial of
update/replace/delete on this operational path. Strict two-key numeric
validation and CONFIG exclusion are tested. This does not introduce a
new plaintext listener or production credentials. Full formal YANG binding,
STREAM notifications, backend ownership and physical image deployment
remain separate work; see `management-mtls.md` for field semantics.

### Recorded backend attribution — 2026-10-12

Private in-memory ledger records now store the backend name copied at
successful programming, separately from the active backend pointer.
CLI object status displays that recorded name; native gNMI state container
Get emits an additional `recorded-backend` string update, also readable
directly at `/state/recorded-backend`. Empty means unknown or absent, not
a guessed active backend; names longer than 63 bytes are not truncated
into misleading identities. No persistent WAL schema is changed.

Tests demonstrate recorded attribution survives an active-backend switch
and a desired delete until ledger reconciliation, then disappears when the
record is forgotten. This does not add dynamic migration or hardware
ownership readback. The ledger header/payload capacity is checked before
backend IO; an oversized raw registry blob fails rather than programming
without an attributable record. Current modeled programmable types fit.
TLS 1.2/1.3 tests read the owner leaf and reject forged leaf writes for
every role. Updated physical ISO deployment and production PKI remain open.

## Next sequence

1. Deploy this read-only suite to a qualified runner; retain Robot XML/HTML
   alongside ISO digest, embedded commit, runtime version and topology.
2. Adapt backend/object visibility to actual DANOS-Open capabilities and
   ledger views; never invent a legacy `show platform` command.
3. Wrap existing learned-route add/withdraw/restore and FRR/VPP recovery
   gates as orchestration steps, preserving their event and packet checks.
4. Translate upstream BGP objectives individually; mutable scenarios require
   isolated topology, explicit ownership, saved configuration and rollback.
5. Keep EVPN/MPLS/IPsec/firewall scope deferred until the release plan admits
   them. Robot orchestration does not resolve the VPP PG pacing fault or
   replace a real PCI performance runner.
