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
assert 'LABEL install' in builder and 'danos.install=1' in builder
assert 'LABEL install-serial' in builder and 'danos.install.input=serial' in builder

mode_check = init.index('test "$kernel_arg" = danos.install=1')
module_load = init.index('done < /modules.load')
installer_exec = init.index('/bin/busybox sh /bin/danos-install')
assert mode_check < module_load < installer_exec
assert 'DANOS INSTALLER PASS' in (root / "danos-test/installer/install_disk.sh").read_text()
assert 'DANOS_INSTALLER_INPUT' in (root / "danos-test/installer/install_disk.sh").read_text()
runner_builder = (root / "danos-test/installer/build_i211_disk_image.sh").read_text()
runtime_package = (root / "danos-test/installer/package_vpp_runtime.sh").read_text()
assert "DANOS-RUNNER-ROOTFS-CONFIGURED PASS" in runner_builder
assert "systemctl restart vpp.service" in runner_builder
assert "api.sock" in runner_builder and "vppctl -s /run/vpp/cli.sock show version" in runner_builder
assert "/etc/modules-load.d/danos-dpdk.conf" in runner_builder
assert "After=local-fs.target systemd-modules-load.service" in runtime_package
assert (root / "danos-test/installer/build_dpdk_installable_iso.sh").is_file()
generic_iso_builder = (root / "danos-test/installer/build_dpdk_installable_iso.sh").read_text()
assert "-boot_image any replay" in generic_iso_builder
assert "/installer/danos-i211-installed.raw.gz" in generic_iso_builder
assert "DANOS_INSTALLER_PAYLOAD_SHA256" in generic_iso_builder

print("Install ISO boot-path wiring PASS")
