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
