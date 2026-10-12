#!/usr/bin/env python3
"""Verify a bounded R2 probe batch against identity-bound R3 counters."""
import argparse
import hashlib
import json
from pathlib import Path
import re

P1 = "GigabitEthernet1/0/0"
P2 = "GigabitEthernet2/0/0"
P4 = "GigabitEthernet4/0/0"
PATHS = (("10.20.0.2", P1), ("10.30.0.2", P2))
TARGETS = tuple(f"30.30.30.{n}" for n in range(2, 6))


def parse_interfaces(raw):
    interfaces = {}
    current = None
    for line in raw.splitlines():
        line = line.strip()
        match = re.match(r"(GigabitEthernet\d+/\d+/\d+)\s+\d+\s+(?:up|down)\b", line)
        if match:
            current = match.group(1)
            interfaces[current] = {}
        match = re.search(r"(?:^|\s)(rx packets|tx packets|rx bytes|tx bytes|drops)\s+(\d+)$", line)
        if match and current:
            interfaces[current][match.group(1)] = int(match.group(2))
    return interfaces


def parse_fib(raw):
    if not re.search(r"\bbuckets:2\b", raw):
        raise ValueError("FIB does not have exactly two ECMP buckets")
    paths = set()
    blocks = re.findall(r"(?m)^\s*path:\[\d+\](.*?)(?=^\s*path:\[|^\s*forwarding:|\Z)", raw, re.S | re.M)
    for gateway, interface in PATHS:
        if any("oper-flags:resolved" in block and
               re.search(re.escape(gateway) + r"\s+" + re.escape(interface) + r"\b", block)
               for block in blocks):
            if re.search(r"ipv4 via\s+" + re.escape(gateway) + r"\s+" + re.escape(interface) + r"\b",
                         raw.split("forwarding:", 1)[-1]):
                paths.add((gateway, interface))
    if paths != set(PATHS):
        raise ValueError(f"FIB paths are not both resolved forwarding adjacencies: {sorted(paths)}")
    return paths


def verify(before, after, traffic):
    for snapshot, phase in ((before, "before"), (after, "after")):
        if snapshot.get("status") != "PASS" or snapshot.get("phase") != phase:
            raise ValueError(f"R3 {phase} snapshot did not pass")
        if snapshot.get("scope") != "read-only R3 interface counters and FIB":
            raise ValueError("unexpected R3 snapshot scope")
        if not re.fullmatch(r"[1-9]\d*", str(snapshot.get("pid", ""))):
            raise ValueError("R3 VPP PID is missing")
        parse_fib(snapshot.get("fib", ""))
    if before["pid"] != after["pid"]:
        raise ValueError("R3 VPP process changed during the counter window")

    for snapshot in (before, after):
        for port, mac in ((1, "00:1f:7a:69:f7:4c"), (2, "00:1f:7a:69:f7:4d")):
            hardware = snapshot.get(f"hardware_p{port}", "")
            if not re.search(r"Ethernet address\s+" + re.escape(mac) + r"\b", hardware, re.I):
                raise ValueError(f"R3 P{port} hardware identity mismatch")

    if traffic.get("status") != "PASS" or traffic.get("role") != "R2" or \
            traffic.get("expected_mac", "").lower() != "00:1f:7a:69:f5:ec":
        raise ValueError("R2 probe identity/status is not qualified")
    if not (traffic.get("pid_before") == traffic.get("pid_after") == traffic.get("pid_final") != "0"):
        raise ValueError("R2 VPP process changed during probe window")
    measured = traffic.get("verified_warm", [])
    if [p.get("target") for p in measured] != list(TARGETS) or any(
            p.get("status") != "PASS" or p.get("sent") != 100 or
            p.get("received") != 100 or p.get("loss_percent") != 0 or p.get("reply_ttl") != 63
            for p in measured):
        raise ValueError("R2 measured probes do not contain four lossless 100-packet targets")
    probes = traffic.get("probes", [])
    total_sent = 0
    warmups = [p for p in probes if p.get("phase") == "warmup"]
    if len(warmups) != 4 or any(p.get("attempt") != 1 for p in warmups):
        raise ValueError("expected exactly one successful 5-packet readiness probe per target")
    for probe in probes:
        match = re.search(r"Statistics: (\d+) sent, (\d+) received, (\d+(?:\.\d+)?)% packet loss",
                          probe.get("output", ""))
        if not match or int(match.group(1)) != int(match.group(2)) or float(match.group(3)) != 0:
            raise ValueError("a probe batch was not lossless or lacks summary counters")
        total_sent += int(match.group(1))
    if total_sent != 420:
        raise ValueError(f"expected 420 requests including warmups, got {total_sent}")

    old, new = parse_interfaces(before.get("interfaces", "")), parse_interfaces(after.get("interfaces", ""))
    deltas = {}
    for interface in (P1, P2, P4):
        if interface not in old or interface not in new:
            raise ValueError(f"missing interface counter table for {interface}")
        deltas[interface] = {
            key: new[interface].get(key, 0) - old[interface].get(key, 0)
            for key in ("rx packets", "tx packets", "rx bytes", "tx bytes", "drops")
        }
        if deltas[interface]["drops"] != 0:
            raise ValueError(f"drop counter increased on {interface}")
    ingress = deltas[P4]
    if ingress["rx packets"] != total_sent or ingress["tx packets"] != total_sent:
        raise ValueError("R3 P4 ingress counters do not match the 420 sent requests")
    if deltas[P1]["tx packets"] <= 0 or deltas[P2]["tx packets"] <= 0 or \
            deltas[P1]["tx packets"] + deltas[P2]["tx packets"] != total_sent:
        raise ValueError("both ECMP egress counters must be nonzero and sum to ingress")
    return {"status": "PASS", "scope": "bounded four-destination physical ECMP path-use correlation; not balance/performance",
            "vpp_pid": before["pid"], "requests": total_sent,
            "targets": list(TARGETS), "deltas": deltas,
            "egress_split": {"P1": deltas[P1]["tx packets"], "P2": deltas[P2]["tx packets"]}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--traffic", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = {"status": "FAIL"}
    try:
        paths = (args.before, args.after, args.traffic)
        docs = [json.loads(path.read_text()) for path in paths]
        result = verify(*docs)
        result["evidence_sha256"] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                    for path in paths}
    except (OSError, ValueError, json.JSONDecodeError) as error:
        result["failure_reason"] = str(error)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
