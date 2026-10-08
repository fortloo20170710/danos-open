#!/usr/bin/env python3
"""Regression tests for the failover-window acceptance evidence."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from verify_ecmp_failover_window import validate


PASS = (
    "VPP-ECMP-FAILOVER-WINDOW PASS flows=4 probes_per_flow=200 total_tx=800 "
    "total_rx=760 loss_pct=5.00 duration_ms=20000 max_loss_pct=15 "
    "baseline_bucket0=200 baseline_bucket1=180"
)


class FailoverWindowTest(unittest.TestCase):
    def test_accepts_measured_loss_within_budget(self):
        self.assertIn("loss_pct=5.00", validate(PASS, 200, 15))

    def test_requires_measurement(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            validate("VPP-ECMP-PATH-RESTORE PASS", 200, 15)

    def test_rejects_loss_over_budget(self):
        with self.assertRaisesRegex(ValueError, "exceeds"):
            validate(PASS.replace("total_rx=760 loss_pct=5.00",
                                  "total_rx=600 loss_pct=25.00"), 200, 15)

    def test_rejects_inconsistent_packet_totals(self):
        with self.assertRaisesRegex(ValueError, "packet totals"):
            validate(PASS.replace("total_tx=800", "total_tx=799"), 200, 15)

    def test_rejects_fail_marker_even_if_pass_is_also_present(self):
        with self.assertRaisesRegex(ValueError, "FAIL marker"):
            validate(PASS + "\nVPP-ECMP-FAILOVER-WINDOW FAIL", 200, 15)


if __name__ == "__main__":
    unittest.main(verbosity=2)
