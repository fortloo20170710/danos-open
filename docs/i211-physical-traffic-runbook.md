# I211 physical LIVE traffic qualification runbook

更新时间：2026-10-14

## 2026-10-14 shared-router management link (scoped PASS)

The operator reports each R1–R4 third I211 port and the development host are
connected to one five-port router. Host `enp4s0` has `192.168.1.243/24`;
gateway is `192.168.1.1`. With the USB-UART identifying R2 r9, P3 was mapped
to VPP `GigabitEthernet3/0/0`, PCI `0000:03:00.0`, MAC
`00:1f:7a:69:f5:ee`. It was assigned runtime address `192.168.1.244/24` and
set administratively up. Carrier came up at 1 Gbps/full duplex. Host→R2 ping
passed 2/2; R2→host VPP ping passed 5/5; host neighbor MAC matched R2 P3.

This is a bounded shared-L2 management reachability PASS only. The VPP address
is not persisted across VPP restart/reboot. R1/R3/R4 third-port state and
addresses are not yet verified, and no throughput, latency, or ECMP claim is
made. Keep the established R2→R3→R1/R4 L3 test links/VLANs separate; do not
flatten or rewire them as part of the management LAN. Host RTL8168/r8169 is not
a DPDK traffic source; continue to use the I211 packet-generator and
independent receiver counters for functional transit, with a qualified PCI
runner still needed for performance.

## Four-node tightly bracketed physical UDP window — 2026-10-13 (PASS)

A fresh snapshot sequence completed within the verifier's 1800s per-node
limit: R3 180.01s, R1 344.23s, R4 291.39s. R2 generated 3072 64-byte frames;
its I211 TX-good delta was 3072 and TX drops did not increase. R3 P4 RX was
+3072, P1/P2 TX were +2304/+768, and drops were unchanged. Independent receiver
RX was R4 +2304 and R1 +768, exactly matching the corresponding R3 egress
counters and totaling 3072. Stable R3 VPP PID and the resolved two-path FIB
were verified. This is a bounded functional forwarding/delivery PASS, not an
ECMP statistical-balance or performance qualification. R3 CPU averaged
99.75% of one logical CPU across the 180s interval, not phase-aligned; measured
phase rates include CLI/control overhead. Latency, paired forwarding CPU,
sustained performance, and line rate remain open.

Artifacts under `build/physical-udp64-tight2-20261013/`; SHA256:

| Evidence | SHA256 |
| --- | --- |
| `r2-source/result.json` | `03ce6a4e46b71f6fd0cc2f5bf0b80664579be94ab84f24ff0cf7b230174db333` |
| `r3-before/result.json` | `b3f1a9df89644969953cf595d7b6e39d269fb639830da307ba2aff9bda86b790` |
| `r3-after/result.json` | `532e27aeaa7119273436215bea6c356813009753483bc7a9eb26534ec5f1c2b6` |
| `r1-before/result.json` | `544c0e8bf8d33b0a91a60100c910503fe117e2ea607d7af25a76b08365ab5740` |
| `r1-after/result.json` | `08d80352fd7c38410bca38b1e598c0fa10270ed84f60897ed3532907cd3038c1` |
| `r4-before/result.json` | `ce0c56dbd6fae652b0fe494a89f2b1f899f588f0d4d3c115b6a4efa7346763df` |
| `r4-after/result.json` | `526c8654d0f6a752423baac5858a7ba78d50fe46d528159536c3449775da258a` |
| `end-to-end-window.json` | `28082da1ce1b16340be8bfa80c062e850ec018a625a347f030e9c9d7dd761897` |

## Repeated four-node UDP window — 2026-10-13 (OBSERVED)

A follow-up single-pass run sent 3072 finite 64-byte frames from R2. The R2
I211 TX-good delta was exactly 3072 and TX drops did not increase. R3 P4 RX was
3072; ECMP P1/P2 TX were 2304/768; R1/R4 independently received 768/2304;
the measured R3 drop deltas were zero and the R3 VPP PID remained 3272. Thus
the endpoint and forwarding counters agree for the batch. The verifier reports
`OBSERVED`, not `PASS`, because the snapshot intervals were R3 3089.77s, R1
4164.02s, and R4 2659.66s, all beyond the 1800s same-window limit. This is
supporting counter correlation only; it does not replace the tightly bracketed
PASS above and does not qualify performance or statistical ECMP balance.

The preceding 49152-frame diagnostic batch was rejected because the R3
before/after interval also enclosed earlier sender retries; its P4 delta did
not equal that batch's source count. No traffic result is inferred from that
failed correlation. To avoid recurrence, the next physical run should be a
single bounded send between fresh snapshots, with serial changes performed
continuously and without unrelated retries or long pauses.

Artifacts under `build/physical-udp64-count256-20261013-batch2/`; SHA256:

| Evidence | SHA256 |
| --- | --- |
| `r2-source/result.json` | `672c2aa16ef1781fd9903f1da0e9bf2658ed45193e1cd4503605e98fb1c611a3` |
| `r3-before/result.json` | `a1018615a6fbba0f063502d8699ecd0677a71d9851aca316e9011ccf2e107ebd` |
| `r3-after/result.json` | `8960e0ddf8773dd117d5473bd817f62fd76779f8f0fd351e7b1fcf213cdbd57e` |
| `r1-before/result.json` | `6c33b90a017d08d84c91ae71e37411e6f801a9e96efe6e6e368c04013e6e515f` |
| `r1-after-retry1/result.json` | `9631fc73bc881fa6e2f2b31042b8d42b25ac4cd3720f3e18619c8238cf5a36e6` |
| `r4-before-retry1/result.json` | `fd70d19f6c9a3bb2d22293f4bbfdb7754133fa9dd9b18370c345db86b2e0545c` |
| `r4-after/result.json` | `0ce5a3046d128ed4ee87534d471d6086d5e44c7910206cfa6fc7764a2dd759ff` |
| `end-to-end-window.json` | `c99445e9ada4d35de04c580504ad77bcf0ac9b68f3c105262c7ccbb420ca343c` |

## Repeated four-node physical UDP window — 2026-10-13 (OBSERVED)

A new single-pass 3072-frame run again produced matching counters: R2 I211 TX
+3072, R3 P4 RX +3072, R3 P1/P2 TX +2304/+768, and independent R1/R4 RX
+768/+2304. R3 PID remained 3272 and its measured drop deltas were zero. The
R3 before/after interval was 256.77s, but receiver windows were R1 3441.18s
and R4 4908.19s. Since the receiver windows exceed the 1800s gate, the overall
result remains `OBSERVED`, despite exact counter agreement. Performance remains
`ENVIRONMENT-OPEN`; this is not a line-rate or ECMP-balance result.

Artifacts under `build/physical-udp64-count256-20261014-tight3/`; SHA256:

| Evidence | SHA256 |
| --- | --- |
| `r2-source/result.json` | `ccca687f28ae6aa892859333f0a4b7ce7e3e0e4e76eea41ac86687c0f5505b94` |
| `r3-before/result.json` | `bc8d67e31d8e9b7a8798e76140c548ef9d47e51544b5e60c36cecd3d137e1aa4` |
| `r3-after/result.json` | `20475151da12035026cc2600bf7bbeb794f6b91d8734214e0d76b8aa5e754a20` |
| `r1-before/result.json` | `0e68160486f4016f077fe573c1bce329e60b300da210eddeedff396d8d6bccd7` |
| `r1-after/result.json` | `0b48fec6077171e1b06a9831c95c2e0799542b67724413f23663aa480c69e472` |
| `r4-before/result.json` | `856734ff490a6810dec3821324ccba62bb5f6a154166f03b2075a1b3164ff9ef` |
| `r4-after/result.json` | `0e833df3283f90a88828f1dbf16e64d0005b8f33da8c446f6b51fdaacd4aeff9` |
| `end-to-end-window.json` | `3df36774f51817829569eccd5c2eee256c71a43485d455ac5e7de195f98080c9` |

## UART RPC long-payload compatibility and earlier wide-window replay — 2026-10-13

The first R3/R4 counter-snapshot attempts exposed two physical-console
constraints: multiline `python3 -c` source was interpreted as shell input, and
the receiver RPC exceeded the tty's single-line input capacity. The read-only
UART transport now sends Base64 source in short shell-variable chunks, aborts
any stale continuation prompt with Ctrl-C, and executes one compact bootstrap;
it creates no remote file. Fifteen UART adapter tests and CTest 76/76 pass.
R3 before/after and R4 after then passed against the live devices.

The earlier wide-window UDP batch generated 3072 frames on R2, with an exact R2
I211 TX-good delta of 3072 and no TX-drop increase. R3 P4 RX/TX increased
3072, P1 TX increased 2304, P2 TX increased 768, and the three relevant R3
drop counters did not increase. Independent RX deltas were R1 +768 and R4
+2304, totaling 3072. VPP PID/start ticks and both resolved ECMP paths remained
stable. Thus the packet counts are consistent end-to-end, but the verifier
returned `OBSERVED`: the per-node snapshot spans were R3 266.00s, R1 11135.01s,
and R4 1089.93s; R1 exceeded the 1800s bound. This is not a tightly bracketed
same-batch proof, a statistical ECMP-balance claim, nor a performance result.
Latency, paired forwarding CPU, sustained performance, and line rate remain
open.

Artifacts under `build/physical-udp64-tight-window-20261013/`; SHA256:

| Evidence | SHA256 |
| --- | --- |
| `r2-source/result.json` | `e60b32f45227db4105e9ed4727b64d731500cfc7e429a3117d201109d9eaea49` |
| `r3-before-final/result.json` | `5ad9b4c6504199e19ed4958647ef254b52eeaf94f6dd5c622fc6498fe3d9a929` |
| `r3-after/result.json` | `3f2d1158f53b60de4092d50dedeaec266b91ebdc3dd079f5dae7076d319004ec` |
| `r1-before/result.json` | `82dd893334b2853505cb08e82aef755761de70be773b2ea376866520cd28d77d` |
| `r1-after/result.json` | `3375806bdd7cb4a5df77ff362e54905eddd86ea9a7ba780aace4f4aa2614ef92` |
| `r4-before/result.json` | `ab3c101a5ae48fb5051d3332913051569070f559a22a989ebd62e8ced0ba7b59` |
| `r4-after-final/result.json` | `40930a5be8495f9b83ee04b3b629db2460c521ccf8d35d9d83e5a1aec8fc717f` |
| `end-to-end-window.json` | `5aeff9a2d37566634da16fe85d7acb6f99dd25bc8a7617ba9ea1558cf0b8a36a` |

## R2 per-phase VPP CPU runner replay — 2026-10-13

After commit `3bc9e10`, the bounded serial runner was replayed on physical R2.
All three four-flow phases completed exactly 1024 frames each (3072 total);
I211 TX-good increased by 3072, TX drops remained 92175, VPP PID/start ticks
remained stable, and all packet-generator streams were removed. Per-phase VPP
process CPU, normalized to one logical CPU, was 99.65%, 99.26%, and 101.05%
at requested 100/500/1000 pps per flow. Values above 100% are valid for
multi-threaded process accounting; this run indicates roughly one core of
VPP polling load, not a throughput/capacity pass. The hardware-counter window
rates (394.05, 1640.05, 2588.68 TX pps) include CLI and stream-control
overhead and are diagnostics, not wire-rate claims. No R3 or independent
receiver snapshots bracketed this replay, so this is source-side runner/CPU
evidence only; latency, paired system CPU, sustained performance and line
rate remain open.

Artifact: `build/physical-udp64-cpu-window-20261013/r2-source/result.json`,
SHA256 `144cb4f42b999cb78105ad1c16269d918e4b9ba77c2442ce46aa50fc21c0e74e`.

## R2→R3 bounded UDP forwarding and CPU snapshots — 2026-10-13

The source runner and R3 before/after snapshots were correlated with the
R2-to-R3-only verifier. R2 I211 TX-good and R3 P4 RX both increased by 3072;
R3 P1/P2 TX increased by 2304/768, summing exactly to ingress, and P1/P2/P4
drop deltas were zero. R3 VPP PID 3272 and start ticks 5373772 were unchanged.
This proves bounded forwarding across both egresses during the snapshot
interval, but does not claim independent receiver delivery (R1/R4 counters
were not captured for this batch).

The R3 CPU snapshots were 691.49 seconds apart, much wider than the few-second
packet-generation phases. Their 99.75% one-core-normalized process CPU average
is therefore explicitly not phase-aligned and is not a traffic CPU result.
R2's per-phase CPU samples are concurrent with each source phase (99.54%,
100.66%, 99.27%), but reflect the packet-generator source VPP, not R3's
forwarding CPU. The observed NIC-counter phase rates include CLI/polling
overhead and are diagnostic only. Latency, paired forwarding CPU, independent
receiver totals, sustained performance and line-rate qualification remain
open.

Artifacts under `build/physical-udp64-r3cpu-window-20261013/`; SHA256:

| Evidence | SHA256 |
| --- | --- |
| `r2-source/result.json` | `d21ccb0555cd693449ab754d98de2004bb9dcb2f1f5ea4d69ed3956f23dc4218` |
| `r3-before/result.json` | `1dbb280a8920b2c69a3c932c847e26c3cf5bc561a978dfd0026f5f65b9fa3faf` |
| `r3-after/result.json` | `ae8bb8a3ad213f4daa7c0ca893fba202eb0c59fc1502f43a8162e8a2c355d155` |
| `r2-r3-window.json` | `4d980036dc48d5ebb45866a10995b144138cd7375e2e79dad9722384ce56a0c1` |

## Four-node physical UDP counter replay — 2026-10-13 (OBSERVED)

R2 again transmitted 3072 finite 64-byte frames; its I211 TX-good delta was
3072 and TX drops remained 92175. R3 P4 RX increased 3072, P1/P2 TX increased
2304/768 (sum 3072), and P1/P2/P4 drop deltas were zero. Independent receiver
RX deltas were R4 +2304 and R1 +768 (sum 3072). VPP identities remained stable.
The counters are numerically consistent with the generated batch, but the
per-node before/after snapshots span about 9.5 hours (R3 34296.82s, R1
34506.95s, R4 34530.42s). They therefore do not prove a tightly bracketed
same-batch end-to-end window. The verifier returns `OBSERVED`, not `PASS`, for
any per-node interval above 1800 seconds. Cross-node wall clocks are also
unsynchronized and were not compared.

R2 source VPP process CPU was sampled during each short generator phase at
99.67%, 99.19%, and 98.72% on a one-logical-CPU basis. The R3 CPU snapshots
were 34296.83s apart and are not traffic-aligned; their 99.75% average is not
used as forwarding CPU qualification. Measured NIC-counter phase rates
394.11/1665.94/2662.08 pps include CLI/control overhead, not wire rate.
Latency, sustained performance, and line rate remain open.

Artifacts under `build/physical-udp64-fullwindow-20261013/`; SHA256:

| Evidence | SHA256 |
| --- | --- |
| `r2-source/result.json` | `83526061fb54858e5d5dd9a1aa03aec7805f7ab8ffeb7a34a1c2fb996bcb84f9` |
| `r3-before/result.json` | `52bf879170246a54aca7163c7b3e801a630cc4507c2293793ae3729b515a31b8` |
| `r3-after/result.json` | `5dba6a94d5986a509c11ab12fee603a59780d1af2306b22a64959a7020ec3839` |
| `r1-before/result.json` | `7d554253db424ccee0eaf284177808d82152552d2b935a4acf4e3b73c160e021` |
| `r1-after/result.json` | `7a51bd6475f434f49caa8802a30cbe5ffdc4cbda79c7765f0273a9a5b35aa457` |
| `r4-before/result.json` | `5822fd98bd0c3f6b71d53894e04ff939c16c42b22bbb7eb85340da21f833b086` |
| `r4-after/result.json` | `9c5ecd7099b1577c38545b2e1ae0fb6432f02703e67c15d9af7814bbb9e67e3c` |
| `end-to-end-window.json` | `b46d38ad00fdcf7874f9a9b26a096a134e543442b3f305b34ed667a3c45246fe` |

## Independent receiver counter capture procedure — 2026-10-13

`capture_physical_receiver_serial.py` performs a read-only, MAC-guarded
snapshot of R1 or R4's VPP hardware good-packet counters. Capture separate
`before` and `after` snapshots around one bounded R2→R3 test; the serial cable
must be moved to each receiver for its snapshot. Then run
`verify_physical_receiver_window.py` to require stable per-node VPP PIDs,
monotonic counters, and (when supplied) an exact expected aggregate RX total.
It emits `receiver_counter_source=external-nic` and the independently observed
`receiver_packets_rx` for the common performance-result recorder. These tools
do not themselves generate traffic or qualify PPS/latency/CPU.

The staged baseline has since been superseded by a complete paired functional
counter window (details below). Neither window measures qualified PPS,
throughput, latency or CPU; 64-byte performance remains open.

## Paired physical receiver and ECMP counter window — 2026-10-13

Fresh identity-guarded snapshots bracketed one R2→R3 probe batch. R2 sent
420 requests (four 5-packet readiness probes plus four measured 100-packet
probes); each measured endpoint passed 100/100, zero loss, TTL 63. R2 VPP PID
remained 2097. R3 PID remained 3272 with the same two resolved forwarding
adjacencies. R3 P4 RX/TX increased +420/+420; P1 toward R4 increased +105 and
P2 toward R1 +315. R3 drop deltas were zero on P1/P2/P4. Independent receiver
NIC RX deltas were R4 +105 and R1 +315, totaling exactly 420. This verifies
functional packet delivery and use of both physical ECMP paths for this
four-destination sample. The 25/75 split is not statistical balance; ICMP
counter correlation is not a 64-byte performance, line-rate, latency or CPU
qualification.

Artifacts are under `build/receiver-window-rerun-20261013/`; SHA256:

| Evidence | SHA256 |
| --- | --- |
| `r2-probe/result.json` | `f082301e0038c2563d57a77a64ea5ad97489891c557ed7a402185529e0a10e95` |
| `r3-before/result.json` | `34c822455f13088d255114db0c0d547b5af28e7ce103b755e19e4ab5f26075e` |
| `r3-after/result.json` | `7318f99a259f3ed5592aa0f04d8fb7a068306d13fb1ff33cdc9a144e93034d63` |
| `r3-correlation.json` | `07392ea1a179420f6716a72fe37b08136d6c8a65b5683320e534e830fc4e0eef` |
| `r1-before/result.json` | `ddce4f69f925b6001126a5425f7956bd3d3484212f2ddae1d67761bc30fb470e` |
| `r1-after/result.json` | `8ff0a80c9ef1ac208e376260f4113fa69010aa98201275d223ad267569f53048` |
| `r4-before/result.json` | `3f7e5d4879dc0430c9e3c5dc27fbc102a0d7d8987b31db13439bb8d49e07d480` |
| `r4-after/result.json` | `9d5f74cb2d5dc0dbe8b0d3cc5bde60608044e3efb3db26a33496a71911fce0f4` |
| `receiver-window.json` | `8f9fa59c813222b40a2893d09963453d6b8b684ea39440565288a111f9d1e9e8` |

## Paired physical 64-byte UDP window — 2026-10-13

R2 P1 identity was checked as MAC `00:1f:7a:69:f5:ec`, VPP PID 2097,
1 Gbps/full duplex, with no pre-existing packet-generator streams. Three
bounded phases used four 64-byte UDP destination flows to `30.30.30.2`–`.5`,
256 frames per flow at requested 100/500/1000 pps per flow (3072 frames total).
The generator stopped each stream at its exact frame limit, the R2 hardware
TX-good-packet counter increased by 3072, source TX-drop count remained
92175, VPP PID/start-time remained stable, and all owned streams were deleted.

R3 PID 3272 and its two resolved FIB paths remained stable. R3 P4 RX increased
by 3072; P1/P2 TX increased by 2304/768, summing to the ingress count, and
their drop counters did not increase. Independent receiver NIC RX increased
by R4 +2304 and R1 +768, totaling 3072. This is a bounded 64-byte packet
delivery/ECMP path-use result, not a line-rate or qualified performance
result. Requested pacing and phase observation durations are not measured
wire PPS; the 75/25 four-flow bucket split is not a balance claim. R3 P4 TX
increased by 3071 (one fewer than ingress); return traffic was not part of the
pass criterion. Latency and synchronized CPU were not measured.

Evidence under `build/physical-udp64-window-20261013/`; SHA256:

| Evidence | SHA256 |
| --- | --- |
| `r2-source.json` | `0d3b048867a83da88ac00dd94d738739cbc3960c66251252ba30fc1be50d255a` |
| `r3-before/result.json` | `b340916a2f2d5849e2375c99c7ff7c744e6cb4609b225b97e3798f98e223f0c1` |
| `r3-after/result.json` | `8fea85e25f8759f9d2c3ccbccd63525b255a036aa2fb4d41d28570fbad7bf287` |
| `r3-correlation.json` | `c27a937fb8f0acb6ad5591e3cf563303b3695f0ff49e7d8602551520d7655012` |
| `r1-before/result.json` | `55da076ee0429bcaa9258edf3d2d22e07e77ccb6c3b7a91e9d0d35760e621a7f` |
| `r1-after/result.json` | `24573d868aceca593a4ed949e52e775d8d065fbb0736cd1b30e84c294d39b3c1` |
| `r4-before/result.json` | `a8c57a181114e5987a0eb17b9f1b75d527aeb6d1e6a8b4daac5039bbd936f44e` |
| `r4-after/result.json` | `534d8e1015c4897b428ca104274876d90efd621c402f410ae9d296ab62702a26` |
| `receiver-window.json` | `6bcc2ee8051fd8752e0c51bdd7ce7de0c53ca18a0e1ee125e08bb47351799665` |

The reusable `danos-test/integration/run_physical_pg_serial.py` runner was
replayed on physical R2 after commit `4748e99`. It passed all three bounded
phases (100/500/1000 requested pps per flow, four flows × 256 frames); VPP
generated-frame count and R2 I211 hardware TX-good delta both equal 3072,
TX drops remained 92175, VPP identity remained stable, and all streams were
removed. Structured artifact:
`build/physical-udp64-runner-smoke-20261013/result.json`, SHA256
`8abdf44693c768dd76b7595d69e9937f554fd3d6b493217b53712e8e02c1a3d0`.
This validates the scripted source-side lane on the physical runner. It is
not a paired receiver-window replay and does not measure wire PPS, throughput,
latency, CPU, or line rate; those qualifications remain open.

## Scripted physical UDP forwarding window — 2026-10-13

The reusable runner was then bracketed with identity-bound R3 counter/FIB
snapshots. R3 VPP PID remained 3272 and both ECMP adjacencies remained
resolved. R2 generated 3072 frames; R3 P4 RX increased by 3072, and egress TX
increased by 2304 on P1 plus 768 on P2, exactly matching the generated count.
P1/P2/P4 drop counters did not increase. The two path buckets therefore both
forwarded this bounded four-flow sample. Independent R1/R4 receiver snapshots
were not taken for this runner replay; an earlier inline-runner window has
separate receiver evidence above. This is functional forwarding evidence only,
not measured wire PPS, throughput, latency, CPU, or line-rate qualification.

Artifacts under `build/physical-udp64-scripted-window-20261013/`; SHA256:

| Evidence | SHA256 |
| --- | --- |
| `r3-before/result.json` | `37b318022466ec0ed5fed68745698d3c2b7b095ed499a5197b03f852773a971c` |
| `r2-source/result.json` | `d5cdf84f1af371b08f9ad97b14df9325a1ee3eafb6ebf1a2b098ff385bf90cc4` |
| `r3-after/result.json` | `05c7a06ec8afa613a8ab8519017ee5aa5b1e688341b384335959e7e69deb1fec` |

Counter deltas were independently checked against the source runner artifact:
R3 P1 RX/TX/drops `+2301/+2304/+0`, P2 `+768/+768/+0`, and P4
`+3072/+3069/+0`. The small RX/TX differences on P1 and P4 reflect opposite-
direction/background packets in interface totals; the forwarding assertion is
the exact P4 ingress and aggregate P1/P2 egress match.

## Scripted physical UDP extended window — 2026-10-13

The same runner was exercised with 1024 frames per flow in each of its three
requested pacing phases: 12,288 generated frames total. R2 hardware TX delta
matched exactly and its TX-drop counter stayed at 92175; VPP identity was
stable and packet-generator streams were removed. R3 VPP PID remained 3272,
and both FIB adjacencies stayed resolved. R3 P4 RX increased by 12,288; P1
and P2 TX increased by 9,216 and 3,072 respectively, summing exactly to
12,288. P1/P2/P4 drops did not increase. This validates bounded longer-window
forwarding and use of both ECMP egresses; it is not a wire-rate, PPS,
throughput, latency, CPU, or long-duration soak qualification. Independent
R1/R4 receiver snapshots were not captured for this run.

Artifacts under `build/physical-udp64-soak-20261013/`; SHA256:

| Evidence | SHA256 |
| --- | --- |
| `r3-before/result.json` | `b3fd1313446132fa613c2ce0e59cb461399b4927a3430b32674365b920cb8010` |
| `r2-source/result.json` | `4571c80298a501ade811ca28ebd355478399864a0a6affaf3a018db95943e7f1` |
| `r3-after/result.json` | `8297123e38b08604735e1cbb7a54251e74e55d66596abee352406a7b70a72df3` |

Counter deltas: R3 P1 RX/TX/drops `+9210/+9216/+0`, P2
`+3069/+3072/+0`, and P4 `+12288/+12279/+0`. As in the smaller window,
per-interface receive/transmit totals include reverse-direction/background
packets; the ingress and aggregate egress counters exactly match the generated
frame total.

## Per-phase hardware-rate instrumentation replay — 2026-10-13

The source runner now brackets each finite pacing phase with the R2 I211
`tx_good_packets` counter and a monotonic observation interval. On physical
R2, each phase's hardware delta matched its 4096 generated frames; aggregate
TX delta matched 12,288, TX drops stayed at 92175, and all streams were
cleaned. The resulting window-average TX rates were 398.55, 1904.98, and
3716.90 pps for requested aggregate rates of 400, 2000, and 4000 pps. The
corresponding 64-byte frame-bit rates were 0.2041, 0.9753, and 1.9031 Mbps.
These rates divide the verified hardware frame count by the runner's observed
phase interval, including stream enable/stop polling overhead; they are
repeatable end-to-end window measurements, not isolated wire-rate or line-rate
measurements. Mbps counts 64-byte frame bits and excludes physical-layer
preamble/IFG overhead. Latency, CPU, and independent receiver counters were
not collected in this replay.

R3 VPP PID remained 3272 and both ECMP paths stayed resolved. R3 P4 RX
increased 12,288; P1/P2 TX increased 9,216/3,072, and P1/P2/P4 drop counters
were unchanged. Thus the source hardware rate observations are paired with
functional forwarding through both egress paths, but do not qualify full
receiver delivery or performance acceptance.

Artifacts under `build/physical-udp64-measured-20261013/`; SHA256:

| Evidence | SHA256 |
| --- | --- |
| `r3-before/result.json` | `c4682e2824f93b1a31910de2d37684b37909cbdb6f40f7b3d07fd00ba0cb1c77` |
| `r2-source/result.json` | `5207f885b9785a1351eb5957caf5d7dc8a4cde08c753f26b7919db7d1e40e5f4` |
| `r3-after/result.json` | `9d6d69f1fde3c6dcfbf7631e6805996fd06f6c9fde30fc83fd18c653c3774b5b` |

## Full independent-receiver window with measured runner — 2026-10-13

The instrumented runner was exercised again with R1/R4 NIC snapshots bracketing
the same 12,288-frame profile. R2 transmitted 12,288 frames according to both
the packet generator and I211 TX-good delta; its TX-drop count stayed at 92175.
Window-average TX rates were 394.39/1919.27/3742.50 pps (64-byte frame-bit
rates 0.2019/0.9827/1.9162 Mbps) for requested aggregate rates of
400/2000/4000 pps. These include runner enable/stop polling overhead, so are
test-window averages and not line-rate measurements.

R3 PID remained 3272 with both FIB paths resolved. P4 RX was +12,288; P1/P2
TX were +9,216/+3,072, exactly 12,288 total, with no drop-counter increases.
Independent receiver snapshots measured R4 RX +9,216 and R1 RX +3,072; the
receiver-window verifier passed with aggregate RX exactly 12,288. The new
`verify_physical_udp64_window.py` joins the R2 source, R3 ingress/egress, FIB,
and independent R1/R4 counters into one digest-indexed JSON result and passed
on this evidence set. Thus this
run closes the bounded end-to-end packet-delivery and two-bucket functional
gate for this profile. It does not qualify latency, CPU, sustained throughput,
or line rate; those remain open.

Artifacts under `build/physical-udp64-receiver-window-20261013/`; SHA256:

| Evidence | SHA256 |
| --- | --- |
| `r1-before/result.json` | `6409937c57e327a60747429105a41afad56638b6c902ef07d98a403f685958fa` |
| `r1-after/result.json` | `b8a726da6b1e41e21500da6cd17d1d56a230f3c4ea8d51b67ac35402b517ebdf` |
| `r4-before/result.json` | `2f55dc8b804c7b4f46f16e3e993af0c8513a9b729f00ba49ac1dafc309c85c5f` |
| `r4-after/result.json` | `07a263ac2a2e9ea3d37c7dd68937e16831e553d31c95fdbaaf1914de80d3b970` |
| `r3-before/result.json` | `a293fe1c9a720e6c4c834ab0151c88dbca7d1ff666b928cbd7b0e69203601668` |
| `r3-after/result.json` | `3d47aad82045e382b3b15560128692379ce28a7539ef88c1f94b99f3dc33ab3a` |
| `r2-source/result.json` | `272636faa32ac2d3be806efe49c62de510f87d6100e85d2a1ee09d3b22c692f7` |
| `receiver-window.json` | `471553b361065fa390f98d602b7aadbec9e35ed2cb9429abdabc918724d0c7db` |
| `end-to-end-window.json` | `14e340804161d6ef517bc25be302b1c4df41fee191369c2dcae4d2e25fb35fa3` |

## Earlier R2 bounded source calibration — 2026-10-13

UART identity `00:1f:7a:69:f5:ec` confirmed R2 VPP 26.10, P1 at 1 Gbps
full duplex, and a resolved route to `30.30.30.0/24` via R3. A finite
four-flow, 64-byte packet-generator calibration completed at 100/500/1000
requested pps per flow, 256 frames per flow per phase (1024 per phase;
3072 total). VPP PID remained 2097; the pre-existing TX-drop count stayed
92175 through all phases; no packet-generator streams remained afterward.

The first orchestration attempt completed only the 100-pps phase and stopped
before creating the next phase due to a local rate-index bug. A read-only
audit confirmed no residual streams; the corrected run then completed all
three phases. The structured result is
`build/r2-bounded-source-calibration-20261013/result.json`. This is a
source-side calibration, not a performance result. R3's before snapshot
predated both the initial 1024-frame phase and corrected 3072-frame run;
the after snapshot records FIB 0→4096 and egress TX deltas P1 +3072/P2
+1024, summing to 4096. This correlates all generated phases with real R3
forwarding and confirms both ECMP buckets carried traffic. P1 drop count
changed 11→12 and P2 stayed 4; do not claim zero loss. The identity-bound
R3 after snapshot is `build/r3-after-r2-bounded-calibration-20261013/result.json`,
SHA256 `68f0283daec4dbad7fe1bfd350d8744c008b02aee27cec23862518873a04c27f`.
No R1/R4 independent receiver deltas or immediate per-run hardware source
TX delta were captured, so receiver delivery and performance qualification
remain `ENVIRONMENT-OPEN`. The structured source result SHA256 is
`057216fd05a982969c4de2b83c9462fdfa3d7c7b1083d3232ab4daa08989065d`.

## R2 four-flow ECMP counter correlation — 2026-10-13

Captured identity-bound, read-only R3 counter/FIB snapshots immediately before
and after a fresh R2 probe batch. R2 sent 420 requests total (four targets,
each 100 measured plus one 5-packet readiness probe); all measured probes were
100/100 with zero loss and TTL 63. R3 VPP PID stayed 1576 and the same two
resolved forwarding DPO adjacencies remained installed. Counter deltas were:
R3 P4 ingress RX/TX +420/+420; P1 toward R4 TX +105; P2 toward R1 TX +315.
The two egress deltas sum exactly to the ingress and transmitted request count;
their drop counters did not increase. Thus both current-release physical ECMP
paths carried this batch. The 25/75 split is only four destination flows and
is not a balance, throughput, or line-rate qualification. No FRR/ZAPI change
or traffic during restart was tested.

Evidence snapshots, probe logs and hashes are indexed in
`iso-installation-acceptance.md`. Counter-capture utility SHA256:
`b520c5d047c3693ec2f306d86eb357eaf2635a1ce6c4f723d21f571d51ba1b69`.

## R2 → R3 post-restart endpoint regression — 2026-10-13

After the R3 VPP service restart/replay checkpoint, the identity-guarded R2
serial probe verified R2 MAC `00:1f:7a:69:f5:ec`, stable VPP PID 2097 and a
1 Gbps P1 link. Through R2→R3, all four remote loopback endpoints
`30.30.30.2`–`.5` passed 100/100 ICMP each, zero loss, reply TTL 63.
This confirms endpoint reachability after R3 recovery, not traffic continuity
during the restart, ECMP bucket distribution, FRR/ZAPI route changes or a
performance result. Per-endpoint logs and result digest are indexed in
`iso-installation-acceptance.md`.

Result: `build/r2-r9-to-r3-r14-postrestart-ecmp-20261013/result.json`,
SHA256 `7340eba9e88a44803209382c1e37a9cff22cdb87967e5857ae2b2e0991dbbafa`.

## R3 r14 post-install checkpoint — 2026-10-12

R3 identity `00:1f:7a:69:f7:4c`, installed r14 build/binary, SATA/ext4 root,
VPP/FRR runtime, sockets and all four I211 DPDK bindings pass inventory.
After install, the existing R3 lab role was restored and the VPP post-start
replay hook registered. Ports 1, 2 and 4 show 1 Gbps carrier; port 3 remains
down. Read-only FIB shows two resolved paths for `30.30.30.0/24`: R4
`10.20.0.2` via port 1 and R1 `10.30.0.2` via port 2. UART Robot 3/3 and
CTest adapter pass. The first Robot report failed on a stale FIB text parser;
the updated parser checks each resolved path and port, and that failed report
is preserved. No packet traffic was generated during this checkpoint, so
forwarding/distribution is still open. Hashes and result locations are in
`iso-installation-acceptance.md`.

## R2 r9 to R3 r14 four-destination transit — 2026-10-13

R2 identity `00:1f:7a:69:f5:ec`, build `3f1691c` / r9, P1 1 Gbps and
resolved route `30.30.30.0/24 via 10.10.0.1`. R2 sent bounded readiness
probes and then 100 packets each to `30.30.30.2`–`.5`; all four completed
100/100 with 0% loss and TTL 63. R2's P1 counter delta was RX +428, TX +431.

R3's post-run FIB retained two resolved equal-weight next hops. ECMP DPO
counter was 430 packets; P1 toward R4 recorded 110 RX/TX, P2 toward R1
recorded 320 RX/TX. The sum matches the request frames, including readiness
probes. This verifies actual use of both physical egresses for this four-flow
sample; the 25.6/74.4 split is not a large-sample balance or performance
claim. R3's P4 toward R2 recorded RX 431/TX 428. Historical interface
drop/error totals were not reset, and no zero-drop transition claim is made.

R2 remains r9 while R3/R1 are r14; R4 version is unverified. This does not
qualify an all-r14 topology or FRR/DPA-driven route changes. Original JSON,
ping logs, post-run Robot XML and hashes are indexed in
`iso-installation-acceptance.md`.

## R3 r14 VPP service restart — 2026-10-13

With the explicit R3 profile and verified post-start replay hook installed,
`systemctl restart vpp` changed PID 741→1576. P1/P2/P4 returned up, the
`30.30.30.0/24` FIB again showed two resolved paths, and R3-originated direct
100-packet probes to `10.20.0.2` and `10.30.0.2` each passed 100/100, zero
loss. UART Robot after restart passed 3/3. This verifies VPP service replay
plus direct adjacency recovery. No packet stream was left running across the
restart, so it does not test transit continuity, loss window or downtime;
FRR/ZAPI and mgrd WAL were not used. Result and ping hashes are indexed in
`iso-installation-acceptance.md`.

## R3 r14 physical ECMP next-hop withdrawal/restore — 2026-10-13

The identity-guarded UART probe temporarily administratively disabled R3 P2
(R1 path), removed only `10.30.0.2` from `30.30.30.0/24`, and verified the
actual forwarding DPO—not only path-list flags—contained one resolved bucket
via R4 P1. R3→R4 direct ping passed 5/5. The probe then brought P2 up,
restored the next hop, retried bounded ARP warm-up, verified both forwarding
adjacencies were IPv4 next-hop DPOs, and passed direct R4 and R1 probes 5/5.
VPP PID stayed 1576; final P2 carrier was 1 Gbps and cleanup reported no
errors. The first seven R1 warm-up groups saw no/full partial replies; the
eighth passed 5/5, so restoration is not described as instant or lossless.

This is a scoped physical VPP FIB/DPO path withdrawal and restore, with
direct-peer reachability. It does not verify FRR/ZAPI route withdrawal,
physical link-loss detection, R2-originated transit across the transition,
remote loopback reachability from R3, loss budget, convergence time, or
performance. Remote peer return routes are specific to R2's `10.10.0.2`, so
R3-originated loopback probes were intentionally excluded. Initial attempts
exposed both an unresolved ARP DPO despite a resolved path-list flag and this
return-route asymmetry; their FAIL artifacts remain preserved separately.

Final result: `build/r3-r14-ecmp-failover-final3-20261013/result.json`,
SHA256 `b321c93f32dce7d2ede733fc2f5194e4bf472a2576f668a7b3ccdc95b6a4f052`.
Probe source SHA256:
`8c451cef589c0d6dd28969eb99bbda37e1b0fb167986ba7f4c02e1e7808bc46e`.
The local FIB-evidence unit test is registered as
`r3_ecmp_failover_serial_evidence`.

## R1 r14 installed-runtime checkpoint — 2026-10-12

R1 identity `00:1f:7a:40:28:20`, SATA ext4 root and exact r14 binary digest
PASS; all four I211 devices use `uio_pci_generic`. P1 connects R3 P2 at
1 Gbps full duplex, R1 `10.30.0.2/24`, R3 `10.30.0.1`; route
`10.10.0.2/32` returns via R3. Loop0 owns `30.30.30.2`–`.5` /32.

`restore_physical_lab_serial.py` deployed the missing project replay helper,
`/etc/danos/physical-lab.cli`, and additive VPP post-start drop-in
`/etc/systemd/system/vpp.service.d/30-danos-lab-replay.conf` after identity
and binary checks. This is a post-install lab overlay, not mgrd WAL replay.
The helper SHA256 is
`518ec5844b43f9e43e526c112857daf7c8c2a62700b62c70d10a69026113c241`;
R1 profile SHA256 is
`68e0beac8a64a0736db0c5249c96727a0a08070bdeeacb033d44c68331ff0b89`.

VPP PID 1717 → 1891 restores this profile. Bounded carrier polling took
3.38 seconds after restart returned; readiness was 4/5 then 5/5. Warm
R3 and transit R2 ICMP each passed 100/100 (TTL 64/63). Earlier immediate
and carrier-only probes failed and remain archived. Do not count this as
lossless restart, ECMP distribution or throughput. Read-only Robot 2/2 PASS.
See `iso-installation-acceptance.md` for result paths and immutable hashes.
Next router-side validation requires UART on R3; do not address R3 while
the UART still identifies R1.

## Recovery checkpoint and preparation history

**Bounded physical UDP PG transit PASS (functional only).** R2 emitted four
finite streams of 20 MAC frames each (64 bytes excluding FCS). R3's unchanged
PID 2312 shows P4 RX/TX +80, P1 +60 and P2 +20; FIB 8000→8080, both next hops
resolved, drops and node errors unchanged. This demonstrates both ECMP
egresses, not equal distribution, UDP application delivery or line rate.
Evidence and hashes are in `v0.16-performance-result-schema.md`; checked
deltas are `build/r3-r2-pg-probe-deltas-20261010.json`. Performance remains
ENVIRONMENT-OPEN pending synchronized measurements and receiver evidence.

**All-restored unified warm-transit regression PASS (lab CLI only).**
Final capture verifies R3 identity, unchanged PID 2312 and correct peer MACs.
Against the separately archived all-restored baseline:

| R3 port | Baseline RX / TX | Final RX / TX | Delta RX / TX | Drops before / after |
|---|---|---|---|---|
| 4 / ingress | 6398 / 6398 | 10398 / 10398 | 4000 / 4000 | 2 / 2 |
| 1 / R4 | 2300 / 2300 | 3300 / 3300 | 1000 / 1000 | 2 / 2 |
| 2 / R1 | 4300 / 4300 | 7300 / 7300 | 3000 / 3000 | 2 / 2 |

FIB aggregate 4000→8000 and both Ethernet adjacencies remain resolved.
Recorded node-error counters are unchanged. Four strict endpoint logs below
complete the evidence chain; older PENDING statements for this regression
are superseded. No whole-machine reboot, cold-zero-loss recovery, ledger
replay or line-rate qualification follows from this result.
Final capture `build/dpdk-installable/r3-all-restored-post-20261010.serial.log`
SHA256 `3b1662031154eae87cd56d14c90a486ff2981883a6f0d953027f707dcb866fe5`.

All-restored endpoint regression completed: R2 identity and shared-prefix
route verified, no pre-existing ping process. A separately identified run
in `/tmp/ecmp-all-restored-20261010/` sent four concurrent 1000-probe streams.
All four strict endpoint logs pass (4000/4000 total, zero loss, TTL 63,
complete unique sequences); original UART-transferred hashes match R2.
Two-egress evidence remains PENDING until R3's post-run counters are compared
with `r3-all-restored-baseline-20261010.serial.log`, PID 2312. No reboot,
route change or further probe traffic was introduced on R3 by this run.
Endpoint logs are archived under `build/dpdk-installable/`:

| Log | SHA256 |
|---|---|
| r2-all-restored-p2-20261010.log | 3b41081769ed15c9325b5ec8841f8f37dccfc649cf47e3a60e401118dcc0014c |
| r2-all-restored-p3-20261010.log | 2fd1c78592091d57180c2e1c1d852ffefccc4a5701fdfe24c6d3d25a3ff4b59f |
| r2-all-restored-p4-20261010.log | c172f78b8db021cc0bb67958186ef10d34ebd0198fc27501417812fdef3ff04a |
| r2-all-restored-p5-20261010.log | 41cd1bb87da041ca7055bc44808b2383b185b76498e7d1ae992e743bab3dde39 |

All-restored topology regression baseline (before new traffic): R3 MAC
verified, PID remains 2312. Three peer MACs and interface addresses correct,
both ECMP adjacencies Ethernet-resolved, FIB aggregate=4000 packets.
Port1 RX/TX=2300/2300, port2=4300/4300, port4=6398/6398,
displayed drops=2 on all three. Error snapshot includes one historical
interface-down event each on ports1/2, three glean ARP requests; retain
these baselines rather than clearing counters. Previous peer recovery probes
are already included and must not be attributed to the forthcoming run.
No configuration or restart action in this capture. Next run four-target
traffic from R2, then compare same-process counters against this snapshot.
Evidence `build/dpdk-installable/r3-all-restored-baseline-20261010.serial.log`
SHA256 `4193263172eaf208125389ca501cb5af3d3701e98e26ae3196179031dc768e14`.
This is a prepared baseline, not a completed new regression result.

R1 peer service recovery: MAC `00:1f:7a:40:28:20` verified. Hook/drop-in
and R1 profile deployed with matching hashes; profile SHA256
`68e0beac8a64a0736db0c5249c96727a0a08070bdeeacb033d44c68331ff0b89`.
VPP PID 794→1843, restart exit 0, active, journal LAB-REPLAY PASS.
P1 10.30.0.2/24, loop0 30.30.30.2–.5/32 and return route to
10.10.0.2/32 automatically restored. Cold transit summary 99/100 (1%)
is retained, warm R1→R3→R2 passes 1000/1000, TTL 63. Original warm
log was transferred, hash matched and strict endpoint verifier passed:
`build/dpdk-installable/r1-restart-warm-20261010.log` SHA256
`fb260facb8d3370e5b7e3204b05a1e17720476eb5a2cf9b58e27114070f8119b`.
Deployment/restart and test transcripts are `r1-replay-restart-20261010.serial.log`
and `r1-post-restart-test-20261010.serial.log` in the same directory.
All four nodes now have deployed explicit lab replay profiles and individual
VPP service restart observations. Earlier undeployed-peer notes below are
historical. Next re-run R2 four-target traffic in this all-restored state and
capture a fresh R3 before/after baseline; no collective lossless restart,
machine reboot or DANOS ledger replay is yet qualified.

R4 peer service recovery: MAC `00:1f:7a:40:01:80` confirmed; same lab
hook/drop-in and R4 profile installed with verified hashes (profile SHA256
`480a4a5d82df77f80e896298eb1d81bd5a8623ed5ef15549ca7d200a0cd49e05`).
VPP PID 792→2140, restart exit 0, active, journal LAB-REPLAY PASS. P1
10.20.0.2/24, loop0 shared 30.30.30.2–.5/32 and 10.10.0.2/32 return
route restored automatically. Cold-neighbor transit summary 99/100 (1%)
is retained. Warm R4→R3→R2 completes 1000/1000, TTL 63, zero loss;
original endpoint log transferred and SHA256 matched, strict verifier PASS.
Local log `build/dpdk-installable/r4-restart-warm-20261010.log` SHA256
`06250fb3cd77468c5f965053c428d7d52b3315cbc9eac234c58cb270aed653ed`.
Deployment/restart transcript is `r4-replay-restart-20261010.serial.log`;
test transcript `r4-post-restart-test-20261010.serial.log` in the same directory.
This qualifies R4 lab service replay plus warm transit, not lossless restart,
whole-machine reboot or simultaneous two-egress recovery. R1 is now the only
node without a lab hook; older R4-not-deployed statements are superseded.

**PASS, scoped R3 VPP service restart / lab CLI replay with warm-neighbor
transit regression**. Final UART observation confirms identity MAC
`00:1f:7a:69:f7:4c`, unchanged post-restart PID 2312, two Ethernet-resolved
equal-weight next hops and FIB aggregate 4000 packets. Compare against the
new-process warm-link snapshot (not old-process counters):

| R3 port / role | Baseline RX / TX | Final RX / TX | Delta RX / TX | Drops baseline / final |
|---|---|---|---|---|
| 4 / R2 ingress | 200 / 200 | 4200 / 4200 | 4000 / 4000 | 2 / 2 |
| 1 / R4 egress | 200 / 200 | 1200 / 1200 | 1000 / 1000 | 2 / 2 |
| 2 / R1 egress | 200 / 200 | 3200 / 3200 | 3000 / 3000 | 2 / 2 |

Combined with four independently archived and strictly verified R2 logs,
both egresses carry real request/reply traffic after automatic startup replay.
Evidence `build/dpdk-installable/r3-restart-delta-20261010.serial.log`
SHA256 `c317843f0d975e52afb6dccec8ab1ffcd2ddbcc05366db5df25300bf20fdf7a5`.
Earlier pending two-egress statements below are superseded for this run.
Cold-neighbor losses remain recorded; no lossless-restart or outage-duration
claim. Full-machine reboot, remaining peer hook deployment, FRR/DPA and
DANOS ledger replay remain OPEN. This is a lab-only hook, not a production
configuration owner. Next deploy profiles to R4/R1 and verify their service
recovery before considering topology-wide machine reboot qualification.

Post-R3-service-restart endpoint regression: R2 MAC verified, source
10.10.0.2 and shared-prefix route preserved. Four concurrent 1000-probe
streams complete 4000/4000, zero loss, all TTL 63. Each original device log
was recovered compressed over UART, SHA256 matched, and strict sequence/
destination/TTL validation passed. This establishes warm-neighbor transit
delivery after service replay; two-egress recovery still needs R3 post-run
deltas against the new-process 200/200 baseline. No overall restart gate
PASS, cold-zero-loss or ledger replay is claimed. Local evidence:

| Log under build/dpdk-installable | SHA256 |
|---|---|
| r2-post-r3-restart-p2-20261010.log | 34ac75cf0232b76434583e6f5480f4bcef197df44be5cf4d799f8d3452f5a1ee |
| r2-post-r3-restart-p3-20261010.log | 8b219ab9cd00ffeff7db6fe672cd29ee44a4785125fc018cbca869dcd9d0172e |
| r2-post-r3-restart-p4-20261010.log | 3d24827fa52e96ff42aa7395a601a7cfb3749fae26aeb25f602606f6a3db1e78 |
| r2-post-r3-restart-p5-20261010.log | 27f3eb791a0465ead95e5eba0955b12e0c529fd7b590c323d5c31ba3ae09c1e3 |

R3 follow-up: identity MAC `00:1f:7a:69:f7:4c` verified before deployment.
Same hook and additive drop-in installed; R3 profile SHA256
`247498d09777db22b4e03153b1ff7774b1ddb7fd7ab599da4657f6101cda2dc3`
matched local renderer. VPP service PID 789→2312, restart exit 0, active,
journal LAB-REPLAY PASS. Three interface addresses and two equal-weight
ECMP next hops automatically reappeared. Initial FIB adjacencies were ARP,
not Ethernet-resolved; first direct probe to each peer was **99/100 (1%)**,
TTL 64. Correct R1/R2/R4 MACs were subsequently learned. Warm repeats
are 100/100 on all three direct links; interface RX/TX increase 100 each,
displayed drops stay 2. This is local-output connectivity, not post-restart
transit ECMP qualification. Baseline for the next R2 transit run is port1,
port2 and port4 RX/TX=200/200, drops=2 each. Do not compare reset counters
against snapshots from the previous VPP process. No machine reboot occurred.
Next move UART to R2 for endpoint transit regression, then collect R3 deltas.
Local evidence under `build/dpdk-installable/`:

- `r3-replay-restart-20261010.serial.log`: `4d3307ebd5864eab6556b11bc6a334fa009454cfe78adf1d9e12abe9aa69441a`
- `r3-post-restart-peers-20261010.serial.log`: `baaeecc091adbd5fdff11376040f4d96e6db12f42a4ed86fb61407c5dadaa752`
- `r3-post-restart-warm-20261010.serial.log`: `ca5273ca439edd2ca5a91a736a3424900a1eccedb33a6292430a33b8a3aa2d3c`

2026-10-10 update: R2-only hook deployment and service recovery observed.
Installed `/usr/libexec/danos/replay-physical-lab`, explicit R2 profile
`/etc/danos/physical-lab.cli`, and additive systemd drop-in
`/etc/systemd/system/vpp.service.d/physical-lab.conf`. Existing readiness
check and PCI discovery remain intact. Hook/profile hashes matched local
sources before restarting. Hook SHA256
`518ec5844b43f9e43e526c112857daf7c8c2a62700b62c70d10a69026113c241`;
R2 profile `7c003e37ac16f9196c44e268c912eb0844bca753f67a7a401ca0fa13c0fc21c0`.
VPP PID changed 791→2097, restart exit 0, service active, journal records
LAB-REPLAY PASS. Interface 10.10.0.2/24 and shared-prefix route restored.
First post-restart cold-neighbor probe was **999/1000 (0.1% actual loss)**,
despite VPP displaying integer 0%; first reply was sequence 2. Before traffic
FIB showed an ARP adjacency. Missing first probe is consistent with unresolved
neighbor but not packet-trace-proven. Warm repeat summary is 1000/1000;
neighbor matches R3 MAC `00:1f:7a:69:f7:4f`.
No claim of zero-loss cold restart, complete topology reboot, or DANOS ledger
replay. Local transcripts: `r2-replay-deploy-20261010.serial.log`,
`r2-vpp-restart-20261010.serial.log`, `r2-restart-warm-20261010.serial.log`
under `build/dpdk-installable/`. Remaining nodes have not received hooks.
Remove only this drop-in and opt-in profile to disable lab replay, then
daemon-reload; no package unit replacement is necessary.
The following preparation notes describe the earlier pre-deployment state.

`python3 danos-test/integration/physical_lab_profile.py R2` renders the
explicit lab CLI topology for the chosen R1/R2/R3/R4 role. Five unit tests
cover addresses, shared loopbacks, return routes, ECMP and absence of implicit
device binding/reboot actions. These deterministic profiles are for a fresh
VPP instance only, not repeated application to live state: loop0 creation
and IP additions can conflict with existing objects. No live configuration
export, service restart or hardware reboot is implemented by this renderer.

R2 inspection confirms `vpp.service` runs `/usr/bin/vpp -c
/run/vpp/startup.conf`, generated by `prepare-dpdk-pci`; `/etc/vpp/startup.conf`
does not exist. Therefore writing an assumed `/etc/vpp/startup.conf` would
not implement recovery. Integrate an explicit lab-only post-start hook with
the generated configuration, preserving existing readiness checks and driver
discovery, and verify CLI errors as well as socket readiness before testing
a restart. No hook has been installed yet. Physical recovery remains OPEN;
this will qualify lab topology replay, not DANOS ledger replay.

## 2026-10-10 physical next-hop withdrawal/restoration closeout

**PASS, scoped manual-VPP-CLI functional qualification**: four concurrent
60000-probe endpoint logs completed 240000/240000, zero loss, TTL 63,
exit status 0. Every sequence is present exactly once for each destination.
Original logs transferred compressed over UART match device SHA256 and pass
`verify_physical_ping_log.py`. Combined with the R3 single-path windows and
independently verified final two-path restoration below, this closes the
two-direction next-hop withdrawal/restoration run. Both transitions occur
within the complete endpoint runs; no probe sequence is lost.

| Target | Sent / received | Elapsed s (R2 uptime) | Observed probes/s | RTT p50 / p99 ms |
|---|---|---|---|---|
| 30.30.30.2 | 60000 / 60000 | 600.45 | 99.925 | 0.1309 / 0.2305 |
| 30.30.30.3 | 60000 / 60000 | 600.44 | 99.927 | 0.1317 / 0.1799 |
| 30.30.30.4 | 60000 / 60000 | 600.44 | 99.927 | 0.1323 / 0.2209 |
| 30.30.30.5 | 60000 / 60000 | 600.45 | 99.925 | 0.1306 / 0.1931 |

Elapsed time includes the per-command wrapper, not synchronized cross-device
timestamps. RTT percentiles use nearest-rank over each 60000 ICMP replies;
these are low-rate RTT, not one-way latency or throughput-capacity metrics.
CPU and wire-rate Mbps were not measured. No physical link was disconnected,
and FRR/DPA did not program these routes. Link-detection, restart/replay,
FRR/DPA convergence, line rate and artifact-bound cold boot remain OPEN.
Earlier IN PROGRESS/PENDING notes below are historical, superseded for this
specific run. All original endpoint logs are retained on R2 under
`/tmp/ecmp-failover-20261010-b/` and locally under `build/dpdk-installable/`:

| Local log | SHA256 |
|---|---|
| r2-failover-20261010-p2.log | 422166a30cf7a875df8961e94de7254aedf3c99429235b73d2245fa0113f7604 |
| r2-failover-20261010-p3.log | 4ba0019123ddb995ec06f82a5c744ceb4405c4746ff3313d6ab37d729a45a512 |
| r2-failover-20261010-p4.log | 55e50a47c33c0c21c641d40dcbbb71b91b3a70d71b268ba279c8715c2aa2529e |
| r2-failover-20261010-p5.log | 8acdf919332f72478d2537b8cc1c2e9bf15f4c7097350ee1267e4b4d9ab12455 |

## 2026-10-10 physical transit ECMP functional checkpoint

Withdrawal/restoration device-side observation completed; final endpoint
verdict remains **PENDING**. Live traffic growth was checked before mutation.
R3's clock is not synchronized to R2; do not correlate their UTC directly.
Deleting only the R1 next-hop changed FIB 2→1 bucket. Over device uptime
71427.40→71437.43 (10.03 s), R4 RX/TX and ingress RX/TX each rose 4007,
R1 remained unchanged. Restoration changed FIB 1→2 and both egresses resumed.
Deleting only R4 next-hop then changed FIB to the R1-only bucket. Over
71456.96→71466.99 (10.03 s), R1 and ingress RX/TX each rose 4005,
R4 remained unchanged. Displayed drops stayed at port1=215, port4=880.
These stable single-path windows do not bound loss at the transitions.

The second explicit restore command suffered UART character loss
(`/ruvpp/cli.sock`, `iroute`), and its subsequent FIB still showed one bucket;
the printed RESTORED marker was not accepted as proof. An EXIT restoration
trap was present. A separate slow per-character resend and independent FIB
check verified both resolved next hops; subsequent R4 RX/TX +497 and R1
RX/TX +1491 confirmed both active again. No shutdown/reboot occurred.
Final original R2 logs and completed summaries are required before declaring
the entire failover run PASS; link failure detection and FRR/DPA convergence
are not tested by CLI next-hop deletion. Local SHA256 evidence:

- `r3-withdraw-restore-20261010.serial.log`: `bdeb8693b2a50a3021e884274ce00d3c0820cc47215ac06da1e83f0960d6766a`
- `r3-withdraw-a-restore-20261010.serial.log`: `b2d76f45a9948f0854d8dbd81513edd61c49141a873be7c0eb6f6d0c6d24eb7f`
- `r3-final-restore-20261010.serial.log`: `327442dcac5e74dc7583d7e231e4a7ef9e17fc1e5560c7655c542debf7cba09a`

Follow-on withdrawal/restoration run is **IN PROGRESS**, not PASS:
R2 started four background endpoint pings to .2–.5, each interval 0.01,
repeat 60000 and `timeout 900`. Device UTC start approximately
2026-10-10 00:50:16, uptime 57833.81 (device clock, not assumed synchronized).
Each log `/tmp/ecmp-failover-20261010-b/p{2,3,4,5}.log` retains start/end
UTC, uptime, command exit status and replies. All four processes and early
TTL-63 replies were observed; no completed summaries yet. Startup transcript
is `build/dpdk-installable/r2-failover-start-20261010.serial.log`.
Next move UART promptly to R3, check traffic is still increasing, record
baseline and perform bounded next-hop removal/restoration with finally-style
route restoration. If endpoint traffic has ended, do not claim a failover
window; restart a separately identified run. Final endpoint logs must be
recovered and validated before acceptance. No interface shutdown or reboot
has been performed in this run.

R2 ingress → R3 → R4/R1 passes scoped manual-CLI ECMP functional
qualification. Four endpoint logs validate 4000/4000 replies, zero loss,
TTL 63. R3's post-run snapshot compared to the **last** setup snapshot
(after the separate 400 failed local-output probes) yields:

| R3 interface / role | Before RX / TX | After RX / TX | Delta RX / TX | Drops before / after |
|---|---|---|---|---|
| port4 / R2 ingress | 75304 / 74524 | 79304 / 78524 | 4000 / 4000 | 880 / 880 |
| port1 / R4 egress | 73336 / 73321 | 74336 / 74321 | 1000 / 1000 | 215 / 215 |
| port2 / R1 egress | 1201 / 1301 | 4201 / 4301 | 3000 / 3000 | absent / absent |

FIB aggregate packets increase 400→4400; both equal-weight next hops
remain resolved with correct peer MACs. Both physical egresses carry
request/reply traffic, distribution 25%/75% for these four destinations.
This is interface-delta evidence, not directly sampled per-bucket counters
or statistical balance across many flows. No interface drops increase;
port2 has no displayed drop counter. Historical error counters are retained,
not reset; absence of a matching pre-run errors snapshot prevents a claim
that all node error counters were unchanged.

Post-run evidence `build/dpdk-installable/r3-ecmp-post-20261010.serial.log`
SHA256 `417c784d1104928ae517a5e48f85c494c5f811baa5c844a4efefb5c55cbb2fbb`.
Baseline SHA256 and four endpoint hashes are recorded below. CLI state is
temporary. Physical path withdrawal/restoration, restart/replay, measured
PPS/throughput/CPU/latency baseline, FRR/DPA-driven programming and strict
cold-boot artifact identity remain OPEN; this does not upgrade the unified
release gate or certify line rate. Next start bounded endpoint traffic on
R2, then move UART to R3 for controlled next-hop withdrawal/restoration.

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

### Physical ECMP preparation history (superseded by checkpoint above)

2026-10-10 R2 four-target transit run: verified MAC `00:1f:7a:69:f5:ec`,
source `10.10.0.2`, resolved route `30.30.30.0/24 via 10.10.0.1`.
Four concurrent VPP pings (.2–.5, interval 0.01, repeat 1000 each) completed
1000/1000 each, 4000 total, zero loss. Original per-target logs were captured
on R2 in `/tmp/ecmp-20261010-a/`, transferred compressed over UART, and
SHA256 matched against the device. All four pass the strict endpoint-log
verifier (complete sequences, correct destination, TTL 63). This proves
endpoint transit delivery, not yet two-path distribution: collect R3's
post-run port counters against the preceding setup snapshot next.
Elapsed throughput, line rate, recovery, and FRR/DPA programming remain
unqualified. Local logs under `build/dpdk-installable/`:

| Log | SHA256 |
|---|---|
| r2-ecmp-20261010-p2.log | 713e1d64736dfc379c7ef88d18052af1291375eae428250687bd34f51f36504a |
| r2-ecmp-20261010-p3.log | d4a66aa79d1d5580b71c30bf358b08ccd053ad11de70c6182d8d08bd25b1d5f1 |
| r2-ecmp-20261010-p4.log | be4172fbbed7e87e5d1c1f957f325662d1795f3fc95f1d7117fe0afb564f5033 |
| r2-ecmp-20261010-p5.log | 4a7e06188ea6655f3f2bdadd779751f9e27e89d6806a7c33d821ba5df46ac5ef |

2026-10-10 R3 ECMP setup: identity MAC `00:1f:7a:69:f7:4c` and
three peer neighbors verified. CLI installed `30.30.30.0/24` via
`10.20.0.2 GigabitEthernet1/0/0` and
`10.30.0.2 GigabitEthernet2/0/0`. FIB shows both paths resolved,
weight 1/preference 0 and two load-balance buckets.
Four local-output pings (.2–.5, 100 each, source port4/10.10.0.1)
reported 0/400 replies; port1 and port2 TX each increased by 200,
RX unchanged. This is outbound local-output distribution only, not
transit ECMP PASS. Peers have return routes to R2's 10.10.0.2/32,
not this diagnostic source 10.10.0.1; the missing matching return route
is a likely explanation, not a packet-trace-confirmed root cause.
Next run traffic from R2 with its configured return address; preserve
the installed ECMP route and do not reboot temporary CLI state.
Evidence `build/dpdk-installable/r3-ecmp-setup-20261010.serial.log`
SHA256 `acf04eacea3e92937358135953d5dae692d85ed07f024fac0a5695917112a95f`.

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
