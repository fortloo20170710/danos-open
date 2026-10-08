#!/usr/bin/env python3
"""Verify both PCI DPDK preflights emit the common versioned SKIP schema."""

import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RUNNERS = (
    ROOT / "danos-test/integration/run_vpp_dpdk_lane.sh",
    ROOT / "danos-test/integration/run_vpp_dpdk_container_lane.sh",
)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="dpdk-preflight-result-") as tmp:
        for index, runner in enumerate(RUNNERS):
            result_path = Path(tmp) / f"preflight-{index}.env"
            env = os.environ.copy()
            for key in ("DPDK_PCI_BDF", "DPDK_ISO", "DPDK_PCI_DRIVER"):
                env.pop(key, None)
            env["DPDK_RESULT_FILE"] = str(result_path)
            proc = subprocess.run(
                ["bash", str(runner)], cwd=ROOT, env=env,
                capture_output=True, text=True, timeout=10, check=False,
            )
            if proc.returncode != 2:
                raise AssertionError(
                    f"{runner.name} returned {proc.returncode}, expected SKIP (2):\n"
                    f"{proc.stdout}{proc.stderr}"
                )
            result = result_path.read_text()
            expected = (
                "schema_version=1\n",
                "status=SKIP\n",
                "preflight_status=SKIP\n",
                "performance_status=ENVIRONMENT-OPEN\n",
                "lane=pci-dpdk\n",
                "target_bdf=''\n",
            )
            for field in expected:
                if field not in result:
                    raise AssertionError(
                        f"{runner.name} result missing {field!r}:\n{result}"
                    )
    print("PASS: both PCI preflights emit the versioned structured SKIP schema")


if __name__ == "__main__":
    main()
