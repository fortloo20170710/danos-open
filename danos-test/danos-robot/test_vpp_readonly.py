import subprocess
import base64
import json
import tempfile
import unittest
from unittest.mock import patch
from vpp_readonly import VppReadOnly
from serial_readonly import read_command, decode_reply
from capture_installed_inventory import qualify


class ReadOnlyTests(unittest.TestCase):
    def inventory(self):
        result = {name: {"returncode": 0, "stdout": "", "stderr": ""}
                  for name in ("root", "kernel", "disks", "pci", "services", "vpp_interfaces", "vpp_version")}
        result["root"]["stdout"] = "/dev/sda1 ext4\n"
        result["disks"]["stdout"] = json.dumps({"blockdevices": [{"tran": "sata", "children": [{"mountpoints": ["/"]}]}]})
        result["services"]["stdout"] = "Id=vpp.service\nActiveState=active\nSubState=running\nMainPID=11\n\nId=frr.service\nActiveState=active\nSubState=running\nMainPID=12\n\nId=danos-mgrd.service\nActiveState=inactive\nUnitFileState=disabled\n"
        result["pci"]["stdout"] = "".join(f"0{i}:00.0 Ethernet [8086:1539]\n\tKernel driver in use: uio_pci_generic\n" for i in range(1, 5))
        result["vpp_interfaces"]["stdout"] = "".join(f"GigabitEthernet{i}/0/0 {i} down\n" for i in range(1, 5))
        result["files"] = {"/etc/danos/build-info.env": "DANOS_BUILD_COMMIT=abc\nDANOS_BUILD_SOURCE_DIRTY=0\n"}
        result["mgrd_sha256"] = "def"
        result["sockets"] = dict.fromkeys(("/run/vpp/api.sock", "/run/vpp/stats.sock", "/run/vpp/cli.sock"), True)
        return result

    def test_installed_inventory_does_not_require_live_traffic(self):
        self.assertTrue(all(qualify(self.inventory(), "abc", "def").values()))

    def test_installed_inventory_rejects_live_usb_or_wrong_binary(self):
        inv = self.inventory()
        self.assertFalse(qualify(inv, "abc", "wrong")["binary_identity"])
        inv["disks"]["stdout"] = json.dumps({"blockdevices": [{"tran": "usb", "children": [{"mountpoints": ["/"]}]}]})
        self.assertFalse(qualify(inv, "abc", "def")["installed_root"])

    def test_inventory_requires_all_bindings_and_sockets(self):
        inv = self.inventory()
        inv["pci"]["stdout"] = inv["pci"]["stdout"].replace("uio_pci_generic", "igb", 1)
        inv["sockets"].pop("/run/vpp/stats.sock")
        verdict = qualify(inv, "abc", "def")
        self.assertFalse(verdict["four_i211_dpdk_bindings"])
        self.assertFalse(verdict["vpp_sockets"])

    def test_uart_refuses_regular_file_without_writing(self):
        with tempfile.NamedTemporaryFile() as target:
            target.write(b"untouched")
            target.flush()
            with self.assertRaises(ValueError):
                read_command(target.name, ["show", "version"])
            target.seek(0)
            self.assertEqual(target.read(), b"untouched")

    def test_service_active_without_process_is_not_ready(self):
        inv = self.inventory()
        inv["services"]["stdout"] = inv["services"]["stdout"].replace("MainPID=12", "MainPID=0")
        self.assertFalse(qualify(inv, "abc", "def")["vpp_frr_active"])

    def test_partial_uart_reply_is_not_decoded(self):
        payload = base64.b64encode(json.dumps([0, "vpp version", ""]).encode())
        prefix = b"MARK:"
        self.assertIsNone(decode_reply(prefix + payload[:5], "MARK:"))
        self.assertIsNone(decode_reply(prefix + payload, "MARK:"))
        self.assertEqual(decode_reply(prefix + payload + b"\r\n", "MARK:"),
                         [0, "vpp version", ""])
        self.assertEqual(decode_reply(b"\x1b[?2004l\r" + prefix + payload + b"\r\n", "MARK:"),
                         [0, "vpp version", ""])

    def test_uart_rejects_mutating_commands_before_open(self):
        for command in (["clear", "errors"], ["set", "interface", "state"],
                        ["show", "hardware-interfaces", "port; reboot"]):
            with patch("serial_readonly.os.open") as opened:
                with self.assertRaises(ValueError):
                    read_command("/dev/ttyUSB0", command)
                opened.assert_not_called()

    def test_rejects_wrong_identity_before_suite(self):
        with patch("vpp_readonly.subprocess.run", return_value=subprocess.CompletedProcess([], 0, "Ethernet address 00:11:22:33:44:55", "")):
            with self.assertRaises(AssertionError):
                VppReadOnly().interface_identity_should_match("GigabitEthernet1/0/0", "00:11:22:33:44:66")

    def test_exit_zero_invalid_output_fails(self):
        with patch("vpp_readonly.subprocess.run", return_value=subprocess.CompletedProcess([], 0, "unknown input", "")):
            with self.assertRaises(AssertionError):
                VppReadOnly().runtime_version_should_be_available()

    def test_ecmp_requires_both_resolved_gateways(self):
        with patch("vpp_readonly.subprocess.run", return_value=subprocess.CompletedProcess([], 0, "30.30.30.0/24 buckets:2 ipv4 via 10.20.0.2 port", "")):
            with self.assertRaises(AssertionError):
                VppReadOnly().ecmp_route_should_have_two_buckets()

    def test_version_uses_readonly_argv(self):
        with patch("vpp_readonly.subprocess.run", return_value=subprocess.CompletedProcess([], 0, "vpp v26.10-test", "")) as run:
            VppReadOnly().runtime_version_should_be_available()
            self.assertEqual(run.call_args.args[0][-2:], ["show", "version"])
            self.assertNotIn("shell", run.call_args.kwargs)


if __name__ == "__main__":
    unittest.main()
