#!/usr/bin/env python3
"""Offline safety and schema checks for receiver UART counter capture."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from capture_physical_receiver_serial import RECEIVERS, build_remote, validate_result


class ReceiverCaptureTest(unittest.TestCase):
    def valid(self, role="R1", phase="before"):
        return {"status": "PASS", "role": role, "phase": phase,
                "mac": RECEIVERS[role]["mac"],
                "interface": RECEIVERS[role]["interface"], "pid": "1234",
                "captured_unix_time": 1.0,
                "rx_good_packets": 100, "tx_good_packets": 90}

    def test_remote_capture_is_read_only_and_identity_guarded(self):
        for role, identity in RECEIVERS.items():
            script = build_remote(role, "before", "TEST_MARKER:")
            self.assertIn(identity["mac"], script)
            self.assertIn('"hardware-interfaces"', script)
            self.assertIn("rx_good_packets", script)
            for forbidden in ("systemctl restart", "packet-generator enable-stream",
                              "set interface", "ip route add", "ip route del"):
                self.assertNotIn(forbidden, script)

    def test_invalid_role_or_phase_is_rejected(self):
        for role, phase in (("R2", "before"), ("R1", "during")):
            with self.subTest(role=role, phase=phase), self.assertRaises(ValueError):
                build_remote(role, phase, "MARK:")

    def test_valid_snapshot_passes(self):
        validate_result(self.valid(), "R1", "before")

    def test_wrong_node_or_phase_is_rejected(self):
        result = self.valid()
        result["mac"] = RECEIVERS["R4"]["mac"]
        with self.assertRaisesRegex(ValueError, "mac mismatch"):
            validate_result(result, "R1", "before")

    def test_missing_or_invalid_counters_are_rejected(self):
        for value in (None, -1, "100"):
            with self.subTest(value=value):
                result = self.valid()
                result["rx_good_packets"] = value
                with self.assertRaisesRegex(ValueError, "rx_good_packets is invalid"):
                    validate_result(result, "R1", "before")

    def test_invalid_capture_timestamp_is_rejected(self):
        result = self.valid()
        result["captured_unix_time"] = float("nan")
        with self.assertRaisesRegex(ValueError, "timestamp is invalid"):
            validate_result(result, "R1", "before")


if __name__ == "__main__":
    unittest.main(verbosity=2)
