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

## Current gap

`danos-test/live/build_live_iso.sh` currently builds a kernel + initramfs +
ISOLINUX LIVE image. It does not package a persistent target root filesystem,
disk partition/install workflow, or an installed-system bootloader path.
Therefore existing images such as
`danos-open-v0.16.0-rc1-i211-dpdk-traffic-runner-r5.iso` are LIVE test images,
not installable releases. The installer/root-filesystem build path and its
QEMU/VMware installation gates are outstanding work; do not label any such
image installable until those gates pass.
