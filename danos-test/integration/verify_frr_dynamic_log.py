#!/usr/bin/env python3
"""Validate bridge event evidence, not packet or independent FIB acceptance."""
import re
import sys
from pathlib import Path


def validate(log: str) -> None:
    commands = set(re.findall(r"zapi command=(\d+)\b", log))
    if not commands.intersection({"9", "31"}):
        raise ValueError("no route add observed")
    if not commands.intersection({"10", "32"}):
        raise ValueError("no route delete observed")
    sweeps = re.findall(r"programming sweep: withdrawn=(\d+) failed=(\d+)", log)
    if any(int(failed) for _, failed in sweeps):
        raise ValueError("withdrawal failed")
    if not any(int(count) > 0 for count, _ in sweeps):
        raise ValueError("no positive withdrawal observed")
    summaries = re.findall(r"fib_live_bridge: processed=(\d+) failed=(\d+)", log)
    if len(summaries) != 1 or int(summaries[0][0]) < 2 or int(summaries[0][1]):
        raise ValueError("missing or unsuccessful bridge summary")
    if re.search(r"ZAPI command \d+ (?:transaction|programming) failed:", log):
        raise ValueError("route processing failure observed")


if __name__ == "__main__":
    try:
        validate(Path(sys.argv[1]).read_text())
    except (ValueError, OSError, IndexError) as error:
        print(f"[FAIL] dynamic event evidence: {error}", file=sys.stderr)
        sys.exit(1)
    print("[OBSERVED] route add/delete and positive successful sweep")
