# DANOS-Open v0.16.0 — DRAFT, NOT RELEASED

Release preparation checkpoint: `affb5e134367877b10141fac6a7bf79cde764b38`
(2026-10-19). No final tag, GitHub release, package publication, or final
artifact qualification has been performed. Parallel untracked testpmd and
performance-tool drafts remain outside this checkpoint and are not release
evidence.

## Planned scope

The minimal L3 NOS loop connects management APIs, transactional DPA state,
FRR/ZAPI and VPP route programming. Functional evidence includes route
add/withdraw/restore, scoped restart/replay, QEMU e1000 ECMP, and VMware
VMXNET3 polling-mode packet forwarding.

The candidate includes native gNMI mTLS, explicit certificate-role mapping,
Unix peer credentials, identity-aware RBAC/audit, transaction staging/WAL
recovery, and connection-owned gNMI STREAM subscriptions with incremental
frame assembly and per-stream dispatch. Unsupported subscription contracts
fail explicitly. These features are covered by the local/hosted tests listed
below; this document does not claim production PKI or physical HA qualification.

## Verified at this checkpoint

- `bash danos-test/integration/release_check.sh`: ALL PASS on the exact
  `affb5e1` source. This ran CTest, external gnmic I1–I5/I9 including the
  socat/OpenSSL TLS path, seeded gNMI readback, 131 Prometheus metrics, forced
  mgrd crash, and recovery of 3 WAL records with gNMI state readback.
  The selected `build/gnmic` binary SHA256 is
  `0a40f8d856fe64116a586e791080d349f8d60319581711b51d9aa9095d3e52b8`.
- Most recent completed remote checks before this checkpoint: CI, Coverage,
  and Interop Gate passed on `affb5e1` (runs `39250438281`, `39250438228`,
  `39250438748`).
- QEMU archived functional evidence: 4,000/4,000 packets at 81.97 pps,
  RTT p50/p99 414.90/3566.20 us, ECMP bucket counts 3,000/1,000, and
  restart/replay PASS. Packet size/Mbps and CPU are unmeasured, so this is not
  PCI performance qualification.
- The newer QEMU lifecycle run separately passed BGP add/withdraw, OSPF,
  next-hop withdrawal/restore, VPP and FRR/zserv restart recovery; its
  failover window delivered 785/800 packets (1.88% loss under its configured
  15% budget). It is not the 1,000-packets-per-flow soak run.
- Archived VMware evidence is historical, tied to its original ISO/commit;
  it is not a fresh runtime qualification for this checkpoint. A clean ISO
  pair exists, but this host currently has no loaded `vmmon`/`vmnet` modules
  or running VMware guests.
- Read-only physical management checks reached all four P3 addresses from
  the development host. The host test route to `30.30.30.0/24` is absent,
  the serial console is occupied by another process, and no physical transit,
  dynamic FRR recovery, or PCI performance run was performed in this check.

## Blocking release conditions

The [precondition checklist](v0.16.0-preconditions.md) is authoritative.
Physical PCI performance, R2↔R3 route/neighbor recovery and dynamic FRR/DPA/VPP
replay, fresh VMware runtime evidence, current-checkpoint remote CI completion,
and final artifact qualification remain open. No open-lane waiver is approved.

Keep `v0.16.0` untagged. Before publishing, update this document with the
exact accepted commit and artifact hashes, completed evidence, release date,
and any explicitly approved limitations. CMake's numeric `0.16.0` is not
proof of final-release acceptance; Debian packaging remains
`0.16.0~rc1-1` until a separate version promotion is approved.

## Deployment and boundaries

Configure mTLS as described in [management deployment](../management-mtls.md).
Do not enable plaintext lab mode for production management. Provision and
qualify PKI on the intended machines separately. Certificate trust/role
changes require daemon restart; automatic CRL/OCSP and online rotation are not
provided. Physical P3 management reachability is not evidence of physical
data-plane or release qualification.
