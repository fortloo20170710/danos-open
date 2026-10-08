# Outstanding work plan

Derived from the v1.0 architecture review and the 2026-10-04 hardening pass
(`docs/security-and-correctness-hardening-2026-10-04.md`). Ordered by
dependency, not by severity: several items below are blocked on the two items
at the top.

Status legend: `[ ]` open · `[~]` in progress · `[x]` done · `[!]` blocked

### 2026-10-09 latest non-serial verification

On clean pushed HEAD `947a600`, CTest passes 55/55 and the deterministic
backend contract passes all three stages. The strict current-evidence QEMU
verifier passes the full FRR/VPP restart and route lifecycle plus 4x1000 ECMP
soak (4000/4000, zero loss, bucket deltas 3000/1000). The accepted VMware
polling baseline revalidates at 2000/2000, zero loss, 98.23 pps, p50/p99
206/449 us; it remains low-rate regression data only. Host PCI preflight
returns structured schema-v1 `SKIP`/`ENVIRONMENT-OPEN` because there is no
target BDF, PCI Ethernet function, or hugepages. CI, Coverage and Interop Gate
all pass on this head (`37824042975`, `37824042760`, `37824042980`), including
hosted VPP and netns/ping jobs.

The physical installer attempt shown in the user's console photo stopped at
the fail-safe confirmation guard; the disk was reported untouched. The image
identity is unknown and no physical installation is accepted. Do not use the
UART while it is assigned to another assistant. Resume physical install/DPDK
qualification only with the correct clean installable ISO, explicit disk
erasure authorization, released UART, and a real traffic generator/peer setup.

### 2026-10-09 physical identity correction and next execution boundary

The latest physical evidence separates two systems that were previously
conflated. The DANOS-Open r5 traffic ISO booted successfully into LIVE with its
VPP CLI socket and two I211 DPDK interfaces UP. A later UART prompt was a
different machine, hostname `R2`, running DANOS Lancaster 2608; its four I211
functions were bound to `uio_pci_generic`, and a routed ping to the development
host succeeded 5/5. R2 has no VPP socket/CLI and is not a DANOS-Open artifact,
so its reachability is useful physical-cabling evidence but cannot close this
project's VPP/FRR or install gates. The UART is now in use by another assistant
and must not be accessed until released.

The route/NH/NHGroup contract, current-source QEMU FRR/VPP lifecycle, 4-flow
ECMP regression baseline, and deterministic CTest are already passing. Keep
the remaining lane truthful: physical DANOS-Open install/cold-boot and
VPP/DPDK dataplane/performance remain open; use only the clean r8 installable
ISO for disk installation and the r5 traffic ISO for LIVE traffic. Next safe
work is local contract/recovery verification and plan/status synchronization;
resume physical testing only after the UART owner releases it and the intended
ISO/target identity is confirmed.

This turn also caught and fixed a stale default in the standalone VMware
verifier: it had selected old serial logs/ISO despite the unified gate using
the accepted clean baseline. The verifier defaults now match the clean ISO and
clean DANOS/peer logs; a CTest configuration assertion prevents regression.
The default command passes with 2000/2000 packets, zero loss and 98.23 pps;
full CTest is 53/53, while the stricter line-rate/ECMP-performance claim stays
open.

### 2026-10-09 follow-up: current-source contract/runtime recheck

On clean `b86aa6d`, local CTest passed 53/53 and the three-stage backend contract
gate passed (DPA 11/11, programming pipeline, composite Route/NH/NHGroup). The
strict identity-bound QEMU verifier passed its four-flow soak, next-hop
withdraw/restore, BGP/OSPF and FRR/zserv/VPP restart/replay checks. VMware's
default clean-evidence verifier independently passed 2000/2000 with zero loss
at 98.23 pps; this remains low-rate polling packet regression, not high-load
ECMP or line rate. CI, Coverage and Interop Gate also pass (`37819360359`,
`37819360271`, `37819360543`), including hosted VPP V1–V5 and K4/K5 netns/ping.

No serial was accessed in this follow-up. These results close no physical gate:
the generic installable r8 image has QEMU install/cold-boot evidence, but
physical installation is unverified; the physical PCI I211 64-byte DPDK lane
still requires an identified runner plus actual generator measurements.
Continue keeping those as separate open acceptance items.

The follow-up code change `b3efd92` fixes a preflight integration inconsistency:
the host DPDK lane emitted schema v1 while the container lane omitted the
version field. Both missing-BDF paths now share a regression test registered
in CTest; local CTest is 54/54. This makes external runner output machine-
consistent but does not supply the still-missing real traffic measurement.

`1008898` closes the consumer side of that schema seam: the DPDK measurement
recorder now rejects a missing or unsupported preflight schema, and its
compatibility tests run under CTest. Full local CTest is 55/55; PCI hardware
and real-generator performance remain open. Commit `c77bb32` also ensures
validation failures themselves retain the current schema-v1 envelope.

The pushed documentation head `73855dc` is clean and all three GitHub workflows
pass: CI `37822407128`, Coverage `37822407089`, and Interop Gate `37822407155`.
The latter completed hosted VPP V1–V5 plus K4/K5 netns/ping successfully. This
closes the remote validation follow-up for the preflight schema work; it does
not change the physical PCI/traffic-generator qualification boundary. Serial
is currently occupied by another assistant, so no UART access, reboot, or
physical installation was performed in this follow-up.

### 2026-10-08 71361f5 current execution update

The clean code/workflow commit is `71361f5d5ef833c7693a44ce70aa79fe09aae0bd`;
subsequent `main` commits through `dd7f2bc` are documentation-only. GitHub CI,
Coverage, and Interop Gate pass on the code commit and a later docs-only commit;
local CTest is 52/52. The Interop failure on `3881730` was isolated to VPP
26.06 probing a host NIC before the loopback fixture; disabling
`dpdk_plugin.so` in this software-only job fixed the index collision, and the
next workflow run passed.

A clean, identity-bound ISO at `3881730e6b79c1dac41f58b767b86df5069f2d07` and
isolated three-VM QEMU topology passed the strict lifecycle gate: BGP/OSPF,
ZAPI add/withdraw/restore, ECMP path down/up, FRR/zserv restart, VPP
restart/replay and post-restart packets. Four 1000-packet flows were lossless
(4000/4000), 81.47 pps at 10 ms interval, RTT p50/p99 371.10/1694.50 us, and
3000/1000 bucket deltas. This is a comparable QEMU functional baseline, not
PCI throughput. Frozen evidence
is under `build/qemu-frr-vpp-topology-705c70b-baseline/accepted-final/` and
indexed in `docs/v0.16-acceptance-matrix.md`. Physical PCI/DPDK remains open.

After the user confirmed serial and Ethernet leads were connected, the earlier
60-second and a fresh 180-second passive `/dev/ttyUSB0` capture both received
0 bytes; the latest `build/physical-current-20261008T1016Z.serial.log` records
`SKIP` for missing `DANOS-INIT-ENTER`. No reboot, host-network change, or disk
write was performed. Start serial capture before the next target cold boot,
then check both peer links/carriers and configure two independent peer paths
before attempting traffic/ECMP. Earlier r5 physical testing passed I211
bind/runtime preflight but failed traffic because the second path/peer return
path was unavailable. Do not infer hardware qualification from QEMU or no-byte
serial output.

### 2026-10-08 v0.16 runtime and release engineering closeout

The tested source `b32204707ff8705bf47c134b5717bbb996316a3c` has 52/52 local
CTest passes. The clean QEMU e1000 artifact
`build/danos-vpp-dpdk-e1000-2port-ecmp-soak-1000-b322047.iso` is bound by
SHA256 `dcce27ec3e49d8efcd4194c00e61eb92c0dcbd8fc606e47bd40fc339102512c1`.
The strict FRR/VPP topology verifier passes, including BGP/OSPF readiness,
ZAPI route add/withdraw/restore, FRR/zserv restart, VPP restart/replay,
post-restart peer packets, ECMP next-hop down/up and a 4 x 1000 packet soak.
The soak delivered 4000/4000, 0% loss, 81.58 pps, bucket deltas 3000/1000;
RTT p50/p99 were 373.10/1689.50 us. This is QEMU functional regression data,
not PCI throughput. The artifact, result, topology manifest and log hashes are
recorded in `docs/v0.16-acceptance-matrix.md`.

The unified release gate also passes backend contract, CTest, live-console/
mgrd restart, QEMU FRR/VPP and VMware VMXNET3 polling (2000/2000, 0% loss,
98.23 pps). It remains `FAIL` with `dpdk_status=ENVIRONMENT-OPEN`: the runner
has no target `DPDK_PCI_BDF` and zero hugepages. No waiver was applied. The
physical I211 UART was passively captured for 180 and 120 seconds with no
bytes; a further 180-second capture on 2026-10-08 also received zero bytes
(`build/physical-current-20261008T0758Z.serial.log`, UTC). The development host's
`enp4s0` has carrier but is configured as `192.168.71.1/24`; an exploratory
ping to `192.168.71.2` received no replies and does not establish the target's
address or link-layer identity. Current physical boot identity, PCI forwarding
and throughput therefore remain unverified. The next release-close action is
to capture a physical cold boot and complete a supported PCI runner lane, not
to promote QEMU values.

### 2026-10-08 DPA conformance lifecycle assertions

The previously thin VRF, ACL and QoS conformance cases now read back and
compare object payloads, reject duplicate create, verify updates, and confirm
delete plus missing-object behavior. ACL coverage includes table lifecycle and
a full rule payload add/read/duplicate/delete cycle. These are storage/API
contract checks, not backend forwarding qualification. Full Debug, ASan and
TSan CTest suites each pass 48/48. ASan passed in a serialized rerun after a
transient daemon shutdown timeout during concurrent sanitizer suites.

### 2026-10-07 r5 QEMU learned-route lifecycle closeout

Closed the same-prefix real FRR-learned add/withdraw/restore/post-FRR-restart
packet gate with a fresh clean shared-L2 ISO and isolated three-guest QEMU
topology. Strict verification passed on live logs and their frozen accepted
copies; four-flow ECMP delivered 4000/4000 packets with zero loss, 3000/1000
bucket deltas, and VPP restart/replay passed. Debug, ASan and TSan full CTest
are each 48/48. Reproducibility identities and artifact digests are in the
acceptance matrix. This closes the QEMU functional learned-route lifecycle
item, not transit throughput or hardware qualification. VMware host modules
and real PCI/DPDK performance remain open external lanes; keep them reported
as ENVIRONMENT-OPEN and do not promote QEMU measurements to line-rate claims.

### 2026-10-07 acceptance harness hardening

The dynamic FRR log gate now requires separate add and delete commands, a
positive withdrawal count with no sweep failures, and one successful bridge
summary. Zero withdrawals, one-sided events, processing failures and duplicate
summaries are rejected by a new regression test. Its successful output is
`OBSERVED`, not full runtime acceptance: independent VPP FIB checks and a
current-artifact topology are still required. QEMU launch output is `STARTED`,
not `PASS`. Debug CTest passes 42/42; shell syntax and diff checks pass.
These deterministic checks do not close FRR/restart, packet or PCI gates.

The LIVE builder now compiles `fib_live_bridge` alongside `danos-mgrd` inside
the same trixie container and copies that fresh binary into the initramfs.
Host build fallbacks have been removed: they could import stale sources or a
newer host libc ABI. A dedicated current-source e1000 ISO build and separate
FRR disk copies were started for topology qualification. Build startup is
not evidence of successful ISO generation or runtime acceptance; a dirty
source identity must not be promoted to the clean release performance gate.

Topology verification now reads the manifest without executing shell content,
hashes the actual ISO, and requires exactly one clean DANOS boot whose commit
and ISO name match the manifest. Missing/mismatched/dirty identities, duplicate
manifest keys, changed ISO contents and concatenated boot logs fail closed.
This identity check precedes the existing lifecycle assertions. Debug CTest
passes 43/43, including rejection fixtures; it does not itself prove runtime
forwarding. The dedicated build remains a diagnostic build until regenerated
from a clean, matching commit.

The first trixie Release build failed on GCC 14's `-Wformat-truncation` in
`resolve_rpc_path`. Origin copying now uses the fixed-size path field and
explicit termination; both mgrd and fib_live_bridge rebuild successfully in
the preserved trixie environment. Debug CTest remains 43/43 PASS. The ISO
builder now exposes CMake output and fails immediately if its build container
exits. `DANOS_ISO_BUILD_IMAGE` can select a pre-provisioned trixie build image;
source targets are still rebuilt and dependencies checked, never taken from
host build outputs. ISO generation and topology runtime remain unverified.

Follow-up: clean source commit `6d4563a8d8a577dbb2e684fec5ddb99b546c45d0`
produced `build/danos-clean-frr-20261007.iso` (193986560 bytes), SHA256
`1ca996ad5879aef862af954690f08f272fa02e310b284471340427d09f1d4349`.
Build returned zero. The fresh bridge is included; the optional gnmic demo
is omitted because no gnmic binary was supplied. Three dedicated QEMU guests
were launched under `build/qemu-current-frr-20261007`, using separate port
31001/31002/32001/33001/34001 segments and snapshot copies of the historical
FRR disks, with fresh cloud-init seeds. The manifest binds this ISO/commit.
Startup is **not PASS**: lifecycle, four-flow 1000-packet soak, independent
identity verification and recovery assertions are still pending runtime
events. Physical serial and other assistants' guests were not used.

Runtime observation: ISO content and clean boot identity verification passes;
VPP API/stat sockets and both e1000 ports are ready. FRR-2 emitted dataplane
readiness, while FRR-1 remained before cloud-final with the reused disk's long
route-cycle oneshot already starting. The fresh traffic listener was not yet
ready. Seeds now runtime-mask that old service in bootcmd (nonblocking stop),
then unmask it only after fresh dataplane provisioning in runcmd. Revised
artifacts are prepared under `build/qemu-current-frr-20261007-r2`; both cloud
config YAML files parse, shell syntax and diff checks pass. They have not yet
been booted, so the ordering repair remains runtime-unverified. Original
guest PIDs remain live and are not restarted for an observation timeout.
Disk headroom dropped to approximately 1.8 GiB; do not launch additional
builds or broadly prune another assistant's artifacts.

The original run subsequently progressed without intervention: FRR-1
readiness passed before the 180-second deadline, both path pings passed,
all four three-packet ECMP probes were loss-free, and bucket deltas were
11/3. The 1000-packet-per-flow soak started. Thus the earlier provisioning
delay was not a terminal failure in this run; do not attribute successful
traffic to the revised, unbooted seed or restart the original guests.

The same original topology completed the four-flow soak (4000/4000, zero
loss, 49090 ms, 81.48 pps, buckets 3000/1000), ECMP next-hop withdraw/restore
probes, and an actual VPP restart/replay of four objects. The current adapter
recorded `ecmp-result.env` under that topology, including complete-sample RTT
p50/p99 407.30/2924.30 us and a clean ISO identity. CPU/Mbps are unmeasured;
PCI performance remains ENVIRONMENT-OPEN. Full verifier next stops on the
pending FRR route-cycle add marker; BGP add/withdraw and OSPF Full were already
observed. Guests remain live for the scheduled FRR cycle and daemon restart.

At approximately seven minutes the original guests remain active; full
verification still lacks the FRR route-cycle markers. Source inspection
found that route-cycle script and unit generation appended to pre-existing
files on reused disks (unlike the other explicitly truncated generated
files). These two writes now replace their files, preventing duplicated
script bodies and multiple oneshot ExecStart entries on subsequent boots.
Shell syntax and diff checks pass. This is a definite provisioning
idempotency defect, but no guest-side inspection yet establishes that it
caused this run's delay; the fix has not been boot-qualified. Do not count
pending FRR completion as PASS or restart live guests based only on elapsed
observation time.

Seed-generation verification: `FRR_SEED_ONLY=1` regenerates seeds against
existing disks without copying them, and refuses a topology whose recorded
guest PID is live. The idle r2 artifacts were regenerated successfully;
attempting the mode against the original live topology was rejected with
exit 2 before seed replacement. Both actual cloud-init route-cycle generation
commands were executed twice in an isolated temporary directory with systemctl
stubbed: exactly one script body, one registration delay and one unit ExecStart
remained. This proves generation idempotency, not guest recovery. Original
guest processes and completed route-cycle evidence remain intact; the FRR
restart phase is still pending. No physical serial or external VM was touched.

FRR recovery evidence is now enforced beyond the service PASS marker: an
ordered restart begin/request-success/completion, latest zebra disconnect
followed by successful re-registration, positive successful VPP replay and
subsequent route notification/programming are required. Zero-object replay,
unrecovered later disconnect and post-reconnect processing errors fail the
gate. Debug CTest 45/45 passes including negative fixtures. These are log
ordering checks, not an independent post-restart FIB/packet inspection.
The original running topology has not yet emitted FRR restart begin; it is
not qualified by the new deterministic tests.

Independent post-VPP-restart inspection via this topology's own QEMU HMP
keyboard obtained CLI `show ip fib table 0 198.51.100.0/24`. A bounded
FIB-CAPTURE-BEGIN/END block after the restart PASS contains the exact prefix,
API refs:1 and dpo-drop ip4, consistent with this static blackhole test
prefix. The assertion passed against real output, not bridge programming
logs. This does not prove forwarding through FRR-learned next hops, or the
still-unobserved FRR daemon restart. Only the dedicated guest was inspected;
no physical serial was accessed.

The original FRR log subsequently emitted a second full route-cycle
add/withdraw/restore/cycle-done sequence. This confirms repeated cycle
execution in this run, rather than a dead guest; daemon restart remains
pending behind it. The original guests were not restarted.

A clean current-source VMware polling ISO also completed build:
`build/danos-vmware-current-20261007.iso`, commit
`850f60f55c84ecc610ca0f8e51fb30b11fe94bf6`, source_dirty=0, SHA256
`a900904bb06741f54e1faead345c423c919a42245bff76280bd7c731f067f523`.
The project runtime profile retains VMXNET3 BDFs 0b:00.0/13:00.0 and
no-rx-interrupts. No VMware guest was started yet, so build completion is
not runtime/packet PASS. vmrun was available and reported zero running VMs
at inspection. Existing project VM configurations remain untouched.

The original FRR restart eventually emitted BEGIN/request rc=0/PASS and
the bridge observed EOF/disconnect/reconnect. **Full recovery gate FAIL**:
the strengthened verifier caught three `ZAPI command 31 transaction failed:
EXISTS` messages after re-registration. Native FRR add dispatch currently
calls route_create, including blackhole routes, so replay of existing desired
routes is not idempotent. Do not promote the service marker to full PASS.
Next fix must handle route replace/replay and NH/NHGroup ownership/cleanup
transactionally, with tests before a fresh runtime replay.

Matched current-source VMware images were also generated (no guests started):
`danos-vmware-current-r2-20261007.iso`, SHA256
`9157a6ea71481601714281e1842c0e0b1021f7c6bf758c4009ec7fdd66c082aa`,
and `danos-vmware-peer-current-20261007.iso`, SHA256
`5f7b31cf396eeb8f5f58b5895ae9c39c3df7afd5b9e987fd0b3f009dd1a6a7a6`.
Both originate from clean a7fec40; DUT uses 192.168.45.3/46.3 with dynamic
neighbor resolution, peer 192.168.45.2/46.2 with 1000 probes per target.
The first default-address VMware ISO is not a matched-topology result.

Native FRR dispatch now reads candidate/committed route state before add:
existing routes update instead of returning EXISTS. Mapper-private NHGroup
and NH identities remain stable on replay; removed ECMP paths, blackhole
conversion and route withdrawal retire the old graph in the same transaction.
Repeated withdraw of an absent route is a no-op. New tests cover candidate
replay, committed replay, 2-to-1 path shrink, blackhole conversion, restoration
and full dependency cleanup. Debug CTest 45/45 passes. This closes deterministic
mapper behavior, not the failed running ISO: rebuild and real FRR restart
qualification are still required. Externally shared graph ownership and the
legacy clean-room route mapper are not expanded by this native-FRR change.

The fix is in clean commit `a783c73`; ASan and TSan mapper/e2e subsets each
pass 3/3. A fresh trixie build produced
`build/danos-frr-replay-fixed-20261007.iso`, SHA256
`4a28468d948fc8e2fdf9848d2ffb19a03353bb7b022ca28b3d03f8b10400da27`.
Old failing-run QEMU PIDs 527171/527184/527194 were stopped after confirming
their owned topology paths; logs and disks were retained. Corrected seed
generation was reapplied to idle r2, then new PIDs 577191/577204/577215 were
launched under `build/qemu-current-frr-20261007-r2`. Manifest matches the
clean fix commit and actual ISO digest. Runtime validation is pending; no
restart PASS is inferred from the deterministic checks or launch success.

VMware preflight found that matched-interface DUT/peer r2 images alone do
not enable the existing four-destination ECMP gate. LIVE peer profiles now
optionally serialize `DANOS_PEER_EXTRA_ADDRS` (space-separated CIDRs on eth0)
and `DANOS_PEER_RETURN_GW` (return path for 30.30.30.1/32). Both default empty;
normal management and physical runner networking are unchanged. The next
VMware peer profile needs 30.30.30.2–5/24 and return via 192.168.45.3, while
the DUT must enable the existing traffic/ECMP test. No gate is weakened to
accept mere local-address pings; these new profiles remain runtime-unverified.
The current QEMU r2 already reports peer readiness and is running its soak
without an observed transaction failure at this stage.

Current full-profile VMware ISO generation completed from ceaf482:
`danos-vmware-ecmp-20261007.iso` SHA256
`97b55371537c6508ee259a56bcc4c45a1e405a342cf99062baa2036ba341b1ee`,
peer `danos-vmware-ecmp-peer-20261007.iso` SHA256
`53e275811b1a3cad097eca97437de893ed8e0c71b43e18f710a81f1447a4daa5`.
Dedicated VM configurations are under `build/vmware-current-20261007`;
historical VM configs are unchanged. vmrun start failed before guest boot:
the VMX diagnostic log reports missing /dev/vmmon, and modinfo reports no
vmmon module for host kernel 7.0.0-38-generic. Noninteractive sudo modprobe
requires authentication. VMware is ENVIRONMENT-OPEN, not packet PASS; host
module installation/loading requires privileged environment preparation.
Only two owned obsolete unpacked initramfs trees were removed to free about
1 GiB; retained ISO contents allow re-extraction. No logs/disks were removed.
The isolated QEMU r2 continues running independently.

Final r2: full strengthened verifier returned zero on live logs and frozen
`build/qemu-current-frr-20261007-r2/accepted` copies after actual FRR restart,
zebra disconnect/reconnect and route replay. No replay EXISTS occurred.
Frozen hashes: DANOS log
`b8727c082c165652da435691ea506a6a1eff6f43f642683ebc31afd211d70e7d`,
FRR-1 log `04a945767299b7c706fbab8e308f9a916795fb26dd183bc6739fcaaac7a5219d`,
ECMP result `41e338d7bd24cd4300586ff31406aa39a5b652904c5da1275c49e20612fe3daa`.
Debug/ASan/TSan full suites each 45/45 and deterministic/mock contract gate
PASS. QEMU control-plane/functional recovery qualification closes for this
clean a783c73 ISO. VMware and real PCI remain open, and neither line rate nor
independent packet forwarding through FRR-learned paths is inferred from the
CLI-seeded ECMP packet lane. The full project goal remains incomplete.

VMware host preparation follow-up (2026-10-07): both vendor source archives
from `/usr/lib/vmware/modules/source` compiled successfully, without source
patches or system installation, against installed kernel headers. Artifacts:
`build/vmware-modules-20261007-5MG2Kf/vmmon-only/vmmon.ko` and
`build/vmware-modules-20261007-5MG2Kf/vmnet-only/vmnet.ko`. Both report vermagic
`7.0.0-38-generic SMP preempt mod_unload modversions`, matching the running
kernel. Missing vmlinux only skipped BTF generation; both builds returned zero.
This closes the local compilation prerequisite, not module loading or VMware
packet acceptance. `sudo -n /sbin/modprobe vmmon` still requires interactive
authentication. An administrator must install/load these modules and restore
VMware host-only networking before the dedicated DUT/peer VMs can run. If
Secure Boot rejects the modules, administrator-managed signing is also needed;
do not disable host security automatically. Physical serial remains untouched.

Post-restart packet follow-up: live r2 VPP inspection found both interfaces
down without addresses; a real ping reported no source address and sent zero
packets. Historical frozen PASS covers the older replay/reconnect gate, not
post-restart forwarding. Fix 4cd9585 restores LIVE-owned interface/address/
neighbor fixtures after VPP restart and requires both peers 3/3 before marking
restart PASS. Ordered evidence tests reject probes before restart, after PASS,
latest incomplete restart, failure markers, zero packets and packet loss.
Debug CTest remains 45/45. The strengthened verifier correctly rejects the old
frozen log, which remains unchanged. Rebuilt clean ISO:
`build/danos-post-restart-20261007.iso`, SHA256
`b8fb6ad44afd7077879bd5c2f07ade75e14e21765f233e47d2303fdc94d26c79`.
Fresh r3 topology is running under `build/qemu-current-frr-20261007-r3` with
snapshot overlays backed by retained r2 disks (no large copies). Only the
three owned, now obsolete r2 processes were terminated; logs/disks remain.
r3 runtime acceptance is pending; fixture restoration does not prove DPA
interface persistence or forwarding through FRR-learned paths.

r3 final: live and frozen `accepted/` logs both pass the strengthened full gate.
Actual VPP restart restores LIVE fixtures and probes both peers 3/3 before PASS;
FRR restart, zebra reconnect and subsequent positive replay/programming pass.
ECMP result: 4000/4000, zero loss, 48360 ms, 82.71 pps, buckets 3000/1000,
RTT p50/p99 346.20/762.00 us. ASan/TSan full suites each 45/45; latest stricter
ECMP-result/recovery subsets 2/2 in Debug/ASan/TSan. Deterministic contract PASS.
Frozen SHA256: DANOS log
`7ddb7f8cb50d6936b8b94b72d89613b6a19f0c8bb0ee8546ddfdb028e30d3b0c`,
FRR-1 `acefeb7e9f9773bc4bd7f1bf030ea7605678564ab2aeb56ef3fca0e0340fbba4`,
result `bd7465fcd0ca46e9880107ba63016456f8d67357524aede4bd1503919090c407`.
Only the obsolete owned `iso-clean-frr-20261007/initramfs` extraction was removed
to reclaim 541 MiB; its ISO, logs and disks remain for recovery/re-extraction.
Scope limits above remain: no FRR-learned packet proof, VMware or PCI qualification.

Learned-route forwarding diagnosis: current r3 detailed VPP FIB for
198.18.0.0/24 is API-owned but its forwarding bucket is `arp-ipv4` via
172.31.0.3 on GigabitEthernet0/3/0. FRR-1's ens4 peer link carries
172.31.0.0/24; VPP port 2 instead connects to FRR-2 ens3 on 10.20.0.0/24.
The existing numeric ifindex map does not make these separate L2 segments
equivalent. The original advertised 198.18 prefix is also backed by a
blackhole, not an echo endpoint. Thus adding ping to the current setup cannot
close the learned-route packet gate without fixing topology and destinations.

Next implementation: introduce an isolated shared L2 qualification segment
that gives the FRR peer and mapped VPP port the same reachable next hop;
provide a real endpoint behind an advertised prefix and a symmetric return
path, then require learned API-owned resolved FIB, actual add/reachability,
withdraw/non-reachability, restore/reachability and restart recovery. Preserve
the existing two-port lane as its own historical/control-plane regression.
Do not rewrite learned gateways or forge neighbors to hide a topology mismatch.
New `verify_learned_route_fib.py` is only a resolved-FIB prerequisite, explicitly
not packet acceptance. It checks the latest detailed target entry, API ownership,
expected gateway/interface and rejects ARP/drop; four fixture tests include
wrong gateway/interface, stale entry and an unrelated route's adjacency.
The real r3 diagnostic correctly returns FAIL. Debug CTest now 46/46 PASS;
this unit result does not close the real learned-route qualification.

Shared-L2 implementation (qualification pending): opt-in
`FRR_SHARED_PEER_L2=1` joins DUT port 2, FRR-2's dataplane socket and both
FRR peer sockets on a QEMU hub. All sockets bind/connect only 127.0.0.1;
the original point-to-point layout stays the default. The seed builder records
its profile, and launch rejects a shared-mode/legacy-seed mismatch before
starting guests. Shared-mode FRR seeds suppress weak-host ARP replies and
provide a real local 198.18.0.1/32 endpoint behind the advertised aggregate.
Build the DUT with `VPP_IF2_EXTRA_ADDRS=172.31.0.1/24`; the extra address is
embedded in build identity/network config and restored after VPP restart.
This changes test fixtures, not learned gateways or production route mapping.

Implementation reference: [QEMU emulated hubs](https://www.qemu.org/docs/master/system/devices/net.html).
Four launcher assembly tests cover default/shared wiring, invalid mode and
seed mismatch. A real paused local QEMU accepted three loopback socket hub
backends and a NIC and remained alive; that is network-startup proof only,
not guest routing/packet acceptance. Debug CTest is 47/47 PASS. A fresh clean
ISO and isolated three-guest run are still needed; add/withdraw/restore and
learned-route post-restart packet probes must follow, not be inferred.
Only old owned `iso-frr-replay-fixed-20261007/initramfs` was removed for
541 MiB of build headroom; its ISO, frozen logs and disks are retained.

Shared-mode clean build completed: `build/danos-shared-peer-20261007.iso`,
source 81a3821 (dirty=0), SHA256
`e59bda64ba96d6409c44c1866af9e9ca146492cea544a640210410512ec6a15b`.
Identity reader now exposes `danos_build_if2_extra_addrs=172.31.0.1/24`;
the primary 10.20.0.1/24 remains unchanged. Isolated r4 topology under
`build/qemu-current-frr-20261007-r4` launched all three guests using fresh
snapshot overlays and matching shared-mode seeds. Both FRR guests emit their
shared-L2 marker and BGP readiness passes. The original r3 owned guests were
terminated after their logs were frozen; no disks/logs were removed.
Generated YAML parses correctly; Debug 47/47 PASS, new FIB/wiring subsets
2/2 in both sanitizer trees. Network boot and unit evidence do not yet qualify
the learned-route packet lifecycle; current r4 endpoint probe is in progress.

r4 initial learned-route probe now completed: API-owned 198.18.0.0/24 FIB
resolves via 172.31.0.3 on GigabitEthernet0/3/0; strict FIB prerequisite PASS.
Between FRR-LEARNED-PROBE-BEGIN/END, actual VPP ping to 198.18.0.1 is 3/3,
zero loss. Snapshot `learned-initial/` preserves the log/manifest; it is not
full lifecycle acceptance. Next required work remains withdraw/restore and
post-VPP/FRR-restart learned-route packet probes, plus independent transit
traffic as appropriate. Do not infer VMware/PCI or line rate from this probe.

Learned-route lifecycle follow-up: actual r4 VPP restart completed; dynamic
198.19.0.1 remained reachable 3/3 with API-owned resolved FIB after restart.
After BGP withdrawal, a source-bound probe sent 3 and received 0, with the
target FIB entry absent. After actual FRR/zserv restart, the persistent
198.18.0.1 endpoint again replied 3/3. These are different-prefix partial
checks, not one complete restored-prefix lifecycle. Frozen partial log:
`build/qemu-current-frr-20261007-r4/learned-partial/danos.serial.log`, SHA256
`5e031b13263bcb64bc4c4835f166ff6cb1fe14edc35fbd64856a67f0b9359c0e`.

Implementation for the next fresh run: shared-mode FRR-2 restores its real
198.19.0.1 endpoint/advertisement after a 60-second withdrawn window. New
`run_learned_route_cycle.py --topology ... --run` waits for real events,
checks ownership/liveness of all three guest PIDs, and drives ADD, WITHDRAW,
RESTORE and POSTFRR probes via the owned DUT HMP keyboard channel. Every phase
requires exact three-packet statistics and positive phases require target
replies plus API-owned resolved FIB; withdrawal requires no target entry.
Validation-only mode runs on frozen logs. Shared-L2 topology verifier requires
this full cycle, so r4 cannot pass by borrowing another prefix's restart
result. The tool is VPP-originated functional proof, not transit/PPS.
Launcher now binds git_commit to embedded ISO identity and records runner
commit separately, allowing safe reuse of an unchanged clean ISO with new
seeds/tools. New manifests also hash both seed ISOs; changed/missing seed
content fails identity checks. Fresh r5 runtime subsequently passed the strict
live and frozen verifier; see the closeout above and
`docs/v0.16-acceptance-matrix.md` for identities and digests.

---

## P0 — blocks any real deployment

### [x] 1. Native gNMI mTLS transport and identity authorization

The daemon requires `DANOS_TLS_CERT`, `DANOS_TLS_KEY`,
`DANOS_TLS_CLIENT_CA` and `DANOS_TLS_ROLE_FILE` for production startup.
OpenSSL 3 checks the certificate chain, validity and client-auth purpose;
TLS 1.2/1.3 and HTTP/2 ALPN `h2` are supported. Each leaf-certificate
SHA256 fingerprint maps to an explicit admin/operator/viewer role. A valid
CA signature alone does not grant access, unknown identities are refused,
and bearer/default-role settings cannot elevate a certificate's role.
Get/Subscribe are reads; viewer Set is rejected with gRPC status 7.

Partial/invalid configuration is fatal and cannot fall back to plaintext.
Private keys must have private permissions and role files must not be
group/other writable. Existing bearer and Unix peer-credential APIs remain
for laboratory/local use; `DANOS_ALLOW_INSECURE=1` explicitly opts into
laboratory h2c only when no TLS settings are present. LIVE/kernel/smoke
scripts declare that choice rather than silently weakening production.

`gnmi_mtls` starts the actual daemon with temporary PKI and covers both
supported TLS versions, role-based Get/Set/Subscribe, certificate role
precedence, concurrent sessions, absent/expired/untrusted/wrong-purpose and
unmapped credentials, ALPN/hostname checks, fail-closed configuration,
laboratory opt-in and joined-worker shutdown. Acceptance also fixed the
audit ring and reconciler/metrics shutdown races exposed by TSan.

Deployment instructions and explicit limits: [management mTLS](management-mtls.md).
Local acceptance: Debug, ASan/UBSan/LSan and TSan each pass CTest 39/39.
This closes gNMI's transport/identity gap, **not all deployment security**:
metrics still require isolation/TLS proxy; NETCONF-over-SSH, online rotation
and CRL/OCSP are not included. Certificates/roles/CA reload on restart.
No production certificate or private key is committed.

### [x] 2. Transaction engine provides staged atomicity

All public DPA CRUD operations now use the transaction passed by the caller.
Create/update/delete mutations remain private until commit; reads consult the
transaction overlay first (including route dumps), so read-your-writes works
without exposing candidate state to other transactions. `CREATE`, `UPDATE`,
and `DELETE` have distinct existence rules. Abort and failed conflict checks
discard the complete staged set.

Commit compares the captured base payloads while holding the object-store
write lock, preallocates replacement storage, writes the transaction records
and a WAL commit marker and fsyncs before publishing the in-memory batch. A
conflict or allocation/WAL failure leaves the object store unchanged. WAL
recovery applies only complete marked transactions; the legacy immediate
object-registry API remains available for internal recovery paths. Existing
callers that relied on `begin -> mutate -> commit` were migrated to the
documented prepare/validate/commit lifecycle rather than weakening the state
machine.

Coverage: private visibility, read-your-writes, create/update/delete abort,
same-key conflict, multi-object all-or-nothing conflict, route-dump overlay,
restart recovery, and an uncommitted transaction WAL record. Full CTest
acceptance is recorded in the current project status.

#### Findings from the two reverted attempts (historical)

Two attempts were made and both were reverted after the test count went red
(12/38 and 13/38). Nothing from either is in the tree. They established the
following, so a third attempt starts from here rather than from scratch:

- **The 51 CRUD entry points are uniform.** Every one is
  `(void)tx;` followed by a single
  `danos_object_create/update/delete(get_default_store(), TYPE, id, ...)`.
  Thin `crud_create/crud_update/crud_delete` wrappers plus a textual rewrite
  converts them; the `tx` parameter is already threaded through and only has
  to stop being discarded.
- **Staging requires a read overlay, not just a write overlay.** Reads are 12
  sites of the same shape. Without `crud_read` consulting the staged set, the
  very common `create` then `read` inside one transaction fails, because nothing
  is in the store until commit. This was the main cause of the failures.
- **Existence probing must pass a real `out_size`.** The store signals "it
  exists, and here is the size" by returning `DANOS_ERR_INVALID_ARG` when
  `out == NULL`; a NULL `out_size` is a plain argument error. Passing
  `NULL, NULL` silently makes every object look absent.
- **Do not collapse the three operations into a boolean.** A `bool remove`
  turns create and update into one upsert and loses the EXISTS / NOT_FOUND
  distinction that callers depend on. Model the operation as an enum
  (`CREATE` / `UPDATE` / `DELETE`) so each keeps its own existence rule, and so
  that a staged create whose payload allocation fails cannot be published as a
  delete.
- **A staged delete does not make the object present.** The overlay lookup has
  to report `remove` and the read path must treat it as absent.
- **`commit` must tolerate being called from `OPEN`.** Many call sites do
  `begin -> mutate -> commit` and today work precisely because writes bypass the
  transaction. Once they stage, a `commit` that refuses would leave the staged
  set unpublished and silently drop the caller's changes. Either run the
  skipped phases inside commit, or fix all 61 call sites; do not leave it
  strict and assume the callers were updated.
- **The read overlay must be consulted *before* the store.** The first attempt
  checked the store first and fell back to the overlay only when the object was
  absent - which means a staged *update* to an object that already exists reads
  back the stale stored value. That is the same invisibility staging exists to
  remove, so it has to be: "if this transaction staged something for this
  object, that is the truth for it".
- **Staging changes abort semantics, and that is what actually breaks the
  suite.** Today a delete applied the moment it was called, so a later abort
  did not undo it and cross-test state looked as callers expected. With
  staging, an aborted delete leaves the object in the store. Tests that create
  in one transaction and assert in another - or that abort and then expect the
  change to have happened - now see different state, which is the `EXISTS`
  failure the second attempt ended on.
- **So the real cost is not the 51 rewrites, it is the implicit contract.** The
  suite passes today partly because writes are immediate; staging makes that
  contract explicit and breaks every caller that leaned on it. Those callers
  have to be found and given the lifecycle they were implicitly assuming, and
  that inventory is the actual work - not the mechanical rewrite.

The successful implementation followed that sequence: pin abort and visibility
semantics first, migrate dependent callers, then enable staging and WAL commit
markers. The findings above remain as historical context for why the earlier
attempts failed.

Note the recorder fix from the earlier pass: transaction records are now
correctly retired, so this work no longer sits on top of a leak.

---

## P1 — correctness of the claims the project makes

### [x] 3. VPP 26.10 runtime and QEMU FRR/VPP functional lane

The real runtime gate runs against `danos-vpp-runtime-recover:local`
(`vpp v26.10-rc0~545-gad99177fe`). The [real control runtime gate](vpp-control-runtime-gate.md)
covers API registration, stats, default/VRF 777 FIB, add/repeat/NH
withdraw/restore/delete and table deletion. The clean-source QEMU FRR/VPP
topology then passed the full runtime verifier on one identity-bound boot:
actual packets on both paths, BGP and OSPF, ZAPI add/withdraw, route restore,
VPP and FRR/zserv restarts, and post-restart reachability. The 1000-packet-per-
flow ECMP soak and next-hop failover/restore also passed. Evidence and exact
digests are in the acceptance matrix.

This closes the VPP runtime/QEMU functional portion of item 3. It does not close
real PCI DPDK performance, line rate, the physical I211 runtime (the latest
passive serial reads were empty), or prove that QEMU PPS represents hardware.
Those remain distinct environment-dependent acceptance lanes and continue to
block the unwaived unified release gate.

Read-only [runtime capture tooling](vpp-runtime-capture.md) now collects
version/interface/FIB/load-balance/error/runtime snapshots, bounded commands,
raw output hashes, source identity and optional exact ISO/log attachments.
CAPTURED is not acceptance PASS. The legacy run_vpp_verify.sh no longer labels
kernel/mock tests or socket existence as real VPP/DPDK acceptance. The QEMU
FIB/table/NH and topology functional gates described above are closed for the
tested identity; physical PCI behavior remains open. API frame evidence is
item 11.

Historical physical I211 runs include a failed two-port traffic attempt; the
latest physical state is UNKNOWN because serial capture received no bytes.
Do not treat either as current hardware PASS or as a substitute for PCI
performance qualification.

### [x] 4. Capability registry must be wired, or deleted

`capability_registry.c` is 38 lines with no header, no callers, and an
unbounded `for (;;)` in its registration path. `danos_vpp_capability_register()`
is defined but never called, so the registry is empty at runtime.

Done: added `capability_registry.h` and `vpp_capability.h` so the entry points
are declared at all, and the VPP adapter now registers its capability table at
install time. The scan is bounded by `DANOS_MAX_BACKENDS` instead of relying on
`danos_backend_get_info()` eventually failing. A regression test asserts
interface/VRF/route are advertised and EVPN/MPLS/ACL/tunnel/multicast are not,
so the registry cannot be wired to a lying table - verified to fail both when
the register call is removed and when the table claims EVPN.

Still open: backend *selection* (picking among several registered backends by
capability and load, per ADR-0007) is not implemented - the registry answers
questions, nothing consults it to choose. And `type_skipped()` is still a
separate hand-maintained list rather than being derived from the table; the
regression test is what keeps the two in agreement.

### [~] 5. Reconciler must be driven by the daemon

Done so far: `danos_reconciler_run_once()` now invokes the tombstone sweep,
so deleting config actually withdraws it from the dataplane (previously only
reachable from tests). `max_retries`, `backoff_*` and `antiflap_*` are handed
to the programming pass and enforced through a bounded per-object retry table:
exponential backoff capped at `backoff_max_ms`, an attempt limit, and
anti-flap parking, so a route whose next hop has not been learned no longer
retries every 50 ms forever. `total_runs` is counted in both reconciler modes
(it was only counted when a state store existed, which the daemon does not
use), and `reconcile_trigger()` marks state dirty so the next pass runs
immediately. Two regression tests cover withdrawal and the retry limit.

Still open:
- `danos_state_diff_desired_programmed()` is stubbed and `diff_cb` is an empty
  stub — drift detection is reported, not repaired per-type
- OPER does not exist: there is no path from backend state to an OPER view, so
  `DANOS_ERR_VERIFY_FAIL` is unreachable

### [!] 6. HA and CoPP are duplicated and disconnected

`danos-ha` (BFD, VRRP, supervisor — 630 LOC) and `danos-security` (CoPP policer,
RBAC, audit) are each built as standalone libraries with only their own test
linking them. Neither is in `danos-mgrd`'s link line. Blocked on item 1 for
the security half.

ADR-0006 delegates BFD to FRR's `bfdd` while `danos-ha/bfd.c` implements it
again — the duplication needs resolving one way or the other.

---

## P2 — protocol compliance gaps

### [~] 7. gNMI `Subscribe` and `Set` semantics

Done:
- POLL is now a real mode. Streams are tracked per stream id; the initial
  Subscribe sends the set and then the sync_response as **separate** messages
  (SubscribeResponse is a oneof, so the previous single combined message was
  invalid), and a Poll on a known stream answers with a notification only. A
  Poll before Subscribe is refused rather than treated as a new subscription.
  Previously every message on a POLL stream was handled as a fresh Subscribe,
  so each Poll re-sent the entire initial set.
- the STREAM change detector now hashes the raw object bytes (type and id
  folded in) streamed from the store. It hashed only interface mtu and
  ifindex, so a change to an interface name, admin state or address, or to a
  VRF name or a route prefix, metric or next hop, left the hash identical and
  subscribers were never told. It also copied each type into a fixed array
  (ifaces[64], vrfs[16], routes[32]), so a larger store silently lost objects.

- Set now applies delete → replace → update in one staging transaction.
  Interface replacement resets omitted modeled fields, preserves identity,
  and route ECMP resize retires obsolete next hops. Late failure rolls back
  interfaces and complete route graphs together.
- Get returns concrete keyed object paths (route prefix plus VRF), correctly
  filters VRF entries, and no longer uses fixed collection-size caps.
  Supported forms and deliberate exclusions are documented in
  [Set/Get semantics](gnmi-set-get-semantics.md).

Still open:
- `updates_only`, `use_models`, `encoding` are ignored; no bytes/decimal
  `TypedValue`
- STREAM still polls on a 200 ms tick rather than subscribing to store events,
  so latency and CPU cost scale with stream count

### [ ] 8. NETCONF has no transport

Message layer only (~250 LOC): no socket, no SSH, no RFC 6242 framing. It
advertises `candidate`, `running`, `rollback-on-error` capabilities that are not
implemented. XML parsing is `strstr`, so two `<interface>` blocks yield only the
first. `session-id` is hardcoded to 1.

### [x] 9. FRR nexthop types and protocol mapping

Done, against upstream FRR 10.3 rather than by inspection:

- nexthop types now follow `enum nexthop_types_t` (lib/nexthop.h): 1 IFINDEX,
  2 IPV4, 3 IPV4_IFINDEX, 4 IPV6, 5 IPV6_IFINDEX, 6 BLACKHOLE.
  `zserv_encode_nexthop()` writes the trailing ifindex for *both* IPv4 forms
  and both IPv6 forms; the decoder read it only for type 3, so every type-2
  nexthop - the common case for an IPv4 route with a gateway - left four
  bytes unread and desynchronised every nexthop after it. Types 4-6 were
  skipped without consuming their payload. An unknown type is now refused
  rather than decoded as garbage. Address width follows the nexthop type, not
  the route family.
- route protocols now follow `lib/route_types.txt`, the canonical registry
  whose declaration order is the numeric value. The old table mapped 8 to ISIS
  and 11 to OSPF; 8 is OSPF6 and 11 is PIM. ISIS (9) and OSPF (7) were not
  mapped at all, so those routes decoded as UNSPEC and were dropped.
  RIP/RIPNG/PIM now map to UNSPEC explicitly instead of to a nearby protocol.
- the nexthop flag bits are named from `lib/zclient.h`; SEG6 is 0x10 and
  SEG6LOCAL 0x20.

Also done:
- nexthops carrying LABEL, EVPN, SEG6 or SEG6LOCAL are now **refused** with
  NOT_SUPPORTED. They were consumed for length but never interpreted, so an
  EVPN or SR-TE route was installed as an ordinary IPv4 route - silently
  forwarding where it should not, and contradicting the capability table.
- the gateway is copied at the nexthop type's width, not the route family's,
  so a v6 nexthop inside an IPv4 route no longer takes 4 bytes of a 16-byte
  address
- a blackhole nexthop now sets `DANOS_ROUTE_FLAG_BLACKHOLE` instead of becoming
  a nexthop with no gateway inside an NHGroup

Note the file carries **two ZAPI route dialects**: `zapi_map_route` decodes a
simplified layout used by the mock path, while `zapi_dispatch_frr` decodes the
native FRR layout and is what real FRR traffic takes. The nexthop fixes are in
the native path. The simplified dialect has no flags field, so it cannot carry
this information and the two will keep differing until it is retired.

Still open:
- `zapi_client` registration does not request ZAPI_SERVICE_IPV4 via
  `zebra_register_zclient`

### [ ] 10. Missing modules named in the architecture diagram

- `danos-ovs`, `danos-p4`, `danos-platform` are empty directories containing
  only a README. The diagram's "OVS (secondary)" and "P4 (programmable)"
  backends do not exist.
- `danos-models` contains no `.yang` files. The YANG/OpenConfig layer is a
  hand-written C enum plus a `strcmp` table; the generated
  `model_paths_gen.inc` is unreachable because its key convention
  (`"interface[name]"`) does not match the decoder's (`name="interface"` plus a
  separate `key_name`).
- RESTCONF does not exist (only mentioned in `docs/archive/`).
- No web UI.
- Hardware acceleration layer (NIC offload, SmartNIC/DPU, FPGA) has no code.

---

## P3 — engineering hygiene

### [~] 11. Golden-bytes tests against real upstream definitions

The single highest-leverage item for preventing a repeat of §4. Every VPP
encoder should be tested against bytes captured from a real VPP instance or
generated from VPP's `.api` files, and every ZAPI frame against bytes recorded
from a real FRR 10.3 zebra. Today both mocks reimplement the client's
assumptions, which is precisely why three wire defects and a handshake bug
survived a suite that passed 35/35.

This has now been got wrong in both directions, which is the argument for
doing it properly. The `.api` declarations alone are not sufficient: they
describe `string name [64]`, but the two legacy socket control messages are
exchanged as raw structs with a fixed 64-byte name and no length prefix, which
is only visible in `socket_client.c` and `socket_api.c`. Conversely the
`.api` declaration *is* authoritative for regular API messages, where
`admin_up_down` really is a `u8` and strings really are length-prefixed with
no padding. Both facts are recorded in
`docs/security-and-correctness-hardening-2026-10-04.md` §4.5.

Done for both protocols. `danos-fib/tests/test_zapi_golden.c` and
`danos-vpp/tests/test_vpp_golden.c` replace literal payloads with encoders
transcribed from upstream (`lib/nexthop.h`, `zebra/zapi_msg.c`,
`lib/zclient.h`, `lib/route_types.txt`, `api_types.h`, `interface.api`,
`socket_client.c`, `socket_api.c`), so the production decoders have to agree
with an implementation written from the specification. Citing the upstream
source in the file keeps the transcription auditable.

Both were verified against the bugs they exist to prevent: restoring the
type-3-only ifindex read fails the ZAPI per-type test, renumbering a nexthop
type fails the constant check, writing `admin_up_down` as a u32 fails the VPP
set_flags test, and adding field-relative padding to the string codec fails the
string test.

Still open: the fixtures are transcribed by hand from upstream source, not
generated mechanically from it. A generator that reads `route_types.txt` and
the `.api` files would remove the transcription step, which is the remaining
human-error surface. And none of this substitutes for item 3 - a live run
against a real VPP and FRR.

### [~] 12. Test-suite honesty

Done:
- the two vacuous conformance cases now assert. `conf_iface_crud` runs the full
  lifecycle, reads the object back, checks its contents, checks a duplicate
  create is refused with `EXISTS`, and checks the delete takes effect -
  verified to fail when the read-back expectation is disturbed.
  `conf_capability_query` asserts a definite, stable answer rather than
  discarding the status.
- `run_v016_release_gate.sh` no longer lets an environment-restricted lane pass
  silently. An open lane blocks the release by default; the policy lives in
  `release_gate_policy.sh` so it is unit-tested without running every lane, and
  `DANOS_RELEASE_ALLOW_OPEN_LANES=1` waives it deliberately with the waiver
  recorded in the result file. Only the exact value 1 waives.

Still open:
- a production C coverage workflow and fresh-run collector now exist; initial
  local baseline is 77.5% lines / 55.5% branches with CTest 41/41. Remote job
  execution is still blocked by GitHub billing; assertion quality and a
  coverage non-regression policy remain open (see coverage-baseline.md).
2026-10-08: `conf_vrf_crud`, `conf_acl_crud` and `conf_qos_crud` now exercise
and assert readback, duplicate, update and deletion semantics. This closes the
listed thin-case gap but does not address the separate storage-vs-backend
scope limitation.

2026-10-07: Route/NH/NHGroup creation-only cases were replaced by full
storage lifecycle/visibility/abort/payload checks. Pipeline tests now pin
dependency recovery after retry exhaustion, mock VPP replay/withdraw/restore
and restart epoch retry reset. The listed VRF/ACL/QoS gaps remain open.

### [~] 13. Build and repository hygiene

Done: `build-asan/` (57 compiled artefacts, 34 MB) is untracked and ignored -
`.gitignore` covered `build/`, `build-tsan/` and `build-fuzz/` but not
`build-asan/`, which is how it slipped in. `tools/gen-model-paths/gen-model-paths`,
a compiled Go binary committed alongside its own `main.go` source, is likewise
untracked and ignored.

Still open:
- a 41 MB `.docx` is tracked. Removing it from *history* is a rewrite, so it
  needs an explicit decision rather than a drive-by `git rm`
- no SBOM, no artefact signing
- CI installs VPP from the floating `noble` FDio repo; container base images use
  floating tags and `apt-get` installs are unpinned

Partial progress: the new [coverage lane](coverage-baseline.md) pins its GitHub
actions to upstream commit SHAs and its gcovr/transitive Python packages to
versions plus wheel hashes. This does not pin the other workflows, apt, VPP
or container inputs and does not close SBOM/signing/history items.

### [ ] 14. Architecture diagram accuracy

The diagram matches the intent, not the implementation. Three specific
inversions, in priority order:

1. **Capability Model** and **Transaction Engine** are drawn as two of DANOS
   Core's four components. One is unwired dead code, the other has no atomicity.
   These are the project's self-declared core IP (architecture spec §19).
2. **Control Plane (FRR)** lists PIM, LDP, EVPN and SR-TE. None has decode
   support in `danos-fib`. EVPN/SR are promoted to design principle #4 while
   the capability table also advertised EVPN support that did not exist.
3. **DPA row** omits Interface — the only object that works end to end — while
   listing seven that `type_skipped()` skips. **Data Plane Backends** omits the
   netlink backend, which is the one that passed the real kernel ping, while
   listing two empty directories.

Suggested: add a status marker to every box (implemented / partial / interface
skeleton / empty) with `docs/project-status.md` as the single source of truth,
and move Hardware Acceleration to a dashed optional layer.

### [x] 15. gRPC transport-layer defects

All fixed:
- per-stream windows keyed by real stream id instead of `stream % 64`, and the
  slot released when a stream ends
- `SETTINGS_INITIAL_WINDOW_SIZE` applied as the RFC 7540 §6.9.2 delta rather
  than overwriting live windows
- `WINDOW_UPDATE` now returned as inbound DATA is consumed; previously any
  request body over 64 KiB deadlocked the connection
- parked-frame queue made FIFO and bounded at 64 entries
- subscription registry and its queues guarded by a lock, with the ordering
  against the event-bus lock documented; `unsubscribe` detaches, releases the
  lock, then calls into the bus so the free is safe
