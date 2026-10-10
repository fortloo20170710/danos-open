# I211 physical LIVE traffic qualification runbook

更新时间：2026-10-04

## 2026-10-09 R3 installed-disk single-port smoke

Following the operator's installation PASS photo and reboot, serial inspection
confirmed root `/dev/sda1` ext4, FRR/VPP active and VPP API/stat sockets.
All four `8086:1539` I211 devices (01:00.0 through 04:00.0) were bound to
`uio_pci_generic`; VPP Intel e1000 PMD exposed four polling ports. This is
runtime observation, not an independent on-device verification of ISO digest.

Temporary admin-up probing identified `GigabitEthernet1/0/0` / 01:00.0 /
MAC `00:1f:7a:69:f7:4c` as the connected 1 Gbps full-duplex link to development
host `enp4s0`. The other three ports had no carrier. A temporary isolated
subnet used VPP `198.18.0.1/30` and host `198.18.0.2/30`:

- Host to VPP: 5 transmitted, 5 received, 0% loss; RTT min/avg/max
  0.108/0.199/0.489 ms.
- VPP to host: 5 sent, 5 received, 0% loss.
- VPP port snapshot included 226 RX, 11 TX and 215 drops; other host traffic
  was present, so these are not isolated per-probe deltas or a zero-drop claim.

After testing, the exact temporary addresses were removed and all VPP ports
restored to their original admin-down state. Host NetworkManager applied its
own shared address while the link was up and cleared addresses on link-down;
no persistent host network profile or wireless management setting was edited.
VPP deletion syntax used for cleanup is
`set interface ip address del GigabitEthernet1/0/0 198.18.0.1/30`.

Scope: physical single-port local ICMP RX/TX smoke PASS. This does **not**
qualify ingress-to-egress forwarding, ECMP, FRR dynamic routes or line-rate
performance. Two independent links/peers are still required for the topology
below. Observations were interactive serial checks, not an identity-bound
full cold-boot traffic capture; do not feed this note into the strict gate as
full hardware traffic PASS.

## 拓扑

### Next physical ECMP topology (not yet qualified)

2026-10-10 R4 preparation: UART verified MAC `00:1f:7a:40:01:80`,
P1 `10.20.0.2/24` and existing return route to `10.10.0.2/32`
via `10.20.0.1`. Created `loop0`, admin-up, with shared local addresses
`30.30.30.2/32` through `30.30.30.5/32`, matching R1. R4→R3→R2
regression reports 1000/1000, 0% loss, TTL 63 in VPP's summary.
The UART transcript contains 1000 reply lines, but the strict endpoint
verifier rejects its reply-line formatting/count match; this is summary-level
evidence only, not a strict archived-log PASS. No reboot or persistent
configuration change; R3's two-next-hop route and bucket distribution
remain unqualified. Next move UART to R3 to install/inspect ECMP.
Evidence `build/dpdk-installable/r4-shared-ecmp-prep-20261010.serial.log`
SHA256 `d225d96cecdad7bdbf91578c82870f1b87d18b5ffe009ac36af3afb570c545c7`.

2026-10-10 R2 follow-up: UART identity MAC `00:1f:7a:69:f5:ec`
confirmed that its FIB lacked the R1 return route. Added
`10.30.0.2/32 via 10.10.0.1 GigabitEthernet1/0/0` and the planned
`30.30.30.0/24` route through the same next hop. R2→R3→R1 then passed
1000 sent/1000 received, 0% loss, replies TTL 63. This validates the
second transit leg, not ECMP distribution or a repeat of R1-originated traffic.
Local evidence `build/dpdk-installable/r2-r1-return-route-20261010.serial.log`
SHA256 `1b093e6d91e7a09284dc59add1b840cb659b5127982dca23be6a853f99066f01`.
Configurations remain temporary VPP CLI state; no reboot was performed.
Next prepare R4's shared loopback destinations, then program and measure
R3's two-next-hop ECMP route.

Operator subsequently installed/booted R1 and connected R1.P1 ↔ R3.P2.
UART confirms R1 root `/dev/sda1` ext4 and active VPP; port MACs are
`00:1f:7a:40:28:20` through `:23`. All four ports were temporarily set
admin-up but no carrier was observed while R3's new port remained unverified.
No physical-port IP or return route is assigned before mapping is confirmed.
R1's newly created `loop0` is up with 30.30.30.2–.5 as /32 addresses;
this is preparation only, not peer reachability or ECMP PASS.
Local preparation transcript: `build/dpdk-installable/r1-ecmp-peer-preparation.serial.log`.
Next required action is enabling the candidate R3 port and confirming the
R1 carrier/MAC mapping, then assigning 10.30.0.2/24 and its return route.

2026-10-10 update: R3 port2 is enabled, 1 Gbps full duplex and configured
10.30.0.1/24. R1.P1 is confirmed as GigabitEthernet1/0/0, MAC
00:1f:7a:40:28:20, 1 Gbps full duplex; configured 10.30.0.2/24 and
10.10.0.2/32 via 10.30.0.1. Other R1 physical ports restored admin-down.
R1→R3 direct check passes 100/100. R1→R2 check returns 0/100 and is not
accepted as a functioning end-to-end path. R2's previously recorded route
only covers 10.20.0.2/32; a return route to 10.30.0.2 (and the planned shared
ECMP destinations) is still to be verified/configured on R2. Missing return
routing is a working explanation, not an independently proven root cause.
Local transcript: `build/dpdk-installable/r1-path-b-test-20261010.serial.log`.
No ECMP route is installed on R3 at this checkpoint. All new entries remain
temporary; no reboot, persistence or DPA programming PASS is claimed.

Keep R2.P1 ↔ R3.P4 (ingress) and R4.P1 ↔ R3.P1 (egress path A).
Install/boot r12 on independent R1 and add R1.P1 ↔ R3.P2 as egress path B;
identify actual VPP port mappings by MAC/link probing before assigning IPs.
Use 10.30.0.1/24 on R3's new link and 10.30.0.2/24 on R1's peer.
R1 and R4 must both locally answer the shared /32 destinations
30.30.30.2–30.30.30.5, and have a return route to R2 10.10.0.2/32 via
their respective R3 next hop. R2 reaches those destinations via 10.10.0.1;
R3 uses equal-weight next hops 10.20.0.2 and 10.30.0.2. These are proposed
test entries, not currently applied configuration.

Acceptance order: independent neighbor reachability → two resolved FIB
buckets → multiple flows with positive counters on both paths → next-hop
withdraw and restore with bounded loss → endpoint-complete logs and counter
windows. Add destinations/flows if initial hashes use only one path; never
declare ECMP from merely configured next hops or one successful ping.
No restart is authorized by this topology note. Persist/replay validation
requires a separately controlled configuration/restore procedure; CLI-only
test entries cannot be claimed to survive VPP restart automatically.

The archived 60000-probe single-path endpoint log is independently checkable:

```sh
python3 danos-test/integration/verify_physical_ping_log.py \
  build/dpdk-installable/r2-r4-60000-full.log --target 10.20.0.2 --packets 60000
```

The verifier rejects incomplete summaries, reply count disagreement,
duplicate/out-of-range sequences, unexpected endpoints/TTL and failure text.
Its scoped PASS explicitly leaves ISO identity, measured run duration, ECMP,
restart and line-rate qualification unverified. It does not replace the
existing strict I211 traffic/performance gate.

### 2026-10-09 installed R3/R4 two-link functional test

Operator wiring: development host `enp4s0` to R3.P4; R3.P1 to R4.P1.
MAC/link checks identify R3.P4 as `GigabitEthernet4/0/0`
(`00:1f:7a:69:f7:4f`) and R4.P1 as `GigabitEthernet1/0/0`
(`00:1f:7a:40:01:80`). R3.P1 is `GigabitEthernet1/0/0`.
Both links negotiate 1 Gbps full duplex. Temporary configuration:

| Node | Interface | Address / route |
|---|---|---|
| Host | enp4s0 | 10.10.0.2/24; 10.20.0.2/32 via 10.10.0.1 |
| R3 | GigabitEthernet4/0/0 | 10.10.0.1/24 |
| R3 | GigabitEthernet1/0/0 | 10.20.0.1/24 |
| R4 | GigabitEthernet1/0/0 | 10.20.0.2/24; 10.10.0.2/32 via 10.20.0.1 |

Other VPP ports are admin-down. Host Wi-Fi default route remains unchanged.
R4 directly pings R3 5/5; host-to-R4 and R4-to-host each pass 10/10 with
TTL 63 and zero loss. A host-to-R4 short run passes 100/100, zero loss,
10293 ms elapsed, RTT min/avg/max/mdev 0.135/0.214/0.297/0.024 ms.
This is physical two-port IPv4 forwarding functionality, not line-rate,
ECMP or FRR/ZAPI/DPA route programming qualification: configuration was
applied directly via VPP CLI. Interactive checks lack a complete identity-bound
cold-boot capture and must not be substituted into the strict release gate.

Configuration is retained temporarily for subsequent tests, not persisted;
reboot loses the VPP CLI configuration. To remove only these test entries:
delete the host's exact 10.20.0.2/32 route and 10.10.0.2/24 address, delete
R4's exact 10.10.0.2/32 VPP route via 10.20.0.1, and remove each listed VPP
address using `set interface ip address del <interface> <address/prefix>`.
Restore previously down test interfaces after removing addresses/routes;
do not flush unrelated host routes or NetworkManager settings.

### Subsequent short-soak results: anomaly retained

### Independent R2 peer follow-up

Operator installed r12 on R2, booted from disk and replaced the development
host link with R2.P1 ↔ R3.P4; R4.P1 ↔ R3.P1 remains unchanged.
Serial identity check shows R2 root `/dev/sda1` ext4 and active VPP.
R2.P1 maps to `GigabitEthernet1/0/0`, MAC `00:1f:7a:69:f5:ec`, link
1 Gbps full duplex. Temporary CLI configuration: 10.10.0.2/24, route
10.20.0.2/32 via 10.10.0.1 on that interface; other R2 ports admin-down.
No disk, driver binding or persistent configuration was changed.

Direct R2→R3 passes 5/5; R2→R3→R4 passes 10/10, TTL 63.
VPP CLI `ping 10.20.0.2 source GigabitEthernet1/0/0 interval 0.01 repeat 1000`
passes 1000 sent/1000 received, 0% loss. R2 learns the correct R3.P4 MAC
dynamically. Local transcript `build/dpdk-installable/r2-r3-r4-1000.serial.log`
SHA256: `6c6256e603786c4eca0a807bb785d0ef9bb0a2613a3ad5b5bb495019cbd2d0d6`.
This is an isolated-peer short functional baseline, not proof that the earlier
host anomaly is fixed, not long-duration stability or line-rate/ECMP acceptance.
Reverse initiation from R4 in this new topology and R3 counter deltas remain
to be collected. R2/R3/R4 temporary test configuration is retained; no reboot.

Reverse initiation is subsequently verified after switching UART to R4:
MAC `00:1f:7a:40:01:80`, port1 at 1 Gbps full duplex, 10.20.0.2/24,
10.10.0.2/32 resolved via 10.20.0.1. Command
`ping 10.10.0.2 source GigabitEthernet1/0/0 interval 0.01 repeat 1000`
passes 1000/1000 with TTL 63 and 0% loss. R4 port RX/TX each increases
10110→11110 (+1000); drops remain 1. Dynamic R3 neighbor MAC is correct.
Local transcript `build/dpdk-installable/r4-r3-r2-1000.serial.log` SHA256:
`ac3f8e06ed0881b400991eb0632822fabd17fd4459281921766b3069377f9206`.
Thus both initiation directions have an isolated three-node short baseline.
R3 counter-window evidence, longer soak, restart recovery and ECMP remain open;
the previous development-host anomaly is still retained separately.

R3 UART follow-up confirms both links remain up and learned neighbors now
match R2 `00:1f:7a:69:f5:ec` and R4 `00:1f:7a:40:01:80` (not the old host
MAC). Separate R3-originated 1000-probe runs to 10.10.0.2 from port4 and
10.20.0.2 from port1 both pass 1000/1000, zero loss. Port drops remain
880 (port4) and 215 (port1) across that window. Local transcript
`build/dpdk-installable/r3-two-peer-1000.serial.log` SHA256:
`aa2b0d10e58de3104b27d304e169c4b019cbe34cd8f1d06cd817308c6d67dd49`.
These are local-output/local-input checks, **not** a transit forwarding window.
For a longer transit run while UART remains on R3, initiate traffic at R2's
physical keyboard (or first arrange a controlled endpoint session); a single
UART cannot directly command all three nodes concurrently.

### R2-initiated 60000-probe transit observation (endpoint verified)

Operator reports launching on R2:
`vppctl -s /run/vpp/cli.sock ping 10.20.0.2 source GigabitEthernet1/0/0 interval 0.01 repeat 60000 > /tmp/r2-r4-soak.log 2>&1 &`.
With UART on R3, snapshots show ongoing traffic then three unchanged samples.
The continuous capture spans R3 timestamps 11:08:09–11:16:37 UTC; collection
started after endpoint traffic began and therefore is not a complete run timer.

| R3 port | Pre-launch recorded RX/TX | Quiescent RX/TX | RX/TX delta | Drops before/after |
|---|---|---|---|---|
| GigabitEthernet1/0/0 (R4 side) | 12336 / 12121 | 72336 / 72121 | +60000 / +60000 | 215 / 215 |
| GigabitEthernet4/0/0 (R2 side) | 13304 / 12424 | 73304 / 72424 | +60000 / +60000 | 880 / 880 |

Snapshot windows show matched RX/TX progress around 100 probes/s; learned
neighbors remain R2/R4 MACs. During capture, DPDK counter remains 22, ARP
source/destination rejection counters remain 682/168 and interface-down
counter remains 4. No counters were reset or static neighbors introduced.
The UART observer exits `WINDOW_QUIESCENT`, not PASS; the separately collected
endpoint verdict below closes sent/received/loss and log completion. Actual
full-run elapsed time was not recorded. Do not infer end-to-end zero loss
solely from R3 counters.
This modest-rate extended test is not line-rate, ECMP or restart qualification.

Local evidence:

- `r3-transit-soak-window.serial.log` SHA256
  `032e9fb70668ede79eca8eef62518a3e52aa477adaf2116585d4289617934142`.
- `r3-transit-soak-continuous.serial.log` SHA256
  `15cb2d6de141dde1c2012d39831bc18971c8050f4b1e04118ad048433ee08ff3`.
- Reusable observer: `danos-test/integration/capture_vpp_transit_window.py`,
  exclusive output creation, bounded capture, incomplete-marker rejection and
  explicit quiescence-vs-endpoint acceptance separation. Parser tests pass.

After UART was moved to R2, MAC `00:1f:7a:69:f5:ec` and 1 Gbps link confirm
the endpoint. `/tmp/r2-r4-soak.log` reports 60000 sent, 60000 received,
0% packet loss; no matching ping process remains. An independent awk check
finds exactly 60000 responses, missing=0, duplicate=0, unexpected_ttl=0
(all TTL 63). The 3,769,042-byte original log was gzip/base64 transferred
over UART and decoded locally; the original and local SHA256 agree:
`92af013aabd640541a1c7da965fb989f73684dd76477bdae945b373fd6ed9ff3`.
Local full log: `build/dpdk-installable/r2-r4-60000-full.log`.
The first decoding attempt missed the ANSI-prefixed BEGIN marker; extraction
was corrected and the entire decoded file verified, not accepted by mere tail.

Endpoint evidence:

- `r2-60000-endpoint-verdict.serial.log` SHA256
  `0259e0b938bc1fd0869b46ab027815eec1467d9122cfcb766a7ab8c0b75c93f7`.
- `r2-60000-sequence-check.serial.log` SHA256
  `58b715c2fc4ab57869a1e7cecffce7e4ef03c97a1b0b098dda6f30cec2e52c2f`.

Verdict: this scoped isolated three-node 60000-probe IPv4 transit test PASS.
Nominal interval is 0.01 seconds, not measured wire throughput. It does not
prove 24-hour stability, ECMP, restart recovery, FRR/DPA-driven programming,
or repair of the previous development-host anomaly. ISO r12 identity remains
operator-reported, not an independently observed physical cold-boot digest.

R3-side follow-up after the serial cable was moved back: both physical links
remain 1 Gbps full duplex; learned neighbors match host/R4 MACs. Another
3000/3000 run passes (33261 ms, RTT avg 0.185 ms), followed by 1000/1000
(11094 ms, RTT avg 0.194 ms). Targeted host capture shows unicast ARP
`who-has 10.10.0.1 tell 10.10.0.2` and R3's matching reply; no capture drops.
This confirms working NUD in that window, not the cause of the earlier loss.
R3 `dpdk-input` counter remains 22, `interface is down` remains 4;
ARP source/destination rejection counters increase 602→642 and 152→160
while unrelated host ARP traffic is present. Do not interpret the DPDK
counter label `no error` as 22 newly failed packets.

Host enp4s0 also has NetworkManager address 192.168.71.1 and pre-existing
routes via 192.168.71.2. These were not removed: the test interface is not
an isolated peer. Background ARP for Internet destinations was captured,
but it does not establish why the initial first-hop neighbor resolution failed.
Next stability qualification should use a dedicated isolated peer or an
explicitly coordinated temporary network profile, preserving existing routes.
No static neighbors were added to conceal the dynamic-ARP anomaly.

Additional local evidence SHA256:

- `r3-diag-3000-ping.log`: `1d957a8cde9daaf02751f3e3b5ef9e216e5d2b80ff6693c84815f7e6f2ca43e7`
- `r3-arp-probe-ping.log`: `5c7741c22c08bf37553a78924d4fa6bbb66d5f9c7826031690896816d0481378`
- `r3-target-arp.log`: `5b3ce423f0add71e9dd811ad5b3e02052483375c0f0d5f431a5695e82b819332`

At approximately 100 requested probes/s (`ping -i 0.01`), first host-to-R4
1000-probe run received 972/1000, 2.8% loss, 15 local unreachable errors,
21832 ms elapsed. R4 RX/TX each rose from 126 to 1100 (+974), while drops
stayed at 1. This run is FAIL against a zero-loss baseline; not a throughput
or line-rate measurement. Host-origin `Destination Host Unreachable` around
sequences 456–476 suggests a first-hop neighbor-resolution problem, but
without simultaneous R3 trace/ARP capture the cause is unproven.

No configuration change was made before subsequent probes: direct R3
300/300 passed; host-to-R4 repeat 1000/1000 passed (11104 ms,
RTT avg 0.189 ms); another 3000/3000 passed (33195 ms, RTT min/avg/max
0.101/0.188/1.469 ms). During the final run, timestamped host neighbor
monitor observed PROBE → REACHABLE transitions for 10.10.0.1, no FAILED
entry. Monitor timeout rc=124 is the expected bounded observation end.
Later PASS runs do not erase the earlier anomaly; hardware stability closeout
remains open pending first-hop diagnostics and a longer identity-bound run.

Logs under `build/dpdk-installable/` (local artifacts, not repository blobs):

| Log | SHA256 |
|---|---|
| r3-r4-1000-ping.log | a2bcc69be042c7f68132d5902211251cb1026c4c3a10a4d8673090986cc0bf05 |
| r3-r4-1000-retest-ping.log | 901eedbebf0dfdca4e742156cf7f32fb42c6f7c1d65a4a21aa04b99f7c163c5f |
| r3-r4-3000-ping.log | 7298ba36a6e70275f92059d513c0efa15edba78674819744806f00c3236e7b9f |
| r3-r4-neighbor-monitor.log | 38377cc6d3808faac2a97fe8663bc7aa663ab4f322856b1f8a90e3e084b4f3b6 |

使用两条独立链路，每条链路各有一个 Linux peer（或两个相互隔离的 Linux network
namespace）。不要把两个 peer 网段桥接到同一个 L2。DANOS traffic ISO 默认配置为：

| Link | I211/VPP side | Peer side | Return route on peer |
|---|---|---|---|
| 1 | `10.10.0.1/24` | `10.10.0.2/24` | `30.30.30.1/32 via 10.10.0.1` |
| 2 | `10.20.0.1/24` | `10.20.0.2/24` | `30.30.30.1/32 via 10.20.0.1` |

两侧 peer 都要在其直连测试口上配置 `30.30.30.2–30.30.30.5`，这样 ECMP 无论选中哪条
路径，四个目标地址都可应答。回程 host route 是必需项；QEMU FRR peer fixture 也显式配置
了相同的 `30.30.30.1/32` 回程路由。

## Linux peer 配置

在 peer 1 上，将 `<IF1>` 替换成连接 DANOS 第一口的专用网卡名；在 peer 2 上，将 `<IF2>`
替换成连接 DANOS 第二口的专用网卡名。确认接口名和线缆后再执行；命令只改指定接口，使用
前应确保它不是 SSH/管理链路或生产接口。

Peer 1：

```sh
IF=<IF1>
sudo ip link set "$IF" up
sudo ip address replace 10.10.0.2/24 dev "$IF"
for address in 30.30.30.2 30.30.30.3 30.30.30.4 30.30.30.5; do
  sudo ip address replace "$address/24" dev "$IF"
done
sudo ip route replace 30.30.30.1/32 via 10.10.0.1 dev "$IF"
sudo sysctl -w net.ipv4.conf.all.rp_filter=0
sudo sysctl -w "net.ipv4.conf.$IF.rp_filter=0"
ping -c 3 -I 10.10.0.2 10.10.0.1
ip route get 30.30.30.1
```

Peer 2：

```sh
IF=<IF2>
sudo ip link set "$IF" up
sudo ip address replace 10.20.0.2/24 dev "$IF"
for address in 30.30.30.2 30.30.30.3 30.30.30.4 30.30.30.5; do
  sudo ip address replace "$address/24" dev "$IF"
done
sudo ip route replace 30.30.30.1/32 via 10.20.0.1 dev "$IF"
sudo sysctl -w net.ipv4.conf.all.rp_filter=0
sudo sysctl -w "net.ipv4.conf.$IF.rp_filter=0"
ping -c 3 -I 10.20.0.2 10.20.0.1
ip route get 30.30.30.1
```

Both direct pings must pass, and each `ip route get` must show the expected interface and gateway.
If the peers are firewalled, allow ICMP echo request/reply on the dedicated test interfaces.

## Capture and acceptance

Start serial capture **before** powering on/cold-booting the target so the log includes
`DANOS-INIT-ENTER`, `DANOS-BUILD`, PCI binding and test markers. Use the exact traffic ISO path
whose profile was verified by the capture script:

```sh
sudo bash danos-test/integration/capture_i211_serial.sh /dev/ttyUSB0 \
  build/danos-open-v0.16.0-rc1-i211-dpdk-traffic-runner-r6.iso \
  build/i211-traffic-r6.serial.log 180 --traffic
```

Functional PASS requires both direct peer pings, at least two resolved ECMP buckets, all four
loss-free destination probes, the 4000-packet soak, and next-hop withdraw/failover/restore. The
result is not a line-rate performance qualification; that remains a separate PCI/traffic-generator
lane. A partial UART capture with explicit failure markers is recorded as `FAIL` with
`identity_status=UNVERIFIED`; no boot or failure markers yields `SKIP`.

## Current observation

The 2026-10-04 field capture showed an ECMP route with two configured next hops but only one
forwarding bucket; all four destination probes received zero replies and second-path recovery
failed. This is consistent with unresolved peer adjacency and/or missing peer return routing, but
the partial serial capture cannot distinguish the exact cause. Configure and verify both peers
above, then repeat with a cold-start capture before changing DPDK binding or VPP settings.
