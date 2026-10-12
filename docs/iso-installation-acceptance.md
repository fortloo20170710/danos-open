# ISO delivery and installation acceptance

更新时间：2026-10-13

## Latest delivery: r14 BIOS installable lab ISO

- ISO: `build/danos-open-v0.16.0-rc1-generic-dpdk-installable-20261012-r14.iso`
- Size: 571801600 bytes; SHA256
  `43efd75fa2217395fc1741ede0be2d23cebd2d3b8d56500eef82cdbcaaee0523`.
- Application and builder source: clean `28b4a9e6269c1c302a81130f6a487fb2d13714bd`.
  Debian trixie build container:
  `sha256:7c74d058d38512da3cf904738d54c0033d4d78dae306395c368245e388d83c49`.
- LIVE and installed mgrd SHA256:
  `17ddee1d830fb61ec918212f3e7af93e59c34b9a2c0ee85ea8c5ecf2640bf215`.
  The image now includes explicit `--backend vpp` with fail-closed startup.
  Default backend remains netlink; production service stays disabled until
  actual mTLS configuration and backend selection are deployed.
- Generic supported-PCI DPDK discovery/binding retained. Boot `install` for
  VGA or `install-serial` for serial installation. The selected whole disk
  requires the exact uppercase `ERASE /dev/...` confirmation.
- Exact ISO USB boot in QEMU, fresh 5 GiB virtio target, full image write and
  readback digest PASS. With ISO detached, `/dev/vda1` ext4 root, exact mgrd
  digest, clean build metadata, VPP/FRR services and API/stat sockets PASS.
  A genuine kernel reboot with random marker persistence also PASS.
- **Installed `/usr/bin/danos-mgrd`** passes ECMP FIB 2/1/2 path changes,
  fresh VPP process (PID 691 -> 871) with absent-route precondition, fresh
  mgrd WAL replay, withdrawal and no resurrection after another mgrd restart.
  A separate child of that installed disk passes full TLS 1.2/1.3, certificate
  RBAC, negative credentials/ALPN and fail-closed startup checks.
  Test PKI/WAL remain in disposable guests; no test keys are in the ISO.
- Scope: Legacy BIOS, 3 GiB root disk template, lab console autologin.
  ISO-installer UEFI/Secure Boot, target-capacity expansion, fleet deployment, packet
  throughput and live reconnect remain unqualified. ECMP resolution uses
  synthetic loopback/neighbors recreated by the harness.

Local evidence index: `build/install-r14-20261012/qualification.json`,
`install-ecmp-result.json`, `management-mtls-result.json`, `disk-payload.manifest`.
Application package: `build/danos-open_0.16.0~rc1-1_28b4a9e_amd64.deb`, SHA256
`f896c6c7a1bc1b20f0c2bed3a335333e88bc40158f1da8815c0d1659ea42d4f0`.

| Evidence | SHA256 |
| --- | --- |
| `installation-readback.serial.log` | `f6d30e3baa8baccb3229f924fbf9d3ceb962574a2c1e190174346c0756513a74` |
| `installed-ecmp-reboot.serial.log` | `c61bbc5a5419a448b66a19b6bf1b31ede44ce4a22314c440bf68b75071fef401` |
| `management-mtls.serial.log` | `b0e4439509ab54186fae04fb793ee4f29d7b13dbc556ab78f0c9f74207d48989` |

Verifier SHA256: `f14ac6c7d1509552e0f557c92ab3c2c7d121d3b760ca4a998395a55231eff3d8`.
Management test SHA256: `a1e902ebcfb34ba4ac9e7afc9b7414a9417d191928375337533ec99df5f5fa15`.
Current CTest: 70/70 PASS, including R3 physical FIB/DPO and counter-window
evidence verifiers.

### R1 physical installed-runtime qualification — 2026-10-12

R1 (P1 MAC `00:1f:7a:40:28:20`) boots r14 from SATA `/dev/sda1`
ext4 on TS16GMSA370. Clean source and installed mgrd digest match the r14
values above. Four I211 `8086:1539` devices use `uio_pci_generic`; VPP
exposes all four ports. VPP/FRR and API/stat/CLI sockets pass inventory;
mgrd remains disabled pending production mTLS. Installed runtime exposes
`/sys/firmware/efi`: **R1 installed-disk UEFI boot observed**, not an
ISO-installer UEFI or Secure Boot qualification. Physical installation was
reported by the user; its write/readback transcript was not captured.

The existing R1 lab role was restored after installation: P1 `10.30.0.2/24`,
return route to `10.10.0.2/32` via R3 `10.30.0.1`, loopback `.2`–`.5` in
`30.30.30.0/24`. The missing lab replay helper and additive VPP systemd
post-start hook were installed separately; they are not claimed baked into r14.
VPP restart PID `1717 → 1891` restores the role. Carrier polling reached
1 Gbps full duplex after 3.38 seconds; direct readiness probes were 4/5,
then 5/5. Subsequent probes to R3 and through R3 to R2 each passed 100/100,
TTL 64/63 respectively. These timings start after restart returns, not at
the beginning of total downtime. Earlier no-carrier and unconverged-neighbor
failures remain archived; this is not lossless restart, mgrd WAL replay,
two-egress ECMP or throughput qualification. Read-only UART Robot: 2/2 PASS.

Local evidence (under `build/`; hashes bind the archived results):

| Evidence | SHA256 |
| --- | --- |
| `r1-r14-installed-post-restart-20261012/inventory.json` | `8b6d4106d06a0fa4bf12d296524c764083ac5ce3037d5d0723b31c1683554acd` |
| `r1-r14-peer-restart-converged-20261012/result.json` | `f31396a345978857451c3d96936b08b9bb4ca397a0c29da92ba8dc835fe648c5` |
| `robot-r1-r14-readonly-20261012/output.xml` | `7d4ab1e3bc24902328a9398abdaf30fea8dc81901291ece3a5a3f5b4f32c43e0` |

Failed attempts: `r1-r14-lab-restore-20261012` (missing helper),
`r1-r14-peer-restart-20261012` (carrier not ready), and
`r1-r14-peer-restart-linkready-20261012` (neighbor not ready).
Successful helper deployment: `r1-r14-lab-restore-hook-20261012`.

### R3 physical installed-runtime qualification — 2026-10-12

R3 (P1 MAC `00:1f:7a:69:f7:4c`) now also boots the r14 installed disk.
Installed root, clean source identity, mgrd binary digest, VPP/FRR services,
runtime sockets, four I211 DPDK bindings and four VPP ports all pass. The
R3 lab profile and VPP post-start replay hook were restored after install;
profile SHA256 is
`247498d09777db22b4e03153b1ff7774b1ddb7fd7ab599da4657f6101cda2dc3`.
R3 ports 1, 2 and 4 report 1 Gbps carrier; port 3 is administratively down.
The R3 `30.30.30.0/24` FIB has two `oper-flags:resolved` paths via
`10.20.0.2` on port 1 and `10.30.0.2` on port 2. UART Robot read-only suite
passes 3/3; CTest adapter passes. The initial adapter run exposed a stale
text parser for current VPP FIB output and zero-traffic counter display; both
were corrected, and the original failed report remains archived.

This establishes installed runtime and resolved ECMP FIB state. No packet
stream was generated during this R3 check, so packet distribution, forwarding
loss and throughput remain unmeasured. R3 install completion was operator
reported; physical disk write/readback transcript was not captured.

| Evidence | SHA256 |
| --- | --- |
| `r3-r14-installed-post-replay-20261012/inventory.json` | `a3797b921602b2018cb67675b5420709cacc4d0b2eaeadfcbc386d97ca49cbd6` |
| `r3-r14-lab-restore-20261012/result.json` | `3b23ee6ab75c38877c09760ba350e8e15b280607d3ef2e5ed75b7f7dd441760e` |
| `robot-r3-r14-readonly-fixed-20261012/output.xml` | `83ec65d39f4057184b4ac4015df41045b94cdd72b6b3afec3a476cbe1dbc2cbc` |

The preceding inventory before profile replay is under
`r3-r14-installed-20261012-recheck1/`; the first parser-mismatch Robot result
is retained under `robot-r3-r14-readonly-20261012/`.

R3's installed PCI/DPDK preflight also passes: four I211 functions are bound
to `uio_pci_generic`, 28×2 MiB hugepages are configured, and the VPP DPDK
plugin and API socket are active. Result SHA256 is indexed in
`docs/v0.16-pci-runner-contract.md`; performance remains open.

### R3 r14 live inventory recheck — 2026-10-13

UART was attached to R3 and the fixed read-only installed inventory was
captured again after the VPP restart test. The installed root is `/dev/sda1`
ext4; source commit and `danos-mgrd` digest still match r14; all four I211
functions remain on `uio_pci_generic`; VPP and FRR are active with API/stat/CLI
sockets present. `danos-mgrd` remains disabled pending production mTLS. The
inventory records existing interface drop counters and does not reset or
reinterpret them. This is an identity/runtime recheck, not a new traffic or
performance result.

Evidence: `build/r3-r14-inventory-current-20261013/inventory.json`, SHA256
`cf69ca0433203a647aa82463656789d68b8ce99c7bc6499482da2fa1b651938c`.
The repeated DPDK preflight result is unchanged and remains PASS; Robot could
not be rerun from this development environment because its Python module is
not installed. The prior post-restart physical Robot result remains the
3/3 PASS indexed below.

### R2 r9 → R3 r14 physical transit and ECMP — 2026-10-13

R2 remains on r9 (`3f1691c`, ISO `...20261009-r9.iso`) and was used as the
traffic source; R3 and R1 are on r14. R2's P1 identity is
`00:1f:7a:69:f5:ec`, link 1 Gbps, with resolved route
`30.30.30.0/24 via 10.10.0.1`. Four VPP ping destinations `.2`–`.5` each
returned 100/100 with 0% loss and TTL 63. The four-flow ECMP DPO counter on
R3 reached 430 packets (including 30 neighbor/readiness warm-up probes).
R3 P1 (R4 path) counted 110 RX/TX and P2 (R1 path) 320 RX/TX, confirming
both physical egress paths carried traffic. The observed 110/320 split is
the result of four destination flows and is not a statistical balance or
throughput result. R2's own P1 delta was RX +428 and TX +431; historical
drop/error totals were not reset.

This is a mixed-image functional forwarding check with R2 on r9 and R3/R1
on r14. R4 was not version-qualified in this run. It does not qualify
homogeneous four-node r14, lossless convergence, FRR/DPA route programming,
large-flow ECMP balance or performance. The R3 Robot suite after the traffic
passed 3/3.

| Evidence | SHA256 |
| --- | --- |
| `r2-r9-runtime-20261013/inventory.json` | `15ebab3b9ffce80efb3a75b742127b68dcc92b54398d26bf20a3cc972d8f5789` |
| `r2-r9-to-r3-r14-ecmp-20261013/result.json` | `b194863111690e512e02178495bb6d9bb32088dcdb5719c21bfb508f26aefa92` |
| `robot-r3-r14-after-r2-ecmp-20261013/output.xml` | `334cbd72fa4dba903e688bddbfe1fc28058d642268dd341f3b72c7b664233b40` |

### R3 r14 VPP service-restart recovery — 2026-10-13

With the verified R3 profile and post-start hook present, VPP service restart
changed PID `741 → 1576`. The profile returned P1/P2/P4 to up, FIB again had
two resolved ECMP paths, and direct 100-packet probes to R4 (`10.20.0.2`)
and R1 (`10.30.0.2`) each passed with zero loss. Post-restart UART Robot
passed 3/3. This proves VPP service replay of the lab CLI profile and direct
peer recovery; it is not mgrd WAL/FRR replay, packet transit during restart,
lossless recovery or restart downtime measurement.

| Evidence | SHA256 |
| --- | --- |
| `r3-r14-vpp-restart-20261013/result.json` | `285359f67a10d7d3c5f6d3c73be8b765e7da2b8e266a9bced613d8fffe7d2bce` |
| `robot-r3-r14-after-vpp-restart-20261013/output.xml` | `ea6ac321f73a7a4f27ec2b45e89f50490efbe960df2a2cc188f0c8637c975a5e` |

### R3 r14 physical next-hop withdrawal/restore — 2026-10-13

An identity-guarded UART probe shut P2 (R1) down, removed only its R1 next hop,
and verified the R3 route's forwarding DPO had one usable R4 adjacency. The
remaining R4 direct probe passed 5/5. P2 and its route were restored; after
bounded ARP warm-up, the forwarding DPO had two usable IPv4 adjacencies and
direct probes to R4 and R1 each passed 5/5. VPP stayed at PID 1576, P2 ended
with 1 Gbps carrier, and no restore errors were reported.

This does not claim FRR/ZAPI withdrawal, remote-prefix transit, continuity
during transition or performance. Several ARP warm-up groups initially had no
reply; successful restoration required neighbor learning and was not
instantaneous. R3-originated remote-loopback probes were excluded because the
peer return routes target R2's source address, not R3's link addresses. The
initial failed parser/reachability attempts remain under
`r3-r14-ecmp-failover-20261013/`, `r3-r14-ecmp-failover-retry-20261013/`,
and `r3-r14-ecmp-failover-final-20261013/`.

| Evidence | SHA256 |
| --- | --- |
| `r3-r14-ecmp-failover-final3-20261013/result.json` | `b321c93f32dce7d2ede733fc2f5194e4bf472a2576f668a7b3ccdc95b6a4f052` |
| `danos-test/integration/probe_r3_ecmp_failover_serial.py` | `8c451cef589c0d6dd28969eb99bbda37e1b0fb167986ba7f4c02e1e7808bc46e` |

The evidence parser unit tests cover two-path state, single-path state, and
the stale-path-list case where the forwarding DPO is still unresolved ARP.
Full CTest now passes 70/70.

### R2 four-flow ECMP counter correlation — 2026-10-13

Read-only R3 snapshots bracketed a fresh identity-checked R2 probe batch.
The same two resolved FIB/DPO next hops and VPP PID 1576 were present before
and after. R2 sent 420 requests total: four 100-packet measured probes plus
one 5-packet readiness probe per endpoint. All measured probes passed 100/100,
zero loss, TTL 63. R3 P4 RX/TX increased by 420/420; P1 TX by 105 and P2 TX
by 315. The egress total equals the ingress and sent total; P1/P2 drop counters
did not increase. This proves that both physical paths carried this batch.
The 25/75 split is a four-destination sample, not statistical ECMP balance,
throughput or line-rate qualification.

| Evidence | SHA256 |
| --- | --- |
| `r3-r14-r2-ecmp-counters-20261013/before/result.json` | `08d1731ef50d22dd9f4b9c90aef4a447937e20bb89fa4ccb0288a341459d3a72` |
| `r3-r14-r2-ecmp-counters-20261013/after/result.json` | `eb41a3617d1366d193920bfa9210f7bce96dde2bb48333cdce74a9a6648e842c` |
| `r2-r9-to-r3-r14-ecmp-counters-20261013/result.json` | `69e692b4ca8742a1ef38778d9070496eee48923ee30ddf4118fd31edb9920ba9` |
| `verification.json` | `6d5a7de298278e3ed64395788a1e359c6bd8c63b4f6be89e2802e95c313a4405` |
| counter capture utility | `b520c5d047c3693ec2f306d86eb357eaf2635a1ce6c4f723d21f571d51ba1b69` |
| counter-window verifier | `def4caf47275052d85645b2a25a076fc55bf67cfd146cbdd48515b91fc2c6732` |
| counter-window tests | `f2b072f9d107124baf642be2901d33d73e2196bcecc7ef9e9ff1f2a24a35ce30` |

### R2 endpoint reachability after R3 recovery — 2026-10-13

After the R3 VPP restart/replay checkpoint, the identity-guarded R2 probe
verified stable VPP PID 2097 and a 1 Gbps P1, then reached `.2`–`.5` in
`30.30.30.0/24` with 100/100 replies, zero loss and TTL 63 per destination.
This confirms reachability after recovery only, not uninterrupted traffic
during restart or ECMP bucket distribution. R2 remains on r9.

| Evidence | SHA256 |
| --- | --- |
| `r2-r9-to-r3-r14-postrestart-ecmp-20261013/result.json` | `7340eba9e88a44803209382c1e37a9cff22cdb87967e5857ae2b2e0991dbbafa` |
| `.2` ping log | `c7c97e3d2e4914765a129c4c56f3a99c6361da36bb1a8aa0ea581d5d7c9d54f8` |
| `.3` ping log | `dda949022a04d0c78ac6362e766d7e136b749219b2117b6055bcfb3d04fb7f64` |
| `.4` ping log | `56743665179b2e769f3289a26eb2bfe14df4ae6db0f17d311592c852f70a9e82` |
| `.5` ping log | `ebc80c5b13959537683e47868d3b6a0dbf1f42da83959c975d2e6a1c58f3b4c8` |

## Previous delivery: r13 BIOS installable lab ISO

### Installed-image management qualification (2026-10-12)

An independent COW child of the accepted installation passed the project's
`danos-mgmt/tests/test_mtls.py` against the exact installed mgrd binary under
Debian Python 3.13. TLS 1.2/1.3, certificate RBAC, operational DPA state and
recorded-backend Get, rejection of state writes, invalid credentials/ALPN,
fail-closed startup and worker shutdown passed. The test additionally checks
explicit laboratory plaintext opt-in; it does not enable the installed service.
Temporary PKI is generated and removed inside the disposable guest. No test
private keys were added to the delivery ISO or original installed disk.

Evidence: `build/install-r13-20261012/management-mtls-result.json` and
`management-mtls.serial.log`. The source script SHA256 is
`4cb52e9c3985ffc6b23b2f4f7d109c0f5d2775ab84c1b0a1f39df8bc0efb4fc2`.
The first attempt exposed missing CA keyUsage under Python 3.13 strict
verification; the fixture was fixed with critical keyCertSign/cRLSign, without
weakening verification. The retry and subsequent genuine reboot passed.
Scope is guest loopback with `--no-vpp`, not physical management deployment,
backend FIB programming or PCI performance. Production PKI remains OPEN.

The subsequent `management-vpp-readonly-result.json` run additionally
requires real VPP API connectivity and passes authenticated read-only state
queries, write rejection and genuine reboot. See `management-mtls.md` for
the evidence identities. No route programming/FIB readback is qualified.

- ISO: `build/danos-open-v0.16.0-rc1-generic-dpdk-installable-20261012-r13.iso`
- Size: 569704448 bytes; SHA256
  `ebb996269e5ca59b9cb38255cac5e429022704e9c7d291b165aae8095df2783d`.
- Application compiled on Debian trixie from `74d5f05`; installer builder
  `ac405da`, clean. LIVE and installed mgrd SHA256 both
  `d6f16a35889757ea7a9bba00755d59dfdc1d311a656e30d8599ef53e3fdf30bc`.
  Manifest records these distinct identities, not a guessed newest binary.
- Generic DPDK profile uses supported PCI detection/binding, not fixed I211
  addresses. Root payload is a 3 GiB Debian template installed onto a fresh
  disposable 5 GiB QEMU virtio disk. Full target-size provisioning is not
  claimed. Legacy BIOS only; UEFI/Secure Boot remain unqualified.
- Exact read-only ISO was booted as QEMU USB media; `install-serial` selected
  through BIOS/ISOLINUX keyboard input. Only `/dev/vda` was selected, with
  exact `ERASE /dev/vda`; full disk readback digest and installer rc=0 passed.
- With ISO absent, installed root `/dev/vda1` ext4, matching binary digest,
  clean build metadata, FRR/VPP active and API/stat sockets passed. A fresh
  kernel boot after reboot and random marker persistence also passed, using
  a copy-on-write child of the proven installed disk. Source disk unchanged.
- mgrd remains disabled until mTLS/PKI configuration; no insecure service
  was enabled. Root autologin is laboratory-only. Physical R3 installation,
  secure management runtime and real DPDK performance are separate gates.

Evidence bundle: `build/install-r13-20261012/qualification.json`,
`installation-readback.serial.log` SHA256
`4ff5c11ab3d6f25a7bb48d6c7d2129a9c5d0b8fc2ffd4a1fb49de50c0a169d50`,
`coldboot-real-reboot.serial.log` SHA256
`8dbf629029ce4928332ca2bd88065ad151ec4a3ad490b6cdcaac13fd0df0a917`.
The latter includes two actual Linux kernel starts with the persistence
check after the second. Verifier source SHA256 for this run is
`1ade56fac45afdcba2d6514d694851b754cfdde275c041cd4f442385cbeb3e7c`.
The installer verifier creates only its own disposable disk; optional reuse
requires matching installation evidence and a new COW
child, not raw rewriting an existing user disk.

Earlier attempts remain archived: `mgrd --help` correctly refused missing
mTLS (verifier replaced that assumption with loader checks); a prompt-only
reboot verdict was retracted and fixed with fresh-kernel evidence; temporary
per-user storage quota caused a failed installation. Two failed temporary
disks from this task were removed after stopping their QEMU instances, while
logs were kept. Storage-related CTest failures were rerun after freeing space;
current deterministic suite is 68/68 PASS. None of those failures is promoted
to an independent success verdict.

## Previous delivery: r12 BIOS installable lab ISO

- Artifact: `build/danos-open-v0.16.0-rc1-generic-dpdk-installable-20261009-r12.iso`
- Size: 520421376 bytes; source `486146445835c2047c37f39cfdd4f9ad057960f9`, clean.
- SHA256: `e249a0661c3d912ea0599d95715e0d59d99e3cc4e32eb9e06c55c0027472f928`.
- Runtime payload unchanged from r9; installation still uses a 4 GiB raw
  image, not ADR-0008 file-level installation. No hardware speed claim.
- Full-block stream writes and 4 MiB exact-length readback replace short
  writes and sector-sized readback. Completion returns to an explicit
  installer shell, not LIVE services. Debian runtime zlib/zstd are included
  and their digests recorded in the ISO manifest; guest loader resolves all
  mgrd dependencies inside the initramfs.

Qualification: QEMU KVM, `-cpu host -m 2048 -smp 2`, disposable 5 GiB virtio
disk, `qemu-xhci` USB storage containing this exact read-only ISO (bootindex 1).
ISOLINUX `install-serial` was selected using monitor `sendkey`; no direct
kernel/initramfs was supplied in this recorded USB test. Installer selected
`/dev/vda`, required `ERASE /dev/vda`, excluded USB `/dev/sda`, passed complete
readback digest, returned rc=0 and printed `DANOS-INSTALLER-COMPLETE`.

With USB/ISO detached, GRUB disk boot and a subsequent reboot both passed:
FRR/VPP active, API/stat sockets present, VPP CLI responds, root `/dev/vda1`
is ext4, `/etc/danos/r12-acceptance-marker` survives reboot. Installed mgrd
SHA256 is `0bf4626d4b85f3a0e3b4e95402d3f4dfeb36523560c16c8a66ace61d6b416cba`.
Management service activation/mTLS is a separate gate; not claimed here.
Default QEMU CPU lacks SSSE3 and cannot run this DPDK runtime; qualification
requires exposing supported CPU features. Related CTest checks pass 3/3.

| Recorded console evidence | SHA256 |
|---|---|
| `build/dpdk-installable/qemu-r12-usb-install.serial.log` | `412ede7fdebfd25054cf6a8d0bfa8809130e1dac579da8b226764477cc5e6425` |
| `build/dpdk-installable/qemu-r12-coldboot-reboot.serial.log` | `4af16e04c346de72cd0e8c7cb200ad9213233fe4bdc79e0816a99e756491382d` |

Legacy BIOS only; UEFI/Secure Boot and R3 physical install/DPDK traffic remain
unqualified. GPT initially describes the 4 GiB template on larger disks; this
raw fallback does not promise full target-capacity provisioning. Root console
autologin is lab-only. r10/r11 are historical candidates, not latest delivery.
Boot `install` for VGA input or `install-serial` for serial input. Whole-disk
selection and exact uppercase `ERASE /dev/...` remain mandatory and destructive.

## Policy

Effective 2026-10-08, every DANOS-Open ISO handed off for installation,
deployment, or hardware acceptance must provide a supported install-to-disk
path and pass the checks below. A bootable LIVE/test image is not an installer
and must never be described or delivered as an installable ISO. If an internal
LIVE-only fixture is required, it must be explicitly labeled as such and must
not substitute for the installable delivery image.

Installation must be explicit and fail-safe: show the selected target disk and
require confirmation before partitioning or formatting. Never silently select
or overwrite a disk. Keep LIVE/diagnostic boot available where useful, but
provide a distinct installer boot entry or a separate installable ISO artifact.

## Required acceptance gates

1. **Image/profile validation** — record source commit, dirty state, build
   profile, ISO SHA-256, BIOS/UEFI boot support, and installer payload.
2. **Boot media test** — boot the exact ISO in QEMU/VMware and reach the
   installer UI or serial-visible installer prompt; verify keyboard and serial
   input where supported.
3. **Disk installation** — install to an explicitly selected disposable VM
   disk; verify partition table, persistent root filesystem, bootloader, and
   successful installer completion. Include UEFI and legacy BIOS where the
   image claims both.
4. **Cold boot from disk** — detach the ISO and boot the installed disk. Verify
   kernel, persistent system root, DANOS services, VPP startup, and management
   console.
5. **Persistence/recovery** — reboot once more and verify installed
   configuration and required services persist. Capture console logs and bind
   results to the installed image/build identity.
6. **Hardware qualification** — only after VM installation passes, test the
   same installable ISO on target hardware. A LIVE-only hardware smoke test is
   useful diagnostically but does not satisfy installation acceptance.

For every gate, store the exact commands, machine-readable result, serial log,
and image digest. Failure, skip, or environment-open must remain distinct; none
may be reported as installation PASS.

## 2026-10-08 I211 lab installer candidate: QEMU gates PASS

Artifact:

- ISO: `build/danos-open-v0.16.0-rc1-i211-installable-20261007T233815Z.iso`
- ISO bytes: `530579456`
- ISO SHA256: `008ae0146820b58ecc8d1895be1075af2f1275feac7a1e0383628fc78de32a1e`
- ISO manifest: `build/danos-open-v0.16.0-rc1-i211-installable-20261007T233815Z.iso.manifest`
- Embedded disk payload SHA256 (compressed):
  `e3ecdce8a965317022246ba3e8a06fa1a9f8248ac92d65dab5cbb3e6710a9915`
- Embedded raw disk: 3,221,225,472 bytes; SHA256:
  `129b3cf387c455f42f3df0c0f4bd244770925a29235bd50346587237ac2a5115`
- Boot support: Legacy BIOS verified; UEFI is not supported/qualified by this
  artifact. This is I211 lab-specific, not a general-purpose NOS release.

The ISOLINUX menu keeps LIVE as the non-destructive default. `install` selects
the VGA/keyboard installer; `install-serial` selects serial input on ttyS0.
Both require selecting a whole-disk device and typing the exact
`ERASE /dev/...` confirmation. The installer rejects mounted/in-use disks,
boot media and undersized targets; it verifies the compressed payload and
hashes the complete target after writing. QEMU exercised `install-serial` on a
disposable 4 GiB disk as `/dev/vda`; it printed
`DANOS INSTALLER PASS: disk image written and readback digest verified`.

After removing the ISO, the installed disk booted twice. VPP and FRR were
`active`; `/run/vpp/api.sock` and `/run/vpp/stats.sock` existed; `vppctl show
version` returned VPP 26.10. A marker created after first boot survived a
subsequent reboot. Full CTest passed 50/50, including `install_disk_safety` and
`install_iso_wiring`.

Evidence logs under `build/i211-installable/`:

| Evidence | SHA256 |
|---|---|
| `qemu-install-serial-20261008.log` | `dbadc39c2e8ca8ae01adc805bacbf250d5c4e7a211ea718afab9350ac079fbcb` |
| `qemu-installed-coldboot-20261008.serial.log` | `589d4e81cff51cf2bf41e01b2285fead0c5756b06b48056171666def7459cd54` |
| `qemu-installed-reboot-20261008.log` | `f9dac225fea5cda743a278c05bde4f35b0e9e5a7003990d3d8f0c70a4909f7eb` |
| `qemu-installed-reboot2-20261008.log` | `3ee1c74ba5b258a58fef2419bd35a1793f5d89eec0057a28af497b1dac3ffd13` |

This restricted lab profile has root autologin on local VGA/serial consoles,
disables SSH, and leaves mgrd disabled until management mTLS is configured.
Do not expose it to untrusted networks. I211 PCI binding and traffic
forwarding on physical hardware are not covered by QEMU install tests.
Physical installation is not yet performed; confirm the target machine and
whole-disk device before overwriting it. Require at least 3,221,225,472 bytes.

Build provenance in the ISO manifest is commit
`9230c2c0f4a5507fc1741da9a3e70019a5396152`, with
`DANOS_SOURCE_DIRTY=1`; this is a validated candidate from a dirty tree, not a
clean release build. Historical r5 and other LIVE-only ISOs remain
non-installable.

## Generic DPDK hardware profile: QEMU install/boot/recovery PASS

The initial r3 ISO booted straight into LIVE after a short, effectively
unusable selection interval. r4 fixed the interactive VGA boot prompt, but
USB installation exposed a second issue: SCSI discovery completed after the
installer's first media scan, and the initramfs lacked `sd_mod`. r5 added
retries and fail-closed recovery but still lacked that module. r3-r5 are
superseded and must not be used for installation.

Validated USB-capable ISO:

- ISO: `build/danos-open-v0.16.0-rc1-generic-dpdk-installable-20261008T010054Z-r6.iso`
- ISO SHA256: `ba79489cc5e652307cb5077a0dc4b495d021f70203fbccf45ea3e2ddf39a5d2a`
- Embedded build identity: commit `cbf0f65bc073785c66abbd20c4fad7555a8d4657`,
  `DANOS_BUILD_SOURCE_DIRTY=1`, build time `2026-10-08T04:05:57Z`.
- The installer waits up to 45 seconds for removable ISO block media and
  rescans devices. The initramfs includes the matching `sd_mod` kernel module.
  Installer failure now stays in a recovery shell rather than silently
  continuing into LIVE.
- QEMU booted this ISO as USB mass storage (not a virtual CD-ROM). The serial
  log shows the ISO enumerated as `/dev/sda`, the installer finding its
  payload, then installing to a disposable 4 GiB `/dev/vda` and printing
  `DANOS INSTALLER PASS` after readback digest verification.
- QEMU VGA/curses showed the boot prompt waiting indefinitely; typing
  `install` entered the installer. Serial log:
  `build/dpdk-installable/qemu-r6-usb-serial.log` (SHA256
  `71b2722221f1a8f04b3e65e62f82edb0dcc3a500c40fa6fdb5901e420100f528`).
- Bootloader selection is VGA/keyboard; ttyS0 becomes the Linux console after
  an entry is selected. Serial-only bootloader selection is not qualified.

Earlier r3 candidate (superseded; do not install):

- ISO: `build/danos-open-v0.16.0-rc1-generic-dpdk-installable-20261008T010054Z-r3.iso`
- ISO SHA256: `965d26b3b8ab24dc74847e1d255f747b31a85d4ad1be75d15063f7e6fdf295b6`
- Embedded raw installed disk SHA256:
  `f6226f08f2a8348530ba83bed4e1f912658dd05d3dadd053441dcc758bf4246d`
- ISO payload compressed SHA256:
  `6adbf27d34b6d767ba19cd03ee17e3aa1e0dc2a48e8edb3abaf07e391758dc18`
- Firmware mode: Legacy BIOS/hybrid boot preserved from the QEMU-qualified
  Debian trixie installer base; UEFI is not qualified.
- The VM installer was exercised against a disposable 4 GiB qcow2 disk; it
  reported `DANOS INSTALLER PASS` after readback digest verification. The ISO
  was detached before two installed-disk boots. On both boots VPP 26.10 and
  FRR were active; API, stats and CLI sockets existed. A marker written on
  first boot survived the next reboot. QEMU had only a virtio management NIC,
  which was detected and skipped as `default-route-management`.
- Physical installation is **not performed**. This artifact has only VM-level
  installer/boot/recovery qualification so far; no real PCI driver binding or
  packet forwarding is claimed by this result.

At boot, the installer profile enumerates PCI functions by Ethernet class
(`class 0x0200xx`), without a baked-in vendor/device ID or BDF list. It protects
every interface carrying a default IPv4/IPv6 route (including physical members
below a bridge or bond) from takeover, then emits VPP `dpdk { dev <BDF> }`
entries and binds other Ethernet candidates. Binding prefers `vfio-pci` only
when IOMMU groups are present and contain only selected Ethernet functions;
otherwise it falls back to `uio_pci_generic`. Binding uses per-device
`driver_override`, never a global PCI `new_id` match.

This is hardware-generic discovery, not a claim that every PCI Ethernet device
has a PMD in this VPP build. Devices without a matching DPDK PMD may be ignored
or reported by VPP; physical acceptance must confirm the expected interfaces
appear and pass packet tests. USB NICs and non-PCI devices are not discovered.
If no default route is configured, all PCI Ethernet functions are considered
dataplane candidates. Use an isolated console during initial hardware bring-up.

The ISO manifest records the source as dirty during candidate creation; this
artifact is not a clean release build. It must not be confused with the
earlier I211-specific or LIVE-only artifacts.

### Physical USB boot and installer fail-safe check (2026-10-08)

The user booted the r6 generic installer ISO on the I211 host and supplied a
console photo. A subsequent read-only USB-UART probe confirmed the running
kernel command line includes `danos.install=1`; `/etc/danos/build-info.env`
matched the r6 artifact name, commit and dirty marker above. `/dev/sdb` is the removable
USB ISO (`ProductCode`, removable=1), while `/dev/sda` is the internal
`TS16GMSA370` disk (16,013,942,784 bytes, removable=0) with existing
partitions. The system enumerated four Intel I211 PCI functions at
`0000:01:00.0` through `0000:04:00.0` (`8086:1539`).

The installer requested the exact confirmation `ERASE /dev/sda`; the operator
entered only `/dev/sda`. The installer reported confirmation mismatch,
`target untouched`, and entered recovery instead of continuing into LIVE.
This is a **physical boot and destructive-write guard PASS**, not a disk
installation PASS. No write was initiated. The recovery initramfs exposes only
`lo` in `/sys/class/net`; it did not initialize VPP or provide a usable
network dataplane, so connected Ethernet cabling is not yet packet-test
evidence. Physical install, cold boot from the installed disk, I211 binding,
and traffic/ECMP tests remain unqualified pending an explicitly authorized
target-disk erase and subsequent live dataplane boot. Do not infer disk safety
from `/dev/sda` naming alone.

### Clean r8 generic installer: QEMU install, cold boot and reboot PASS

The clean r8 candidate supersedes the dirty r6/r7 images for further hardware
qualification:

- ISO: `build/danos-open-v0.16.0-rc1-generic-dpdk-installable-r8.iso`
- ISO SHA256:
  `b3a668789b78b548b35a59f984b06939aa70b0f5f7bb631adada935bf4748d46`
- Embedded source: `0578cb75881c213338a4b0ef198633b9351b1194`,
  `DANOS_BUILD_SOURCE_DIRTY=0`.
- Installed payload SHA256 (compressed):
  `352cb136245974da1d1f57d2fca019584f6f0349bf0bc1f24d35f53846ad5648`.

QEMU booted the ISO as USB mass storage, installed to a disposable 4 GiB
`/dev/vda`, and verified the complete target readback digest. With the ISO
detached, cold boot confirmed `/etc/danos/build-info.env` matches the clean
source commit, DANOS package `0.16.0~rc1-1`, and VPP package `26.10-1`;
`vpp.service` was active and both API and stats sockets existed. A marker
survived a subsequent reboot with the same VPP/socket checks passing. Logs:

- `build/dpdk-installable/qemu-r8-install.serial.log`
- `build/dpdk-installable/qemu-r8-coldboot.serial.log`
- `build/dpdk-installable/qemu-r8-reboot.serial.log`

These are QEMU installer and persistence results, not physical PCI/forwarding
qualification. The USB-UART adapter opened successfully when checked, but
captured no new physical-host output in the latest passive read. The last
UART-derived physical state therefore remains the r6 confirmation-mismatch
recovery shell. Do not install r8 to the internal `/dev/sda` without explicit
confirmation that its existing data may be erased.

### 2026-10-09 user-provided installer photo: safety abort confirmed

The latest console photo shows the `DANOS-OPEN INSTALLER — GENERIC DPDK
PHYSICAL LAB RUNNER` prompt, `/dev/sda` selected as a 16 GB internal disk, and
an installed image size of 3,221,225,472 bytes. At the destructive confirmation
prompt the entered text was `/dev/sda`, but the installer requires the exact
string `ERASE /dev/sda`. It therefore printed `confirmation did not match;
target untouched`, exited with `rc=1`, and entered the recovery BusyBox shell.
This is a fail-safe installer abort, not evidence that installation completed
or that the installed system booted. The photo does not expose the ISO
filename/digest, so this run is not attributed to a specific r-number. No
serial device was accessed for this interpretation; it is based only on the
image supplied in the conversation. Do not retry a destructive confirmation
until the operator explicitly confirms that the selected internal disk may be
erased.

### Clean r9 generic installer: QEMU install, cold boot and reboot PASS

The current installable image was rebuilt from clean source commit
`3f1691c62ae6200fa81028b4d8a81dfcdef6f6fc`. The installed Debian trixie disk
is based on the previously QEMU-qualified r8 runner; its `danos-open` package
was rebuilt from the current source with CMake Release settings and installed
offline into a disposable disk copy. The ISO's embedded manifest and installed
`/etc/danos/build-info.env` both identify the current commit and package hash.

- ISO: `build/danos-open-v0.16.0-rc1-generic-dpdk-installable-20261009-r9.iso`
- ISO SHA256: `9c464d0ebe2a6e6bcc21d85565a1036a7bb9ecd9b41f62341f65407539b7fb3d`
- ISO size: 520,421,376 bytes; Legacy BIOS verified, UEFI not qualified.
- Compressed installed-disk payload SHA256:
  `56db0831ec72b0050f81cd10f28a4f4db1fa3fa06ade5e1a80d3f21d57230855`
- Raw installed disk: 4,294,967,296 bytes; SHA256:
  `1862c90ac7f7d71135c5d2c88f58fc45d4d8211c76e31a20e2cceaee59067cdf`
- `danos-open` package SHA256:
  `3e09683cf24069aabaa0d3a49c0861f5061c27aadb3ac512dbb4bb722078f891`
- Installed `/usr/bin/danos-mgrd` SHA256:
  `0bf4626d4b85f3a0e3b4e95402d3f4dfeb36523560c16c8a66ace61d6b416cba`

QEMU booted the exact ISO as USB mass storage and installed it to a disposable
4 GiB `/dev/vda`. The installer completed full target readback verification
(`DANOS INSTALLER PASS`). With the ISO detached, cold boot and a subsequent
reboot both showed `vpp.service` and `frr.service` active, `/run/vpp/api.sock`
and `/run/vpp/stats.sock` present, `danos-open` `0.16.0~rc1-1` installed, and
VPP 26.10 responding to `vppctl`. The running manager binary matched the
Release build hash above.

Logs and reusable payload:

- `build/dpdk-installable/qemu-r9-install.serial.log`
- `build/dpdk-installable/qemu-r9-coldboot.serial.log`
- `build/dpdk-installable/qemu-r9-reboot.serial.log`
- Log SHA256s, in the same order:
  `a674eeb2aa81b05ca0f7e7cab02382e981154b55cc455570d26820a944293dbd`,
  `5a9e0debff8de463fbd3bcf7ceb0cab1101232eb242f61b784a24692e4fc97c5`,
  `01ba82cd181d6b34df3e7a031261e6b2b68c359adf69dd57e6510842ba2acfc4`.
- `build/dpdk-installable/danos-open-v0.16.0-rc1-dpdk-installed-20261009T0116Z.raw.gz`
- Matching `.manifest` beside the payload; ISO manifest:
  `build/danos-open-v0.16.0-rc1-generic-dpdk-installable-20261009-r9.iso.manifest`.

This is VM installer/boot qualification only. It does not certify physical
installation, automatic PCI driver binding on I211, DPDK forwarding, or
performance. Physical installation will erase the explicitly selected target
disk and remains a separate operator action; do not use the USB boot device as
the installation target.
# 2026-10-09 r10 optimized raw installer checkpoint

Artifact: `build/danos-open-v0.16.0-rc1-generic-dpdk-installable-20261009-r10.iso`.
SHA256: `fb1e30c236e74f7bfa0bc3c0bb837a0696aafe27e94bb30b4205a934b98ed0dc`.
Installer source: `a8811f9`; installed runtime payload is unchanged from r9.
This is an optimized raw installer, **not** the pending ADR-0008 rootfs installer.

Embedded manifest/script and BIOS boot structure checks pass. A disposable
5 GiB QEMU virtio disk was installed using the kernel/initramfs extracted from
this exact ISO, with the ISO attached as read-only CD media. Write output was
`1024+0 records in/out`; full 4 GiB readback digest passed and installer exited
with rc=0. Guest uptime at the subsequent LIVE transition was approximately
106 seconds; this is not a measured physical-install duration.

With ISO detached and no direct kernel arguments, the installed disk boots
through GRUB to Debian root shell; FRR is active. VPP was still activating at
the sampled check and API/stat sockets were not confirmed. Consequently full
runtime qualification remains pending, as do USB firmware boot, UEFI and R3
physical installation. Post-install LIVE mgrd reports missing `libz.so.1`;
this dependency issue is open and must not be hidden by the installer PASS.
