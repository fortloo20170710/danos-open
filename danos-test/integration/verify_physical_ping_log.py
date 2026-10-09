#!/usr/bin/env python3
"""Verify endpoint ICMP evidence only; never grant PCI performance/ECMP PASS."""
import argparse
import hashlib
import ipaddress
import json
from pathlib import Path
import re
import sys


def verify(raw, target, expected, ttl):
    ipaddress.IPv4Address(target)
    if expected < 1 or not 1 <= ttl <= 255:
        raise ValueError("positive packet count and valid TTL required")
    text = raw.decode("utf-8", errors="strict").replace("\r", "")
    summaries = re.findall(r"^Statistics: (\d+) sent, (\d+) received, ([\d.]+)% packet loss$", text, re.M)
    if len(summaries) != 1:
        raise ValueError("exactly one completed VPP ping summary required")
    sent, received, loss = summaries[0]
    if int(sent) != expected or int(received) != expected or float(loss) != 0:
        raise ValueError("summary does not meet the expected zero-loss packet count")
    if re.search(r"unreachable|timed out|timeout|FAIL", text, re.I):
        raise ValueError("failure text present")
    replies = re.findall(r"^\d+ bytes from ([\d.]+): icmp_seq=(\d+) ttl=(\d+) time=([\d.]+) ms$", text, re.M)
    reply_lines = re.findall(r"^.*bytes from.*$", text, re.M)
    if len(replies) != expected or len(reply_lines) != expected:
        raise ValueError("reply lines disagree with expected count")
    seen = set()
    for address, sequence, observed_ttl, latency in replies:
        seq = int(sequence)
        if address != target or int(observed_ttl) != ttl:
            raise ValueError("unexpected reply address or TTL")
        if not 1 <= seq <= expected or seq in seen:
            raise ValueError("duplicate or out-of-range sequence")
        seen.add(seq)
        if float(latency) < 0:
            raise ValueError("negative latency")
    return {"schema_version": 1, "scope": "physical-endpoint-icmp-log",
            "status": "PASS", "target": target, "sent": expected,
            "received": expected, "loss_percent": 0, "reply_ttl": ttl,
            "log_sha256": hashlib.sha256(raw).hexdigest(),
            "identity": "NOT_VERIFIED", "elapsed_time": "NOT_RECORDED",
            "ecmp": "NOT_TESTED", "line_rate": "NOT_TESTED",
            "restart_recovery": "NOT_TESTED"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--target", required=True)
    parser.add_argument("--packets", required=True, type=int)
    parser.add_argument("--ttl", type=int, default=63)
    args = parser.parse_args()
    try:
        print(json.dumps(verify(args.log.read_bytes(), args.target, args.packets, args.ttl), sort_keys=True))
    except (ValueError, OSError) as error:
        print(f"ICMP endpoint evidence FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
