#!/usr/bin/env python3
"""Check an API-installed learned route's resolved VPP FIB, not packet acceptance."""
import argparse
import re
import sys
from pathlib import Path


def validate(log: str, prefix: str, gateway: str, interface: str) -> None:
    headers = list(re.finditer(rf"(?m)^{re.escape(prefix)} fib:\d+ index:\d+", log))
    if not headers:
        raise ValueError("no detailed FIB entry for learned prefix")
    block = log[headers[-1].start():]
    entries = list(re.finditer(r"(?m)^\d+\.\d+\.\d+\.\d+/\d+ fib:", block))
    if len(entries) > 1:
        block = block[:entries[1].start()]
    forwarding = block.partition("forwarding:")[2]
    if not forwarding:
        raise ValueError("no forwarding chain in learned route FIB")
    if not re.search(r"\bAPI refs:[1-9][0-9]*\b", block):
        raise ValueError("learned route is not API-owned")
    if re.search(r"arp-ipv4|dpo-drop|ip4-drop", forwarding):
        raise ValueError("learned route forwarding is unresolved or drop")
    if not re.search(rf"ipv4 via {re.escape(gateway)} {re.escape(interface)}:", forwarding):
        raise ValueError("no resolved forwarding adjacency for expected learned next hop")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fib-log", required=True, type=Path)
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--gateway", required=True)
    parser.add_argument("--interface", required=True)
    args = parser.parse_args()
    try:
        validate(args.fib_log.read_text(errors="replace"), args.prefix,
                 args.gateway, args.interface)
    except (OSError, ValueError) as error:
        print(f"[FAIL] learned-route FIB: {error}", file=sys.stderr)
        sys.exit(1)
    print("[PASS] resolved API-owned learned-route FIB (packet proof still required)")
