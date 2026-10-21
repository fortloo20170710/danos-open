#!/usr/bin/env python3
"""Missing archived VM evidence must be reported as environment-open."""

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "danos-test/perf/run_virtual_baseline_lanes.sh"


class VirtualBaselineLanePreflight(unittest.TestCase):
    def test_missing_qemu_and_vmware_artifacts_are_structured_open(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "results"
            env = os.environ.copy()
            env.update({
                "BASELINE_OUT_DIR": str(output),
                "QEMU_TOPOLOGY_DIR": str(Path(temp) / "missing-qemu"),
                "VMWARE_ISO": str(Path(temp) / "missing-vmware.iso"),
                "VMWARE_PEER_LOG": str(Path(temp) / "missing-peer.log"),
                "VMWARE_DANOS_LOG": str(Path(temp) / "missing-danos.log"),
            })
            result = subprocess.run(
                ["bash", str(SCRIPT)], env=env, capture_output=True, text=True,
                timeout=10, check=False,
            )
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            qemu = (output / "qemu.env").read_text()
            self.assertIn("status=ENVIRONMENT-OPEN", qemu)
            self.assertIn("lane=qemu-e1000", qemu)
            report = json.loads((output / "baselines.json").read_text())
            self.assertEqual(report["status"], "ENVIRONMENT-OPEN")
            self.assertEqual(
                {lane["lane"]: lane["status"] for lane in report["lanes"]},
                {"qemu-e1000": "ENVIRONMENT-OPEN",
                 "vmware-vmxnet3-polling": "ENVIRONMENT-OPEN"},
            )
            self.assertFalse(any(
                lane["physical_performance_accepted"] for lane in report["lanes"]
            ))


if __name__ == "__main__":
    unittest.main()
