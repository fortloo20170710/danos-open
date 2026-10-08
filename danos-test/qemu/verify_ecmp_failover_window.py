#!/usr/bin/env python3
"""Validate measured ECMP packet loss while a next-hop is withdrawn/restored."""

import argparse
import re
import sys
from pathlib import Path


MARKER = re.compile(
    r"VPP-ECMP-FAILOVER-WINDOW PASS flows=(?P<flows>\d+) "
    r"probes_per_flow=(?P<per_flow>\d+) total_tx=(?P<tx>\d+) "
    r"total_rx=(?P<rx>\d+) loss_pct=(?P<loss>\d+(?:\.\d+)?) "
    r"duration_ms=(?P<duration>\d+) max_loss_pct=(?P<max_loss>\d+) "
    r"baseline_bucket0=(?P<bucket0>\d+) baseline_bucket1=(?P<bucket1>\d+)"
)


def validate(text: str, probes_per_flow: int, max_loss_pct: int) -> str:
    if re.search(r"VPP-ECMP-FAILOVER-WINDOW(?:-FLOW)? FAIL", text):
        raise ValueError("failover-window FAIL marker present")
    matches = list(MARKER.finditer(text))
    if not matches:
        raise ValueError("failover-window PASS marker missing or malformed")
    fields = {key: float(value) for key, value in matches[-1].groupdict().items()}
    expected_tx = probes_per_flow * 4
    if fields["flows"] != 4 or fields["per_flow"] != probes_per_flow:
        raise ValueError("flow count does not match the configured test")
    if fields["tx"] != expected_tx or fields["rx"] > fields["tx"]:
        raise ValueError("packet totals do not match four flows")
    if fields["duration"] <= 0:
        raise ValueError("measurement duration must be positive")
    if fields["bucket0"] <= 0 or fields["bucket1"] <= 0:
        raise ValueError("baseline did not exercise both ECMP buckets")
    calculated_loss = (fields["tx"] - fields["rx"]) * 100 / fields["tx"]
    if abs(calculated_loss - fields["loss"]) > 0.02:
        raise ValueError("reported loss percentage disagrees with packet totals")
    if fields["loss"] > max_loss_pct or fields["max_loss"] != max_loss_pct:
        raise ValueError("measured loss exceeds or disagrees with the configured budget")
    return matches[-1].group(0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--probes-per-flow", type=int, default=200)
    parser.add_argument("--max-loss-pct", type=int, default=15)
    args = parser.parse_args()
    try:
        marker = validate(args.log.read_text(errors="replace"), args.probes_per_flow,
                          args.max_loss_pct)
    except (OSError, ValueError) as exc:
        print(f"[FAIL] QEMU ECMP failover-window: {exc}", file=sys.stderr)
        return 1
    print(f"[PASS] QEMU ECMP failover-window: {marker}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
