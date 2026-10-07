# ISO delivery and installation acceptance

更新时间：2026-10-08

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
