# DANOS-Open v0.16.0 — RELEASE PREPARATION, NOT TAGGED

Prepared: 2026-11-06. Accepted candidate commit:
`5332720c20d88be0cc10290c0eba4dfbb1e82b3f` (merged main).
No `v0.16.0` tag, GitHub release or package publication has been created. A
human release manager must approve the items in
[v0.16.0-final-checklist.md](v0.16.0-final-checklist.md) first.

## Release result

The unified gate reports **`status=PASS`** as an **approved-with-open-lanes**
result: `vmware_waived=1`, `dpdk_status=PASS`, `qemu_gate_status=PASS`.
Archive: `docs/evidence/v016-release-gate-preflight-2026-11-05-waived.env`.

This is not a clean qualification. The VMware `vmware-vmxnet3` validator lane
is waived: VPP cannot drive VMware vmxnet3 on Workstation. Native and
DPDK/uio vmxnet3 both bind the virtual NICs but expose no usable VPP dataplane
interface. Waiver approved by `miwide20190328`; bounded kernel-vmxnet3
evidence is two VMs, 64B 1000/1000 zero loss, and 1000/1000 after reboot.

## Highlights

- Management plane: native gNMI mTLS, Unix peer credentials, RBAC/audit,
  connection-owned STREAM subscriptions, NETCONF over SSH with candidate,
  startup, validate, locks, copy/delete-config and durable confirmed-commit
  rollback state.
- Data plane: netlink/OPER readback, missing-route repair, transactional WAL
  recovery, FRR/ZAPI and VPP route programming, restart/replay and ECMP
  failover/restore.
- Physical I211: 64-byte testpmd **777,589 pps / 398.1 Mbps** with zero
  observed loss; dual-path VPP ECMP FIB `buckets:2`, 40/40 probes 0% loss,
  per-interface TX split **31/10**.
- Four-node deployment: R1 `.241`, R2 `.242`, R3 `.245`, R4 `.244`, persistent
  SSH and static LAN addressing; current-main `danos-open 0.16.0~rc1-1`
  installed on all four with matching mgrd/netconfd/subsystem hashes.
- R1–R4 gNMI apply/rollback drill: route appears in the kernel FIB, inverse
  delete removes it, services stay active, management ping 0% loss.
- VMXNET3: clean VMware ISO builds and boots (guest sees `15ad:07b0`, 10 Gbps
  link); packet validation remains waived as above.
- Physical I211 runner/traffic ISOs were built at their pinned commits and
  pass the QEMU console, serial, VPP-plugin and profile validators.

## Artifacts

| Artifact | SHA256 |
|---|---|
| QEMU e1000 traffic ISO | `a7265767a75c70c4b1f2e42dc3955b03ceca4e684b3a9251b9b97b88d2f3f07e` |
| I211 runner ISO (`88f4bed`) | `b5236912ba73415e905ea54434b3bf37c234b574850b7fee1d836395e34261af` |
| I211 traffic ISO (`2bcb087`) | `12210311d576d6c318744ba3614deab5873b17a8bb9d16c0b7bb9027cb2db036` |
| VMware VMXNET3 ISO | `d815aa50ce857b1193bf116f99496b50670846344ca4b1c3ecd6afe6d5b99a9a` |
| Debian package (`0.16.0~rc1-1`) | `d48b073337412703988e664b8286743245f9fff617b30e5d79d367463f0ac508` |

Per-ISO manifests are in `build/*.iso.manifest`.

## Versions and packaging

- CMake project version: `0.16.0`.
- Debian package version: `0.16.0~rc1-1`.
- Git tag: absent. Packaging promotion to `0.16.0-1` is a human release
  decision and was not performed.

## Limitations and non-claims

- The VMware dual-path VPP/peer-traffic baseline is waived, not passed.
- VFIO remains blocked on the physical runner (firmware exposes zero IOMMU
  groups); UIO was used.
- L3-02 is a bounded 64B/ECMP pass; line-rate latency, per-core CPU and
  sustained performance are not claimed.
- L3-03 (physical R2/R3/R4 rollback relay drills), L3-04 (physical dynamic FRR
  BGP/OSPF recovery) and L3-06 (uniform four-node qualification) remain
  `ENVIRONMENT-OPEN` in the release decision and are not covered by the gate's
  VMware waiver. The release manager must explicitly accept or close them.
- Production PKI/mTLS deployment (REL-01) still requires security-owner
  sign-off; test certificates are not production qualification.
- No tag or publication has been performed.
