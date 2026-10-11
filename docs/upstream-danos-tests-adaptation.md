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
runner Robot execution or configuration change was performed in this step.

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
