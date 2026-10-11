import unittest
import subprocess
import sys
from pathlib import Path
from probe_pg_pacing import stream_counts, check_pacing


class PacingTests(unittest.TestCase):
    def test_invalid_enable_gap_rejected_before_runtime(self):
        for gap in ("nan", "inf", "-1", "1.01"):
            response = subprocess.run([
                sys.executable, str(Path(__file__).with_name("probe_pg_pacing.py")),
                "--source-mac", "invalid", "--target-mac", "invalid",
                "--source-ip", "invalid", "--interface", "invalid",
                "--target", "invalid", "--out", "/tmp/unused-pacing-gap.json",
                "--enable-gap", gap], capture_output=True, text=True)
            self.assertEqual(response.returncode, 2)
            self.assertIn("enable-gap must be finite", response.stderr)

    def test_pacing_guard(self):
        rows = {"flow": dict(enabled=True, count=1000)}
        check_pacing(rows, ["flow"], [[10, 10.01]], 11, 1000, 4096)
        with self.assertRaises(ValueError):
            check_pacing(rows, ["flow"], [[10, 10.01]], 10.1, 1000, 4096)
        with self.assertRaises(ValueError):
            check_pacing(rows, ["flow"], [[10, 10.01]], 9, 1000, 4096)
        with self.assertRaises(ValueError):
            check_pacing(rows, ["flow"], [[10, 10.01]], 20, 1000, 256)

    def test_exact_counts_and_enabled_state(self):
        self.assertEqual(stream_counts("pace1000_0 No 256 limit 256, rate 1e3\n",
                                       ["pace1000_0"]),
                         {"pace1000_0": dict(enabled=False, count=256)})

    def test_missing_or_truncated_rows_are_rejected(self):
        for raw in ("no streams currently defined", "pace100 No 256 limit 256"):
            with self.assertRaises(ValueError):
                stream_counts(raw, ["pace1000_0"])


if __name__ == "__main__":
    unittest.main()
