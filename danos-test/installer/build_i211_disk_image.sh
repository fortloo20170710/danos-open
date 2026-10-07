#!/usr/bin/env bash
# Construct a persistent Debian trixie I211 test-runner disk using the
# project's pinned VPP/DANOS .deb payloads, then boot it once for validation.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BASE_IMAGE=${DANOS_DEBIAN_BASE_IMAGE:-$ROOT/build/debian-13-generic-amd64.full.qcow2}
VPP_DEB=${DANOS_VPP_DEB:-$ROOT/build/danos-vpp-runtime_26.10-1_amd64.deb}
DANOS_DEB=${DANOS_OPEN_DEB:-$ROOT/build/danos-open_0.16.0~rc1-1_amd64.deb}
OUT_DIR=${1:-$ROOT/build/i211-installable}
MEMORY_MB=${DANOS_INSTALL_VM_MEMORY_MB:-4096}
VCPUS=${DANOS_INSTALL_VM_VCPUS:-4}
RUN_ID=$(date -u +%Y%m%dT%H%M%SZ)

for required in "$BASE_IMAGE" "$VPP_DEB" "$DANOS_DEB"; do
    test -r "$required" || { echo "ERROR: required input missing: $required" >&2; exit 2; }
done
for command in qemu-img qemu-system-x86_64 xorriso gzip sha256sum timeout; do
    command -v "$command" >/dev/null || { echo "ERROR: missing tool: $command" >&2; exit 2; }
done

mkdir -p "$OUT_DIR"
OUT_DIR=$(cd "$OUT_DIR" && pwd)
WORK=$(mktemp -d "$OUT_DIR/rootfs-work.XXXXXX")
OVERLAY="$WORK/danos-i211-installed.qcow2"
SEED="$WORK/cloud-init-seed.iso"
SERIAL_LOG="$OUT_DIR/danos-open-v0.16.0-rc1-i211-rootfs-$RUN_ID.serial.log"
QCOW2="$OUT_DIR/danos-open-v0.16.0-rc1-i211-installed-$RUN_ID.qcow2"
RAW_DISK="$WORK/danos-open-v0.16.0-rc1-i211-installed.raw"
RAW_GZ="$OUT_DIR/danos-open-v0.16.0-rc1-i211-installed-$RUN_ID.raw.gz"
MANIFEST="$OUT_DIR/danos-open-v0.16.0-rc1-i211-installed-$RUN_ID.manifest"
qemu-img check "$BASE_IMAGE"
qemu-img create -q -f qcow2 -F qcow2 -b "$BASE_IMAGE" "$OVERLAY"

SEED_TREE="$WORK/seed-tree"
mkdir -p "$SEED_TREE"
cp "$VPP_DEB" "$SEED_TREE/danos-vpp-runtime.deb"
cp "$DANOS_DEB" "$SEED_TREE/danos-open.deb"
cat > "$SEED_TREE/meta-data" <<'META'
instance-id: danos-i211-runner-v016rc1
local-hostname: danos-i211-runner
META
cat > "$SEED_TREE/user-data" <<'USERDATA'
#cloud-config
users:
  - default
  - name: danos
    gecos: DANOS I211 lab runner
    groups: [adm, sudo]
    shell: /bin/bash
    lock_passwd: true
    sudo: ["ALL=(ALL) NOPASSWD:ALL"]
write_files:
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
      profile=i211-physical-lab-runner
      vpp=26.10
      dpdk_driver=uio_pci_generic
      management_service=disabled-until-mtls-configured
  - path: /etc/issue
    permissions: '0644'
    content: |
      DANOS-Open I211 physical lab runner (Debian trixie; serial-console lab image)
      Root serial autologin is enabled for isolated hardware testing. Do not expose
      this image to an untrusted network. Configure management mTLS before enabling mgrd.
runcmd:
  - [sh, -c, 'sed -i "s|http://deb.debian.org/debian|http://repo.huaweicloud.com/debian|g; s|http://security.debian.org/debian-security|http://repo.huaweicloud.com/debian-security|g" /etc/apt/sources.list.d/debian.sources || true']
  - [sh, -c, 'mkdir -p /mnt/danos-seed; for n in 1 2 3 4 5 6 7 8 9 10; do mount -o ro /dev/vdb /mnt/danos-seed 2>/dev/null && break; sleep 1; done; test -r /mnt/danos-seed/danos-open.deb']
  - [sh, -c, 'apt-get update -o Acquire::Retries=3 -o Acquire::Languages=none']
  - [sh, -c, 'DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends /mnt/danos-seed/danos-vpp-runtime.deb /mnt/danos-seed/danos-open.deb frr iproute2 iputils-ping pciutils kmod systemd-sysv']
  - [sh, -c, 'systemctl disable --now ssh.service ssh.socket 2>/dev/null || true; systemctl mask ssh.service ssh.socket 2>/dev/null || true']
  - [sh, -c, 'systemctl disable --now danos-mgrd.service 2>/dev/null || true']
  - [sh, -c, 'systemctl enable serial-getty@ttyS0.service getty@tty1.service vpp.service']
  - [sh, -c, 'systemctl daemon-reload; systemctl start vpp.service']
  - [sh, -c, 'for n in $(seq 1 45); do test -S /run/vpp/api.sock && break; sleep 1; done; test -S /run/vpp/api.sock']
  - [sh, -c, 'vpp --version; vppctl -s /run/vpp/cli.sock show version; vppctl -s /run/vpp/cli.sock show hardware-interfaces']
  - [sh, -c, 'printf "DANOS-I211-ROOTFS-CONFIGURED PASS\n" > /dev/ttyS0']
  - [sh, -c, 'cloud-init clean --logs --machine-id; shutdown -h now']
USERDATA

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
if ! grep -Fq 'DANOS-I211-ROOTFS-CONFIGURED PASS' "$SERIAL_LOG"; then
    echo "ERROR: rootfs customization gate missing; qemu_rc=$QEMU_RC serial=$SERIAL_LOG" >&2
    tail -n 100 "$SERIAL_LOG" >&2 || true
    exit 1
fi

qemu-img convert -p -O raw "$OVERLAY" "$RAW_DISK"
RAW_BYTES=$(stat -c '%s' "$RAW_DISK")
RAW_SHA=$(sha256sum "$RAW_DISK" | awk '{print $1}')
gzip -1 -n -c "$RAW_DISK" > "$RAW_GZ"
GZIP_SHA=$(sha256sum "$RAW_GZ" | awk '{print $1}')
printf 'DANOS_INSTALLED_DISK_BYTES=%s\nDANOS_INSTALLED_DISK_SHA256=%s\nDANOS_INSTALLED_DISK_GZIP_SHA256=%s\nDANOS_SOURCE_COMMIT=%s\nDANOS_VPP_IMAGE=%s\n' \
    "$RAW_BYTES" "$RAW_SHA" "$GZIP_SHA" \
    "$(git -C "$ROOT" rev-parse HEAD)" "${VPP_IMAGE:-danos-vpp-runtime-recover:local}" \
    > "$MANIFEST"
mv "$OVERLAY" "$QCOW2"
rm -f "$RAW_DISK"
echo "INSTALLED_DISK_QCOW2=$QCOW2"
echo "INSTALLER_PAYLOAD=$RAW_GZ"
echo "SERIAL_LOG=$SERIAL_LOG"
echo "INSTALL_MANIFEST=$MANIFEST"
cat "$MANIFEST"
