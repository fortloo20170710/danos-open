import subprocess
import base64
import json
import unittest
from unittest.mock import patch
from vpp_readonly import VppReadOnly
from serial_readonly import read_command, decode_reply


class ReadOnlyTests(unittest.TestCase):
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
