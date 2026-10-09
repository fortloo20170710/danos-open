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
