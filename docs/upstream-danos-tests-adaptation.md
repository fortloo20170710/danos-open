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
