# VPP 26.10 control runtime gate

Use only an isolated project runtime with loopbacks at sw_if_index 1 and 2.
This test changes interface flags, table 777 and prefix 198.51.100.0/24.
Do not run it against production or another assistant's runtime.

Start the project image with --network none and --user UID:GID, mounting a
new private (0700) directory as /run/danos-vpp and
danos-test/integration/vpp-control-runtime.conf read-only as its startup config.
DPDK is disabled. No PCI, host interfaces, hugepages configuration or serial
access is required. Create two loopbacks via vppctl in that dedicated container.

Then run:

```sh
python3 danos-test/integration/verify_vpp_control_runtime.py \
  --container danos-open-control-20261007-r2 \
  --socket-dir build/vpp-control-20261007-r2 \
  --binary build/danos-test/vpp_live_test \
  --output build/vpp-control-new-evidence
```

Output directories cannot be reused. The gate captures runtime image/version,
source commit/dirty flag, binary SHA256, command return codes and log hashes.
API phases require handshake, control_ping, interface up and actual stats socket
connection. It checks API-origin FIB presence and path-list count after add,
repeat, 2→1 withdraw and 1→2 restore, then route absence after delete for tables
0 and 777. It also verifies table 777's named FIB disappears after deletion.
The two-loopback prerequisites must remain under this gate's exclusive control.
On failure preserve the logs and stop that isolated runtime; do not guess PASS.

## 2026-10-07 evidence

build/vpp-control-gate-20261007-r4/manifest.json is PASS against project image
danos-vpp-runtime-recover:local, VPP v26.10-rc0~545-gad99177fe (839 messages).
The build included dirty wire-layout fixes atop da2bec6; the manifest fixes the
binary digest. Earlier failed runs are retained separately and not promoted.

Two real runtime errors escaped mocks: interface flags were encoded as legacy
u8 rather than the current enum, and ip_table.name[64] used dynamic-string
encoding instead of the fixed array. Runtime logs reported message lengths
15 versus 18 and 30 versus 80 respectively. Corrected bodies are 8 and 70 bytes.
Golden tests now pin those target-runtime layouts, including short-buffer refusal.

Upstream definitions:
[interface.api](https://github.com/FDio/vpp/blob/master/src/vnet/interface.api)
and [ip.api](https://github.com/FDio/vpp/blob/master/src/vnet/ip/ip.api).
This targets the declared VPP 26.10 runtime, not universal compatibility with
legacy VPP APIs. Different CRC/layout versions require explicit qualification.

This is real API/FIB evidence, not packet forwarding, FRR→DPA integration,
dataplane restart/reconnect, ECMP traffic distribution or PCI performance.
Those remain separate required gates. Stop only the dedicated containers when
finished; preserve evidence directories.
