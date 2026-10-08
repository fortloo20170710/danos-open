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
