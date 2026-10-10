#!/usr/bin/env python3
"""Collect real VPP process CPU usage; this alone is not traffic qualification."""
import argparse
import json
import math
import os
from pathlib import Path
import time


def parse_stat(raw):
    # comm may contain spaces and parentheses; numeric fields follow its last ')'.
    pid = int(raw.partition(" (")[0])
    fields = raw.rsplit(") ", 1)[1].split()
    return pid, int(fields[19]), int(fields[11]) + int(fields[12])


def cpu_percent(first, last, elapsed, hz):
    if first[:2] != last[:2]:
        raise ValueError("process identity changed during sampling")
    if not math.isfinite(elapsed) or elapsed <= 0 or hz <= 0:
        raise ValueError("invalid sampling clock")
    if last[2] < first[2]:
        raise ValueError("CPU ticks moved backwards")
    # 100% is one logical CPU, not a percentage of all host CPUs.
    return (last[2] - first[2]) * 100 / hz / elapsed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--duration", type=float, default=10)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.pid <= 0 or not math.isfinite(args.duration) or not 0 < args.duration <= 3600:
        parser.error("positive PID and finite duration in (0,3600] required")
    try:
        proc = Path("/proc") / str(args.pid)
        executable = (proc / "exe").resolve(strict=True)
        if executable.name != "vpp":
            raise ValueError("selected process is not the VPP executable")
        first = parse_stat((proc / "stat").read_text())
        hz = os.sysconf("SC_CLK_TCK")
        before = time.monotonic()
        time.sleep(args.duration)
        last = parse_stat((proc / "stat").read_text())
        after = time.monotonic()
        result = dict(status="PASS", scope="cpu-sample-only", pid=args.pid,
                      process_start_ticks=first[1], executable=str(executable),
                      ticks_start=first[2], ticks_end=last[2], clock_ticks_per_second=hz,
                      monotonic_start=before, monotonic_end=after,
                      duration_ms=(after-before)*1000,
                      cpu_pct=cpu_percent(first, last, after-before, hz),
                      cpu_basis="one-logical-cpu", performance_status="ENVIRONMENT-OPEN")
    except (OSError, ValueError, IndexError) as exc:
        result = dict(status="FAIL", scope="cpu-sample-only", failure_reason=str(exc),
                      performance_status="ENVIRONMENT-OPEN")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
