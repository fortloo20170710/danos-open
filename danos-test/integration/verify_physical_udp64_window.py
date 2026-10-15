#!/usr/bin/env python3
"""Correlate a bounded R2 64-byte UDP run across R3 and independent peers."""
import argparse
import datetime as dt
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
MAX_BRACKET_SECONDS = 1800
HOST_SCOPE = "independent-host paced UDP64 source batch"
HOST_TARGETS = ("30.30.30.2", "30.30.30.3", "30.30.30.4", "30.30.30.5")


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


def validate_host_source(source: dict) -> tuple[int, int]:
    """Validate source-side intent and route guards; NIC counters prove delivery."""
    try:
        sender = source["sender"]
        count = source["count_per_flow"]
        rate = source["requested_pps_per_flow"]
        sent = source["sent_per_target"]
        routes = source["routes"]
        if (source.get("status") != "PASS" or source.get("scope") != HOST_SCOPE or
                sender != {"source_ipv4": "192.168.1.243", "interface": "enp4s0",
                           "required_gateway": "192.168.1.244", "carrier_up": True}):
            raise ValueError("status, scope, or sender identity mismatch")
        if (type(count) is not int or not 1 <= count <= 4096 or
                type(rate) is not int or not 1 <= rate <= 1000 or
                source.get("targets") != list(HOST_TARGETS) or
                source.get("udp_destination_port") != 30000 or
                source.get("l2_frame_bytes_including_fcs") != 64 or
                source.get("udp_payload_bytes") != 22 or
                source.get("requested_total_frames") != count * len(HOST_TARGETS) or
                sent != {target: count for target in HOST_TARGETS} or
                source.get("sendto_completed") != count * len(HOST_TARGETS)):
            raise ValueError("target, packet size, or sent-frame totals mismatch")
        if set(routes) != set(HOST_TARGETS) or any(
                route.get("returncode") != 0 or route.get("matches") is not True or
                not re.search(r"\bvia 192\.168\.1\.244\b.*\bdev enp4s0\b",
                              route.get("output", "")) or
                not re.search(r"\b(?:src|from) 192\.168\.1\.243\b",
                              route.get("output", ""))
                for route in routes.values()):
            raise ValueError("one or more guarded routes do not use R2 via enp4s0")
        elapsed = source["elapsed_seconds"]
        if (type(elapsed) not in (int, float) or not math.isfinite(elapsed) or elapsed <= 0 or
                source.get("performance_status") != "ENVIRONMENT-OPEN"):
            raise ValueError("source timing/performance status invalid")
        started = dt.datetime.fromisoformat(source["started_utc"].replace("Z", "+00:00")).timestamp()
        finished = dt.datetime.fromisoformat(source["finished_utc"].replace("Z", "+00:00")).timestamp()
        if finished < started:
            raise ValueError("source timestamps are reversed")
        started_mono = source.get("started_monotonic")
        finished_mono = source.get("finished_monotonic")
        if (type(started_mono) not in (int, float) or
                type(finished_mono) not in (int, float) or
                not math.isfinite(started_mono) or not math.isfinite(finished_mono) or
                finished_mono < started_mono):
            raise ValueError("monotonic source timestamps are invalid")
        return count * len(HOST_TARGETS), (started, finished, started_mono, finished_mono)
    except (KeyError, TypeError, ValueError, AttributeError) as error:
        raise WindowError(f"independent host source result invalid: {error}") from None


def verify_window(paths: dict[str, Path]) -> dict:
    required = ("source", "r3_before", "r3_after")
    missing = [key for key in required if key not in paths]
    if missing:
        raise WindowError("missing evidence paths: " + ",".join(missing))
    receiver_keys = ("r1_before", "r1_after", "r4_before", "r4_after")
    has_receiver_evidence = any(key in paths for key in receiver_keys)
    if has_receiver_evidence and any(key not in paths for key in receiver_keys):
        raise WindowError("receiver evidence must include R1/R4 before/after snapshots")
    source = read_json(paths["source"])
    host_source = source.get("scope") == HOST_SCOPE
    if host_source:
        generated, source_started = validate_host_source(source)
    else:
        try:
            count = source["frames_per_flow_per_phase"]
            validate_result(source, count)
            generated, source_started = source["generated_frames"], None
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
    r3_cpu_pct = None
    cpu_before, cpu_after = before.get("vpp_cpu_sample"), after.get("vpp_cpu_sample")
    if cpu_before is not None or cpu_after is not None:
        if not isinstance(cpu_before, dict) or not isinstance(cpu_after, dict):
            raise WindowError("R3 VPP CPU samples are incomplete")
        for sample in (cpu_before, cpu_after):
            if (str(sample.get("pid")) != str(before["pid"]) or
                    not sample.get("start_ticks") or
                    type(sample.get("cpu_ticks")) is not int or sample["cpu_ticks"] < 0 or
                    type(sample.get("clock_ticks_per_second")) is not int or
                    sample["clock_ticks_per_second"] <= 0 or
                    type(sample.get("monotonic")) not in (int, float) or
                    not math.isfinite(sample["monotonic"])):
                raise WindowError("R3 VPP CPU sample is invalid")
        elapsed = cpu_after["monotonic"] - cpu_before["monotonic"]
        delta = cpu_after["cpu_ticks"] - cpu_before["cpu_ticks"]
        hz = cpu_before["clock_ticks_per_second"]
        if (cpu_before["start_ticks"] != cpu_after["start_ticks"] or elapsed <= 0 or
                delta < 0 or cpu_after["clock_ticks_per_second"] != hz):
            raise WindowError("R3 VPP CPU sample identity or interval changed")
        r3_cpu_pct = delta * 100 / (hz * elapsed)
        r3_cpu_elapsed = elapsed
    else:
        r3_cpu_elapsed = None
    for snapshot in (before, after):
        timestamp = snapshot.get("captured_unix_time")
        if type(timestamp) not in (int, float) or not math.isfinite(timestamp):
            raise WindowError("R3 capture timestamp is invalid")
    capture_before = before.get("host_capture_finished_monotonic")
    capture_after = after.get("host_capture_started_monotonic")
    if capture_before is None or capture_after is None:
        raise WindowError("R3 host-side monotonic capture boundaries are missing")
    if (type(capture_before) not in (int, float) or type(capture_after) not in (int, float) or
            not math.isfinite(capture_before) or not math.isfinite(capture_after)):
        raise WindowError("R3 host-side monotonic capture boundaries are invalid")
    if capture_after <= capture_before:
        raise WindowError("R3 after snapshot is not later than before snapshot")
    if source_started is not None:
        _, _, mono_start, mono_finish = source_started
        if not (capture_before <= mono_start <= mono_finish <= capture_after):
            raise WindowError("host source batch is not bracketed by R3 snapshots")
    snapshot_windows = {"R3": capture_after - capture_before}
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
    p1, p2, p4 = R3_PORTS
    if r3[p4]["rx packets"] != generated:
        raise WindowError("R3 P4 RX does not match R2 generated frames")
    if r3[p1]["tx packets"] + r3[p2]["tx packets"] != generated:
        raise WindowError("R3 aggregate ECMP egress does not match generated frames")
    if any(r3[port]["drops"] for port in R3_PORTS):
        raise WindowError("R3 drop counter increased during traffic window")

    receiver = None
    receiver_deltas = None
    if has_receiver_evidence:
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
        for role, before_key, after_key in (("R1", "r1_before", "r1_after"),
                                            ("R4", "r4_before", "r4_after")):
            peer_before, peer_after = read_json(paths[before_key]), read_json(paths[after_key])
            peer_start = peer_before.get("host_capture_finished_monotonic")
            peer_end = peer_after.get("host_capture_started_monotonic")
            if (type(peer_start) not in (int, float) or type(peer_end) not in (int, float) or
                    not math.isfinite(peer_start) or not math.isfinite(peer_end) or peer_end <= peer_start):
                raise WindowError(f"{role} host-side monotonic capture boundaries are invalid")
            snapshot_windows[role] = peer_end - peer_start

    tightly_bracketed = all(0 < seconds <= MAX_BRACKET_SECONDS
                            for seconds in snapshot_windows.values())
    status = "PASS" if tightly_bracketed else "OBSERVED"
    if tightly_bracketed:
        scope = ("bounded physical 64-byte UDP end-to-end functional window" if has_receiver_evidence
                 else "bounded physical 64-byte UDP R2-to-R3 forwarding window")
        performance_limitations = ["latency not measured", "R3 CPU snapshot interval is not phase-aligned",
                                   "window-average rate is not line-rate qualification"]
    else:
        scope = "counter-delta consistency over wide per-node snapshot intervals"
        performance_limitations = [
            "per-node snapshot interval exceeds 1800 seconds; exact deltas do not prove tight same-batch timing",
            "latency not measured", "R3 CPU snapshot interval is not phase-aligned",
            "window-average rate is not line-rate qualification"]

    evidence_hashes = {key: hashlib.sha256(path.read_bytes()).hexdigest()
                       for key, path in paths.items()}
    return {
        "schema_version": 1, "status": status,
        "scope": scope,
        "snapshot_windows_seconds": snapshot_windows,
        "snapshot_window_limit_seconds": MAX_BRACKET_SECONDS,
        "performance_status": "ENVIRONMENT-OPEN",
        "performance_limitations": performance_limitations,
        "generated_frames": generated,
        "source_kind": "independent-host" if host_source else "R2 VPP packet-generator",
        "r2_hardware_tx_delta": None if host_source else source["hardware_tx_delta"],
        "r2_tx_drops_before": None if host_source else source["tx_drops_before"],
        "r2_tx_drops_after": None if host_source else source["tx_drops_after"],
        "phase_rates": [] if host_source else [{key: phase[key] for key in (
            "requested_pps_per_flow", "hardware_tx_delta", "observation_seconds",
            "measured_tx_pps", "measured_mbps", "vpp_cpu_pct_one_core")
                        if key in phase} for phase in source["phases"]],
        "r3_pid": before["pid"], "r3_counter_deltas": r3,
        "r3_vpp_cpu_pct_one_core": r3_cpu_pct,
        "r3_cpu_sample_elapsed_seconds": r3_cpu_elapsed,
        "independent_receiver_rx_deltas": receiver_deltas,
        "receiver_packets_rx": receiver["receiver_packets_rx"] if receiver else None,
        "evidence_sha256": evidence_hashes,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "r3-before", "r3-after"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("r1-before", "r1-after", "r4-before", "r4-after"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    paths = {"source": args.source, "r3_before": args.r3_before, "r3_after": args.r3_after}
    for key in ("r1_before", "r1_after", "r4_before", "r4_after"):
        value = getattr(args, key)
        if value is not None:
            paths[key] = value
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
    return 0 if result["status"] in ("PASS", "OBSERVED") else 1


if __name__ == "__main__":
    raise SystemExit(main())
