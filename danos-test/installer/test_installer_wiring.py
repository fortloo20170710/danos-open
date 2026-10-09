#!/usr/bin/env python3
"""Guard the build-time and initramfs wiring for the install boot path."""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
builder = (root / "danos-test/live/build_live_iso.sh").read_text()
init = (root / "danos-test/live/init").read_text()

initramfs_reset = builder.index('rm -rf "$WORK/initramfs"')
installer_module_copy = builder.index('while read -r module_name module_path')
assert installer_module_copy > initramfs_reset, "installer modules must be staged after initramfs creation"
assert 'cp "$PROJECT_ROOT/danos-test/installer/install_disk.sh"' in builder
assert 'DANOS_INSTALLER_ENABLE="${DANOS_INSTALLER_ENABLE:-0}"' in builder
boot_cfg = (root / "danos-test/installer/dpdk-installer-isolinux.cfg").read_text()
assert 'LABEL install' in boot_cfg and 'danos.install=1' in boot_cfg
assert 'LABEL install-serial' in boot_cfg and 'danos.install.input=serial' in boot_cfg
assert 'dpdk-installer-isolinux.cfg' in builder
assert "PROMPT 1" in boot_cfg and "TIMEOUT 0" in boot_cfg
assert "CONSOLE 1" in boot_cfg and "SERIAL " not in boot_cfg
assert "DISPLAY /isolinux/boot.msg" in boot_cfg

mode_check = init.index('test "$kernel_arg" = danos.install=1')
module_load = init.index('done < /modules.load')
installer_exec = init.index('/bin/busybox sh /bin/danos-install')
assert mode_check < module_load < installer_exec
assert 'DANOS INSTALLER PASS' in (root / "danos-test/installer/install_disk.sh").read_text()
assert 'DANOS_INSTALLER_INPUT' in (root / "danos-test/installer/install_disk.sh").read_text()
installer = (root / "danos-test/installer/install_disk.sh").read_text()
assert "media_wait_seconds=${DANOS_INSTALLER_MEDIA_WAIT_SECONDS:-45}" in installer
assert "mdev -s" in installer and "waiting for USB/ISO block media" in installer
assert "DANOS-INSTALLER-RECOVERY" in init
assert 'DANOS-INSTALLER-COMPLETE' in init
assert init.index('DANOS-INSTALLER-COMPLETE') < init.index('# network drivers')
runner_builder = (root / "danos-test/installer/build_i211_disk_image.sh").read_text()
runtime_package = (root / "danos-test/installer/package_vpp_runtime.sh").read_text()
assert "DANOS-RUNNER-ROOTFS-CONFIGURED PASS" in runner_builder
assert "/etc/danos/build-info.env" in runner_builder
assert "DANOS_BUILD_SOURCE_DIRTY" in runner_builder
assert "DANOS_OPEN_PACKAGE_SHA256" in runner_builder and "DANOS_VPP_PACKAGE_SHA256" in runner_builder
assert "DANOS_SOURCE_DIRTY" in runner_builder and "dpkg-deb -f" in runner_builder
assert 'INSTALL_VM_TIMEOUT_SECONDS=${DANOS_INSTALL_VM_TIMEOUT_SECONDS:-1800}' in runner_builder
assert runner_builder.index('if test "$WAIT_RC" -eq 124; then') < runner_builder.index('wait "$QEMU_PID" 2>/dev/null\nQEMU_RC=')
assert "systemctl restart vpp.service" in runner_builder
assert "api.sock" in runner_builder and "vppctl -s /run/vpp/cli.sock show version" in runner_builder
assert "/etc/modules-load.d/danos-dpdk.conf" in runner_builder
assert "After=local-fs.target systemd-modules-load.service" in runtime_package
assert (root / "danos-test/installer/build_dpdk_installable_iso.sh").is_file()
generic_iso_builder = (root / "danos-test/installer/build_dpdk_installable_iso.sh").read_text()
assert 'ISO_DOCKER_NETWORK="${DANOS_ISO_DOCKER_NETWORK:-bridge}"' in builder
assert '--network "$ISO_DOCKER_NETWORK"' in builder
assert 'ISO_SKIP_APT="${DANOS_ISO_SKIP_APT:-0}"' in builder
assert 'DANOS_ISO_SKIP_APT must be 0 or 1' in builder
assert 'Acquire::http::Timeout=30 -o Acquire::Retries=2' in builder
assert 'Components: main\n' in builder
assert 'install -m 0755 "$ROOT/danos-test/live/init" "$TMP/initramfs/init"' in generic_iso_builder
assert 'cmp -s "$TMP/initramfs-check/init" "$ROOT/danos-test/live/init"' in generic_iso_builder
assert 'DANOS_INSTALLER_SD_MOD' in generic_iso_builder
assert 'test -s "$TMP/initramfs-check/modules/sd_mod.ko"' in generic_iso_builder
assert "-boot_image any replay" in generic_iso_builder
assert "/installer/danos-i211-installed.raw.gz" in generic_iso_builder
assert "DANOS_INSTALLER_PAYLOAD_SHA256" in generic_iso_builder
assert 'DANOS_INSTALLER_LIBZ' in generic_iso_builder
assert "/isolinux/isolinux.cfg" in generic_iso_builder

print("Install ISO boot-path wiring PASS")
