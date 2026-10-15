#!/usr/bin/env python3
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from send_host_udp64 import (build_payload, route_matches, inspect_routes,
                             send_batch, validate_args)
import tempfile


class HostUdp64Tests(unittest.TestCase):
    def test_route_guard_requires_exact_gateway_device_and_source(self):
        good = "30.30.30.2 via 192.168.1.244 dev enp4s0 src 192.168.1.243 uid 1000"
        self.assertTrue(route_matches(good, gateway="192.168.1.244",
                                     interface="enp4s0", source="192.168.1.243"))
        for bad in (good.replace("192.168.1.244", "192.168.1.1"),
                    good.replace("enp4s0", "wlp3s0"),
                    good.replace("192.168.1.243", "10.248.5.61"),
                    "unreachable 30.30.30.2"):
            self.assertFalse(route_matches(bad, gateway="192.168.1.244",
                                           interface="enp4s0", source="192.168.1.243"))

    def test_route_get_from_selector_is_accepted_as_source_identity(self):
        output = "30.30.30.2 from 192.168.1.243 via 192.168.1.244 dev enp4s0 uid 1000"
        self.assertTrue(route_matches(output, gateway="192.168.1.244",
                                      interface="enp4s0", source="192.168.1.243"))

    @patch("send_host_udp64.subprocess.run")
    def test_inspect_routes_does_not_change_routes(self, run):
        run.return_value.returncode = 0
        run.return_value.stdout = "30.30.30.2 via 192.168.1.244 dev enp4s0 src 192.168.1.243\n"
        routes = inspect_routes(["30.30.30.2"], gateway="192.168.1.244",
                                interface="enp4s0", source="192.168.1.243")
        self.assertTrue(routes["30.30.30.2"]["matches"])
        self.assertEqual(run.call_args.args[0][:4], ["ip", "-4", "route", "get"])

    def test_arguments_are_bounded_and_distinct(self):
        validate_args("192.168.1.243", "192.168.1.244",
                      ["30.30.30.2", "30.30.30.3"], 1000, 4096, "enp4s0")
        for rate, count, targets in ((1001, 1, ["30.30.30.2"]),
                                     (1, 4097, ["30.30.30.2"]),
                                     (1, 1, ["30.30.30.2"] * 2)):
            with self.assertRaises(ValueError):
                validate_args("192.168.1.243", "192.168.1.244", targets,
                              rate, count, "enp4s0")
        with self.assertRaises(ValueError):
            validate_args("192.168.1.243", "192.168.1.244",
                          ["30.30.30.2"], 100, 1, "../enp4s0")

    def test_payload_has_64_byte_ethernet_frame_size_without_fcs(self):
        self.assertEqual(len(build_payload(123, 2)), 22)

    @patch("send_host_udp64.inspect_routes")
    def test_route_mismatch_is_structured_and_sends_nothing(self, routes):
        routes.return_value = {target: {"matches": False, "output": "via wrong gateway"}
                               for target in ("30.30.30.2",)}
        with patch("send_host_udp64.interface_carrier", return_value=True):
            with tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / "result.json"
                result = send_batch(source="192.168.1.243", gateway="192.168.1.244",
                                    interface="enp4s0", targets=["30.30.30.2"],
                                    rate=100, count=1, output=output)
                self.assertEqual(result["status"], "ENVIRONMENT-OPEN")
                self.assertEqual(result["sent_per_target"]["30.30.30.2"], 0)
                self.assertTrue(output.is_file())


if __name__ == "__main__":
    unittest.main()
