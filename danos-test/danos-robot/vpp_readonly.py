"""Original DANOS-Open read-only keywords; no legacy Vyatta commands."""
import re
import subprocess


class VppReadOnly:
    ROBOT_LIBRARY_SCOPE = "SUITE"

    def __init__(self, executable="/usr/bin/vppctl", socket="/run/vpp/cli.sock", serial_device=""):
        self.executable = executable
        self.socket = socket
        self.serial_device = serial_device

    def _read(self, command):
        if self.serial_device:
            from serial_readonly import read_command
            result = read_command(self.serial_device, command)
        else:
            result = subprocess.run([self.executable, "-s", self.socket, *command],
                                    capture_output=True, text=True, timeout=10)
        if result.returncode or not result.stdout.strip():
            raise AssertionError(f"read-only command failed: {command}: {result.stderr}")
        if re.search(r"unknown input|invalid command|connection refused", result.stdout, re.I):
            raise AssertionError("invalid operational command output")
        try:
            from robot.api import logger
        except ImportError:
            pass
        else:
            logger.info("read-only " + " ".join(command) + "\n" + result.stdout)
        return result.stdout

    def runtime_version_should_be_available(self):
        output = self._read(["show", "version"])
        if not re.search(r"\bvpp\s+v\S+", output):
            raise AssertionError("VPP version identity missing")

    def interface_identity_should_match(self, interface, expected_mac):
        if not re.fullmatch(r"GigabitEthernet\d+/\d+/\d+", interface):
            raise AssertionError("explicit physical interface required")
        if not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", expected_mac):
            raise AssertionError("explicit expected MAC required")
        output = self._read(["show", "hardware-interfaces", interface])
        if not re.search(r"Ethernet address\s+" + re.escape(expected_mac) + r"\b", output, re.I):
            raise AssertionError("physical node identity mismatch")

    def interface_counters_should_be_available(self):
        output = self._read(["show", "interface"])
        physical = re.findall(r"(?m)^GigabitEthernet\d+/\d+/\d+\s+\d+\s+", output)
        if not physical or not re.search(r"\bCounter\s+Count\b", output):
            raise AssertionError("physical interface counter table missing")

    def ecmp_route_should_have_two_buckets(self):
        output = self._read(["show", "ip", "fib", "30.30.30.0/24"])
        if "30.30.30.0/24" not in output or not re.search(r"\bbuckets:2\b", output):
            raise AssertionError("expected physical lab ECMP route absent")
        paths = re.findall(
            r"(?m)^\s*path:\[\d+\].*?(?=^\s*path:\[\d+\]|^\s*forwarding:)",
            output,
            re.S | re.M,
        )
        expected = {
            "10.20.0.2": "GigabitEthernet1/0/0",
            "10.30.0.2": "GigabitEthernet2/0/0",
        }
        for gateway, interface in expected.items():
            if not any(
                "oper-flags:resolved" in path
                and re.search(re.escape(gateway) + r"\s+" + re.escape(interface) + r"\b", path)
                for path in paths
            ):
                raise AssertionError("expected resolved ECMP next hop absent")


# Robot's path-based importer selects a class matching the module basename.
vpp_readonly = VppReadOnly
