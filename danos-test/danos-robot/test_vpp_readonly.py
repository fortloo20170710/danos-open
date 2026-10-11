import subprocess
import unittest
from unittest.mock import patch
from vpp_readonly import VppReadOnly


class ReadOnlyTests(unittest.TestCase):
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
