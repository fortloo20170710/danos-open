#!/usr/bin/env python3
"""Correlate a bounded R2 64-byte UDP run across R3 and independent peers."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re

from run_physical_pg_serial import validate_result
from verify_physical_receiver_window import verify_window as verify_receivers

R3_PATHS = (("10.20.0.2", "GigabitEthernet1/0/0"),
            ("10.30.0.2", "GigabitEthernet2/0/0"))
R3_PORTS = ("GigabitEthernet1/0/0", "GigabitEthernet2/0/0",
            "GigabitEthernet4/0/0")
R3_MACS = {"GigabitEthernet1/0/0": "00:1f:7a:69:f7:4c",
           "GigabitEthernet2/0/0": "00:1f:7a:69:f7:4d"}


class WindowError(ValueError):
    pass


def read_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise WindowError(f"cannot read {path}: {error}") from None
    if not isinstance(value, dict):
        raise WindowError(f"{path} must contain a JSON object")
    return value


def parse_interfaces(raw: str) -> dict:
    result = {}
    for block in re.split(r"(?m)(?=^GigabitEthernet)", raw):
        match = re.match(r"(GigabitEthernet\d+/\d+/\d+)", block)
        if not match:
            continue
        counters = {key: int(value) for key, value in re.findall(
            r"\b(rx packets|tx packets|drops)\s+(\d+)", block)}
        result[match.group(1)] = counters
    return result


def validate_fib(raw: str) -> None:
    if not re.search(r"\bbuckets:2\b", raw):
        raise WindowError("R3 FIB must have exactly two ECMP buckets")
    paths = set()
    blocks = re.findall(r"(?m)^\s*path:\[\d+\](.*?)(?=^\s*path:\[|^\s*forwarding:|\Z)",
                        raw, re.S | re.M)
    forwarding = raw.split("forwarding:", 1)[-1]
    for gateway, interface in R3_PATHS:
        resolved = any("oper-flags:resolved" in block and
                       re.search(re.escape(gateway) + r"\s+" + re.escape(interface) + r"\b", block)
                       for block in blocks)
        active = re.search(r"ipv4 via\s+" + re.escape(gateway) + r"\s+" +
                           re.escape(interface) + r"\b", forwarding)
        if resolved and active:
            paths.add((gateway, interface))
    if paths != set(R3_PATHS):
        raise WindowError("R3 FIB paths are not both resolved forwarding adjacencies")


def verify_window(paths: dict[str, Path]) -> dict:
    required = ("source", "r3_before", "r3_after", "r1_before", "r1_after",
                "r4_before", "r4_after")
    missing = [key for key in required if key not in paths]
    if missing:
        raise WindowError("missing evidence paths: " + ",".join(missing))
    source = read_json(paths["source"])
    try:
        count = source["frames_per_flow_per_phase"]
        validate_result(source, count)
    except (KeyError, TypeError, ValueError) as error:
        raise WindowError(f"R2 source result invalid: {error}") from None

    before, after = read_json(paths["r3_before"]), read_json(paths["r3_after"])
    for snapshot, phase in ((before, "before"), (after, "after")):
        if (snapshot.get("status") != "PASS" or snapshot.get("phase") != phase or
                snapshot.get("scope") != "read-only R3 interface counters and FIB"):
            raise WindowError(f"R3 {phase} snapshot did not pass")
        if not re.fullmatch(r"[1-9]\d*", str(snapshot.get("pid", ""))):
            raise WindowError(f"R3 {phase} VPP PID is invalid")
        validate_fib(snapshot.get("fib", ""))
        for interface, mac in R3_MACS.items():
            if not re.search(r"Ethernet address\s+" + re.escape(mac) + r"\b",
                             snapshot.get("hardware_" + ("p1" if interface.endswith("1/0/0") else "p2"), ""), re.I):
                raise WindowError(f"R3 hardware identity mismatch on {interface}")
    if before["pid"] != after["pid"]:
        raise WindowError("R3 VPP process changed during traffic window")
    for snapshot in (before, after):
        timestamp = snapshot.get("captured_unix_time")
        if type(timestamp) not in (int, float) or not math.isfinite(timestamp):
            raise WindowError("R3 capture timestamp is invalid")
    if after["captured_unix_time"] <= before["captured_unix_time"]:
        raise WindowError("R3 after snapshot is not later than before snapshot")
    first, last = parse_interfaces(before.get("interfaces", "")), parse_interfaces(after.get("interfaces", ""))
    if any(port not in first or port not in last for port in R3_PORTS):
        raise WindowError("R3 interface counter snapshot is incomplete")
    counter_fields = {"rx packets", "tx packets", "drops"}
    if any(not counter_fields.issubset(snapshot[port])
           for snapshot in (first, last) for port in R3_PORTS):
        raise WindowError("R3 packet/drop counters are incomplete")
    r3 = {port: {key: last[port][key] - first[port][key]
                 for key in ("rx packets", "tx packets", "drops")}
          for port in R3_PORTS}
    if any(value < 0 for counters in r3.values() for value in counters.values()):
        raise WindowError("R3 interface counter reset/decrease detected")
    generated = source["generated_frames"]
    p1, p2, p4 = R3_PORTS
    if r3[p4]["rx packets"] != generated:
        raise WindowError("R3 P4 RX does not match R2 generated frames")
    if r3[p1]["tx packets"] + r3[p2]["tx packets"] != generated:
        raise WindowError("R3 aggregate ECMP egress does not match generated frames")
    if any(r3[port]["drops"] for port in R3_PORTS):
        raise WindowError("R3 drop counter increased during traffic window")

    try:
        receiver = verify_receivers({
            "R1": (paths["r1_before"], paths["r1_after"]),
            "R4": (paths["r4_before"], paths["r4_after"]),
        }, expected_rx_total=generated)
    except ValueError as error:
        raise WindowError(str(error)) from None
    receiver_deltas = {role: receiver["receivers"][role]["rx_delta"] for role in ("R1", "R4")}
    # R3 P1 is physically connected to R4; P2 is physically connected to R1.
    if receiver_deltas["R4"] != r3[p1]["tx packets"]:
        raise WindowError("R4 independent RX does not match R3 P1 egress")
    if receiver_deltas["R1"] != r3[p2]["tx packets"]:
        raise WindowError("R1 independent RX does not match R3 P2 egress")

    evidence_hashes = {key: hashlib.sha256(path.read_bytes()).hexdigest()
                       for key, path in paths.items()}
    return {
        "schema_version": 1, "status": "PASS",
        "scope": "bounded physical 64-byte UDP end-to-end functional window",
        "performance_status": "ENVIRONMENT-OPEN",
        "performance_limitations": ["latency not measured", "CPU not measured",
                                    "window-average rate is not line-rate qualification"],
        "generated_frames": generated,
        "r2_hardware_tx_delta": source["hardware_tx_delta"],
        "r2_tx_drops_before": source["tx_drops_before"],
        "r2_tx_drops_after": source["tx_drops_after"],
        "phase_rates": [{key: phase[key] for key in (
            "requested_pps_per_flow", "hardware_tx_delta", "observation_seconds",
            "measured_tx_pps", "measured_mbps")} for phase in source["phases"]],
        "r3_pid": before["pid"], "r3_counter_deltas": r3,
        "independent_receiver_rx_deltas": receiver_deltas,
        "receiver_packets_rx": receiver["receiver_packets_rx"],
        "evidence_sha256": evidence_hashes,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "r3-before", "r3-after", "r1-before", "r1-after", "r4-before", "r4-after"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    paths = {"source": args.source, "r3_before": args.r3_before, "r3_after": args.r3_after,
             "r1_before": args.r1_before, "r1_after": args.r1_after,
             "r4_before": args.r4_before, "r4_after": args.r4_after}
    try:
        result = verify_window(paths)
    except (WindowError, OSError, ValueError, TypeError, KeyError) as error:
        result = {"schema_version": 1, "status": "FAIL", "failure_reason": str(error),
                  "performance_status": "ENVIRONMENT-OPEN"}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"status": result["status"], "generated_frames": result.get("generated_frames"),
                      "receiver_packets_rx": result.get("receiver_packets_rx"),
                      "performance_status": result["performance_status"], "out": str(args.out)}))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
