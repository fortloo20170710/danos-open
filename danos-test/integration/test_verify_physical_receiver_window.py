#!/usr/bin/env python3
"""Tests for independent physical receiver counter window verification."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from capture_physical_receiver_serial import RECEIVERS
from verify_physical_receiver_window import WindowError, verify_window


class ReceiverWindowTest(unittest.TestCase):
    def write_snapshot(self, directory, role, phase, rx, tx=1000, pid="200"):
        path = directory / f"{role.lower()}-{phase}.json"
        path.write_text(json.dumps({
            "status": "PASS", "role": role, "phase": phase,
            "mac": RECEIVERS[role]["mac"], "interface": RECEIVERS[role]["interface"],
            "pid": pid, "captured_unix_time": 10 if phase == "before" else 20,
            "rx_good_packets": rx, "tx_good_packets": tx,
        }))
        return path

    def fixture(self, directory):
        return {
            "R1": (self.write_snapshot(directory, "R1", "before", 100),
                   self.write_snapshot(directory, "R1", "after", 175, 1075)),
            "R4": (self.write_snapshot(directory, "R4", "before", 200),
                   self.write_snapshot(directory, "R4", "after", 425, 1225)),
        }

    def test_aggregates_independent_rx_deltas(self):
        with tempfile.TemporaryDirectory() as temp:
            result = verify_window(self.fixture(Path(temp)), expected_rx_total=300)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["receiver_packets_rx"], 300)
        self.assertEqual(result["receivers"]["R1"]["rx_delta"], 75)
        self.assertEqual(result["receivers"]["R4"]["rx_delta"], 225)

    def test_expected_total_mismatch_fails(self):
        with tempfile.TemporaryDirectory() as temp, self.assertRaisesRegex(WindowError, "!= expected"):
            verify_window(self.fixture(Path(temp)), expected_rx_total=301)

    def test_pid_change_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            paths = self.fixture(Path(temp))
            after = json.loads(paths["R1"][1].read_text())
            after["pid"] = "201"
            paths["R1"][1].write_text(json.dumps(after))
            with self.assertRaisesRegex(WindowError, "PID changed"):
                verify_window(paths)

    def test_counter_reset_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            paths = self.fixture(Path(temp))
            after = json.loads(paths["R4"][1].read_text())
            after["rx_good_packets"] = 199
            paths["R4"][1].write_text(json.dumps(after))
            with self.assertRaisesRegex(WindowError, "decreased/reset"):
                verify_window(paths)


if __name__ == "__main__":
    unittest.main(verbosity=2)
