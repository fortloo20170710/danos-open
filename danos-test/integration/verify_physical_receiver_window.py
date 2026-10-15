#!/usr/bin/env python3
"""Correlate identity-bound before/after NIC RX counters from R1 and R4."""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path

from capture_physical_receiver_serial import RECEIVERS, validate_result


class WindowError(ValueError):
    pass


def read_snapshot(path: Path, role: str, phase: str) -> dict:
    try:
        result = json.loads(path.read_text())
        validate_result(result, role, phase)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        raise WindowError(f"{role} {phase} snapshot invalid: {error}") from None
    return result


def verify_window(paths: dict[str, tuple[Path, Path]], expected_rx_total: int | None = None) -> dict:
    peers = {}
    for role in ("R1", "R4"):
        if role not in paths:
            raise WindowError(f"missing {role} receiver snapshots")
        before_path, after_path = paths[role]
        before = read_snapshot(before_path, role, "before")
        after = read_snapshot(after_path, role, "after")
        if before["pid"] != after["pid"]:
            raise WindowError(f"{role} VPP PID changed during receiver window")
        before_time = before.get("captured_monotonic", before["captured_unix_time"])
        after_time = after.get("captured_monotonic", after["captured_unix_time"])
        if after_time <= before_time:
            raise WindowError(f"{role} after snapshot is not later than before snapshot")
        rx_delta = after["rx_good_packets"] - before["rx_good_packets"]
        tx_delta = after["tx_good_packets"] - before["tx_good_packets"]
        if rx_delta < 0 or tx_delta < 0:
            raise WindowError(f"{role} hardware counters decreased/reset")
        peers[role] = {
            "mac": RECEIVERS[role]["mac"], "interface": RECEIVERS[role]["interface"],
            "pid": before["pid"], "rx_before": before["rx_good_packets"],
            "rx_after": after["rx_good_packets"], "rx_delta": rx_delta,
            "tx_before": before["tx_good_packets"], "tx_after": after["tx_good_packets"],
            "tx_delta": tx_delta,
            "before_sha256": hashlib.sha256(before_path.read_bytes()).hexdigest(),
            "after_sha256": hashlib.sha256(after_path.read_bytes()).hexdigest(),
        }
    rx_total = sum(item["rx_delta"] for item in peers.values())
    if expected_rx_total is not None and rx_total != expected_rx_total:
        raise WindowError(f"independent receiver RX total {rx_total} != expected {expected_rx_total}")
    return {
        "schema_version": 1, "status": "PASS", "scope": "independent R1/R4 NIC counter window",
        "receiver_counter_source": "external-nic", "receiver_packets_rx": rx_total,
        "expected_rx_total": "" if expected_rx_total is None else expected_rx_total,
        "recorded_utc": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "receivers": peers,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for role in ("r1", "r4"):
        parser.add_argument(f"--{role}-before", type=Path, required=True)
        parser.add_argument(f"--{role}-after", type=Path, required=True)
    parser.add_argument("--expected-rx-total", type=int)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = verify_window({
            "R1": (args.r1_before, args.r1_after),
            "R4": (args.r4_before, args.r4_after),
        }, args.expected_rx_total)
    except WindowError as error:
        result = {"schema_version": 1, "status": "FAIL", "failure_reason": str(error)}
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result))
        return 1
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"status": result["status"], "receiver_packets_rx": result["receiver_packets_rx"],
                      "out": str(args.out)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
