#!/usr/bin/env bash
# Construct a persistent Debian trixie generic DPDK test-runner disk using the
# project's pinned VPP/DANOS .deb payloads, then boot it once for validation.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BASE_IMAGE=${DANOS_PREBUILT_RUNNER_BASE_IMAGE:-${DANOS_DEBIAN_BASE_IMAGE:-$ROOT/build/debian-13-generic-amd64.full.qcow2}}
VPP_DEB=${DANOS_VPP_DEB:-$ROOT/build/danos-vpp-runtime_26.10-1_amd64.deb}
DANOS_DEB=${DANOS_OPEN_DEB:-$ROOT/build/danos-open_0.16.0~rc1-1_amd64.deb}
OUT_DIR=${1:-$ROOT/build/dpdk-installable}
MEMORY_MB=${DANOS_INSTALL_VM_MEMORY_MB:-4096}
VCPUS=${DANOS_INSTALL_VM_VCPUS:-4}
RUN_ID=$(date -u +%Y%m%dT%H%M%SZ)
SOURCE_COMMIT=$(git -C "$ROOT" rev-parse HEAD)
SOURCE_DIRTY=$(if git -C "$ROOT" diff --quiet && git -C "$ROOT" diff --cached --quiet; then echo 0; else echo 1; fi)
BUILD_UTC=$(date -u +%Y-%m-%dT%H:%M:%SZ)

for required in "$BASE_IMAGE" "$VPP_DEB" "$DANOS_DEB"; do
    test -r "$required" || { echo "ERROR: required input missing: $required" >&2; exit 2; }
done
for command in qemu-img qemu-system-x86_64 xorriso gzip sha256sum timeout dpkg-deb; do
    command -v "$command" >/dev/null || { echo "ERROR: missing tool: $command" >&2; exit 2; }
done
DANOS_DEB_SHA256=$(sha256sum "$DANOS_DEB" | awk '{print $1}')
VPP_DEB_SHA256=$(sha256sum "$VPP_DEB" | awk '{print $1}')
DANOS_DEB_VERSION=$(dpkg-deb -f "$DANOS_DEB" Version)
VPP_DEB_VERSION=$(dpkg-deb -f "$VPP_DEB" Version)

mkdir -p "$OUT_DIR"
OUT_DIR=$(cd "$OUT_DIR" && pwd)
WORK=$(mktemp -d "$OUT_DIR/rootfs-work.XXXXXX")
OVERLAY="$WORK/danos-dpdk-installed.qcow2"
SEED="$WORK/cloud-init-seed.iso"
SERIAL_LOG="$OUT_DIR/danos-open-v0.16.0-rc1-dpdk-rootfs-$RUN_ID.serial.log"
QCOW2="$OUT_DIR/danos-open-v0.16.0-rc1-dpdk-installed-$RUN_ID.qcow2"
RAW_DISK="$WORK/danos-open-v0.16.0-rc1-dpdk-installed.raw"
RAW_GZ="$OUT_DIR/danos-open-v0.16.0-rc1-dpdk-installed-$RUN_ID.raw.gz"
MANIFEST="$OUT_DIR/danos-open-v0.16.0-rc1-dpdk-installed-$RUN_ID.manifest"
qemu-img check "$BASE_IMAGE"
qemu-img create -q -f qcow2 -F qcow2 -b "$BASE_IMAGE" "$OVERLAY"

SEED_TREE="$WORK/seed-tree"
mkdir -p "$SEED_TREE"
cp "$VPP_DEB" "$SEED_TREE/danos-vpp-runtime.deb"
cp "$DANOS_DEB" "$SEED_TREE/danos-open.deb"
cat > "$SEED_TREE/meta-data" <<'META'
instance-id: danos-dpdk-runner-v016rc1
local-hostname: danos-dpdk-runner
META
cat > "$SEED_TREE/user-data" <<'USERDATA'
#cloud-config
users:
  - default
  - name: danos
    gecos: DANOS generic DPDK lab runner
    groups: [adm, sudo]
    shell: /bin/bash
    lock_passwd: true
    sudo: ["ALL=(ALL) NOPASSWD:ALL"]
write_files:
  - path: /etc/apt/sources.list.d/00-danos-trixie.sources
    permissions: '0644'
    content: |
      Types: deb
      URIs: http://repo.huaweicloud.com/debian
      Suites: trixie trixie-updates trixie-backports
      Components: main
      Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg

      Types: deb
      URIs: http://repo.huaweicloud.com/debian-security
      Suites: trixie-security
      Components: main
      Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
  - path: /etc/modules-load.d/danos-dpdk.conf
    permissions: '0644'
    content: |
      vfio-pci
      uio_pci_generic
  - path: /usr/local/sbin/danos-runner-install-packages
    permissions: '0755'
    content: |
      #!/bin/sh
      set -eu
      if dpkg-query -W -f='${Status}' danos-vpp-runtime 2>/dev/null | grep -q 'install ok installed'; then
          dpkg -i /mnt/danos-seed/danos-vpp-runtime.deb /mnt/danos-seed/danos-open.deb
      else
          apt-get update -o Acquire::Retries=2 -o Acquire::http::Timeout=30 -o Acquire::Languages=none
          DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends /mnt/danos-seed/danos-vpp-runtime.deb /mnt/danos-seed/danos-open.deb frr iproute2 iputils-ping pciutils kmod systemd-sysv
      fi

  - path: /etc/systemd/system/serial-getty@ttyS0.service.d/override.conf
    permissions: '0644'
    content: |
      [Service]
      ExecStart=
      ExecStart=-/sbin/agetty --autologin root --noclear %I $TERM
  - path: /etc/systemd/system/getty@tty1.service.d/override.conf
    permissions: '0644'
    content: |
      [Service]
      ExecStart=
      ExecStart=-/sbin/agetty --autologin root --noclear %I $TERM
  - path: /etc/danos/runner-profile
    permissions: '0644'
    content: |
      profile=generic-dpdk-physical-lab-runner
      vpp=26.10
      dpdk_driver=auto (vfio-pci when IOMMU groups are safe, otherwise uio_pci_generic)
      management_service=disabled-until-mtls-configured
  - path: /etc/danos/build-info.env
    permissions: '0644'
    content: |
      DANOS_BUILD_COMMIT=__SOURCE_COMMIT__
      DANOS_BUILD_SOURCE_DIRTY=__SOURCE_DIRTY__
      DANOS_BUILD_UTC=__BUILD_UTC__
      DANOS_BUILD_PROFILE=generic-dpdk-physical-lab-runner
      DANOS_OPEN_PACKAGE_VERSION=__DANOS_DEB_VERSION__
      DANOS_OPEN_PACKAGE_SHA256=__DANOS_DEB_SHA256__
      DANOS_VPP_PACKAGE_VERSION=__VPP_DEB_VERSION__
      DANOS_VPP_PACKAGE_SHA256=__VPP_DEB_SHA256__
  - path: /etc/issue
    permissions: '0644'
    content: |
      DANOS-Open generic DPDK physical lab runner (Debian trixie; serial-console lab image)
      Root serial autologin is enabled for isolated hardware testing. Do not expose
      this image to an untrusted network. Configure management mTLS before enabling mgrd.
runcmd:
  - [sh, -c, 'rm -f /etc/apt/sources.list.d/debian.sources']
  - [sh, -c, 'mkdir -p /mnt/danos-seed; for n in 1 2 3 4 5 6 7 8 9 10; do mount -o ro /dev/vdb /mnt/danos-seed 2>/dev/null && break; sleep 1; done; test -r /mnt/danos-seed/danos-open.deb']
  - [sh, -c, '/usr/local/sbin/danos-runner-install-packages']
  - [sh, -c, 'systemctl disable --now ssh.service ssh.socket 2>/dev/null || true; systemctl mask ssh.service ssh.socket 2>/dev/null || true']
  - [sh, -c, 'systemctl disable --now danos-mgrd.service 2>/dev/null || true']
  - [sh, -c, 'systemctl enable serial-getty@ttyS0.service getty@tty1.service vpp.service']
  - [sh, -c, 'systemctl daemon-reload; if ! systemctl restart vpp.service; then systemctl status vpp.service --no-pager || true; journalctl -u vpp.service -n 100 --no-pager || true; cat /var/log/vpp/vpp.log 2>/dev/null || true; exit 1; fi']
  - [sh, -c, 'for n in $(seq 1 45); do test -S /run/vpp/api.sock && break; sleep 1; done; if ! test -S /run/vpp/api.sock; then systemctl status vpp.service --no-pager || true; journalctl -u vpp.service -n 100 --no-pager || true; cat /var/log/vpp/vpp.log 2>/dev/null || true; exit 1; fi; vpp --version; vppctl -s /run/vpp/cli.sock show version; vppctl -s /run/vpp/cli.sock show hardware-interfaces; printf "DANOS-RUNNER-ROOTFS-CONFIGURED PASS\n" > /dev/ttyS0']
  - [sh, -c, 'cloud-init clean --logs --machine-id; shutdown -h now']
USERDATA

sed -i \
    -e "s|__SOURCE_COMMIT__|$SOURCE_COMMIT|g" \
    -e "s|__SOURCE_DIRTY__|$SOURCE_DIRTY|g" \
    -e "s|__BUILD_UTC__|$BUILD_UTC|g" \
    -e "s|__DANOS_DEB_VERSION__|$DANOS_DEB_VERSION|g" \
    -e "s|__DANOS_DEB_SHA256__|$DANOS_DEB_SHA256|g" \
    -e "s|__VPP_DEB_VERSION__|$VPP_DEB_VERSION|g" \
    -e "s|__VPP_DEB_SHA256__|$VPP_DEB_SHA256|g" \
    "$SEED_TREE/user-data"

xorriso -as mkisofs -quiet -o "$SEED" -V CIDATA -J -R \
    "$SEED_TREE/user-data" "$SEED_TREE/meta-data" \
    "$SEED_TREE/danos-open.deb" "$SEED_TREE/danos-vpp-runtime.deb"

qemu-system-x86_64 \
    -machine q35,accel=kvm:tcg -cpu max -smp "$VCPUS" -m "$MEMORY_MB" \
    -drive "file=$OVERLAY,format=qcow2,if=virtio" \
    -drive "file=$SEED,format=raw,if=virtio,readonly=on" \
    -netdev user,id=mgmt -device virtio-net-pci,netdev=mgmt \
    -boot order=c,menu=off -display none -monitor none \
    -serial "file:$SERIAL_LOG" -no-reboot \
    >"$WORK/qemu.stdout.log" 2>&1 &
QEMU_PID=$!
set +e
timeout 1200 bash -c 'while kill -0 "$1" 2>/dev/null; do sleep 1; done' _ "$QEMU_PID"
WAIT_RC=$?
wait "$QEMU_PID" 2>/dev/null
QEMU_RC=$?
set -e
if test "$WAIT_RC" -eq 124; then
    kill "$QEMU_PID" 2>/dev/null || true
    echo "ERROR: rootfs customization VM timed out; serial log: $SERIAL_LOG" >&2
    exit 1
fi
if ! grep -Fq 'DANOS-RUNNER-ROOTFS-CONFIGURED PASS' "$SERIAL_LOG"; then
    echo "ERROR: rootfs customization gate missing; qemu_rc=$QEMU_RC serial=$SERIAL_LOG" >&2
    tail -n 100 "$SERIAL_LOG" >&2 || true
    exit 1
fi

qemu-img convert -p -O raw "$OVERLAY" "$RAW_DISK"
RAW_BYTES=$(stat -c '%s' "$RAW_DISK")
RAW_SHA=$(sha256sum "$RAW_DISK" | awk '{print $1}')
gzip -1 -n -c "$RAW_DISK" > "$RAW_GZ"
GZIP_SHA=$(sha256sum "$RAW_GZ" | awk '{print $1}')
printf 'DANOS_INSTALLED_DISK_BYTES=%s\nDANOS_INSTALLED_DISK_SHA256=%s\nDANOS_INSTALLED_DISK_GZIP_SHA256=%s\nDANOS_SOURCE_COMMIT=%s\nDANOS_SOURCE_DIRTY=%s\nDANOS_BUILD_UTC=%s\nDANOS_OPEN_PACKAGE_VERSION=%s\nDANOS_OPEN_PACKAGE_SHA256=%s\nDANOS_VPP_PACKAGE_VERSION=%s\nDANOS_VPP_PACKAGE_SHA256=%s\nDANOS_VPP_IMAGE=%s\nDANOS_BASE_IMAGE=%s\nDANOS_BASE_IMAGE_SHA256=%s\n' \
    "$RAW_BYTES" "$RAW_SHA" "$GZIP_SHA" \
    "$SOURCE_COMMIT" "$SOURCE_DIRTY" "$BUILD_UTC" \
    "$DANOS_DEB_VERSION" "$DANOS_DEB_SHA256" "$VPP_DEB_VERSION" "$VPP_DEB_SHA256" \
    "${VPP_IMAGE:-danos-vpp-runtime-recover:local}" \
    "$BASE_IMAGE" "$(sha256sum "$BASE_IMAGE" | awk '{print $1}')" \
    > "$MANIFEST"
mv "$OVERLAY" "$QCOW2"
rm -f "$RAW_DISK"
echo "INSTALLED_DISK_QCOW2=$QCOW2"
echo "INSTALLER_PAYLOAD=$RAW_GZ"
echo "SERIAL_LOG=$SERIAL_LOG"
echo "INSTALL_MANIFEST=$MANIFEST"
cat "$MANIFEST"
