#!/usr/bin/env python3
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("test_multicore_perf.py")


class MulticorePerfCliTest(unittest.TestCase):
    def test_missing_trex_writes_structured_open_result(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "nested" / "result.json"
            run = subprocess.run(
                [sys.executable, str(SCRIPT), "--output", str(output)],
                text=True, capture_output=True, check=False)
            self.assertEqual(run.returncode, 2)
            self.assertIn("ENVIRONMENT-OPEN", run.stderr)
            result = json.loads(output.read_text())
            self.assertEqual(result["status"], "ENVIRONMENT-OPEN")
            self.assertFalse(result["accepted"])
            self.assertFalse(result["synthetic"])
            self.assertIn("TRex integration is not available", result["reason"])

    def test_simulation_is_marked_non_acceptance(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "demo.json"
            run = subprocess.run(
                [sys.executable, str(SCRIPT), "--simulate", "--cores", "2",
                 "--output", str(output)], text=True, capture_output=True,
                check=False)
            self.assertEqual(run.returncode, 0, run.stderr)
            result = json.loads(output.read_text())
            self.assertEqual(result["status"], "SIMULATED")
            self.assertFalse(result["accepted"])
            self.assertTrue(result["synthetic"])
            self.assertEqual(len(result["result"]["per_core"]), 2)

    def test_simulated_scaling_never_prints_acceptance_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "scaling-demo.json"
            run = subprocess.run(
                [sys.executable, str(SCRIPT), "--simulate", "--cores", "4",
                 "--validate-scaling", "--output", str(output)],
                text=True, capture_output=True, check=False)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn("SIMULATED (demo only; not acceptance data)", run.stdout)
            self.assertNotIn("  PASS", run.stdout)
            result = json.loads(output.read_text())
            self.assertEqual(result["status"], "SIMULATED")
            self.assertFalse(result["accepted"])


if __name__ == "__main__":
    unittest.main()
