# Read-only VPP runtime evidence

The collector captures CLI state, not packets or binary API frames. It never
creates routes/interfaces, clears counters, restarts VPP, configures hugepages,
binds PCI devices, opens serial ports or starts containers.

```sh
python3 danos-test/integration/capture_vpp_runtime.py \
  --output build/vpp-capture-before --label before \
  --cli-socket /run/vpp/cli.sock

# An existing, authorized container; no container is created:
python3 danos-test/integration/capture_vpp_runtime.py \
  --output build/vpp-capture-added --label route-added \
  --container danos-vpp-runtime-recover \
  --evidence build/runtime-live-test.log
```

Use a new directory for each phase: before, route-added, NH-withdrawn,
NH-restored, route-deleted and post-restart. Apply changes only through existing
authorized topology/backend tests. Existing output directories are refused.
--vppctl selects the executable (inside the container if selected).
--iso optionally copies/hashes the exact project ISO. Repeated --evidence
attaches existing logs, API schemas or independently recorded packet/wire
captures; no attachment is executed. Avoid credentials in logs. Output directory
permissions are 0700.

The fixed query set collects version, interface counters, hardware, IPv4/IPv6
FIB, load-balance buckets, errors, runtime and threads. These are
[FD.io show commands](https://docs.fd.io/vpp/19.04/clicmd.html); actual runtime
availability is checked and unsupported-command output retained.
Timeout is per command (default 10 seconds, maximum 60).

manifest.json records schema, UTC time, collector host, source commit/dirty
state, target socket/container, command argv/returncode/timeout/duration and
SHA256 for raw stdout/stderr and attachments. Collector identity does not prove
runtime provenance; attach the exact boot/build manifest and ISO digest.

- CAPTURED, exit 0: queries completed without detected errors.
- ERROR, exit 1: command failure, timeout or recognized CLI error.
- SKIP, exit 77: required local executable/socket unavailable.
- Invalid arguments/overwrite refusal: exit 2.

Every result has acceptance=NOT_EVALUATED. CAPTURED is **not PASS**. Sequential
queries are not an atomic snapshot. Compare the raw outputs with intended
route/table/NH and controlled traffic expectations; FIB presence alone is not
forwarding evidence, nor do counters alone establish ECMP flow distribution.

P1-3 remains open until real FIB add/delete, VRF tables, NH withdraw/re-add and
FRR/VPP topology recovery are verified. Missing-runtime SKIP and fake CLI tests
do not qualify. Binary encoder golden evidence still needs upstream schemas and
independently captured API frames (item 11).

run_vpp_verify.sh now delegates capture/all here. Old perf/conformance/e2e and
socket-only acceptance modes are refused: the software baseline tested Linux,
not VPP/DPDK, and mock success/socket existence never proved runtime acceptance.
Use dedicated topology/DPDK lanes instead.

vpp_runtime_capture is a deterministic fake-CLI tool test covering exact argv,
hashes, missing runtime, overwrite rejection, zero-exit CLI errors and
process-group timeout cleanup. It is not a real VPP test.
