#!/usr/bin/env python3
"""Bounded real interface-rate observations; not line-rate qualification."""
import argparse
import json
import math
import re
import subprocess
import time
from pathlib import Path
from collect_vpp_cpu import parse_threads, parse_stat


def parse_interfaces(raw):
    ports, current = {}, None
    for line in raw.splitlines():
        row = re.match(r"^(\S+)\s+\d+\s+(?:up|down)\s", line)
        if row:
            current = row[1] if row[1].startswith("GigabitEthernet") else None
            if current:
                ports[current] = {}
        match = re.search(r"\b(rx packets|tx packets|rx bytes|tx bytes|drops)\s+(\d+)", line)
        if current and match:
            ports[current][match[1]] = int(match[2])
    if not ports:
        raise ValueError("no physical interface counters")
    return ports


def rate_bounds(delta, before, after):
    low_time, high_time = after[1] - before[0], after[0] - before[1]
    if delta < 0 or not all(math.isfinite(t) for t in (*before, *after)):
        raise ValueError("counter reset or invalid clock")
    if not before[0] <= before[1] < after[0] <= after[1]:
        raise ValueError("overlapping or reversed observations")
    return dict(lower=delta / low_time, upper=delta / high_time)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--duration", type=float, default=10)
    p.add_argument("--socket", default="/run/vpp/cli.sock")
    p.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    if not math.isfinite(a.duration) or not 0 < a.duration <= 3600:
        p.error("duration must be finite in (0,3600]")
    def identity():
        reply = subprocess.run(["/usr/bin/vppctl", "-s", a.socket, "show", "threads"],
                               capture_output=True, text=True, check=True, timeout=10)
        threads = parse_threads(reply.stdout)
        pid = threads[0]["lwp"]
        proc = Path("/proc") / str(pid)
        if (proc / "exe").resolve(strict=True).name != "vpp":
            raise ValueError("CLI process is not VPP")
        return dict(threads=threads, process=parse_stat((proc / "stat").read_text())[:2])
    def snapshot():
        start = time.monotonic()
        result = subprocess.run(["/usr/bin/vppctl", "-s", a.socket, "show", "interface"],
                                capture_output=True, text=True, check=True, timeout=10)
        end = time.monotonic()
        return dict(clock=[start, end], ports=parse_interfaces(result.stdout), raw=result.stdout)
    initial_identity = identity()
    first = snapshot()
    time.sleep(a.duration)
    last = snapshot()
    if identity() != initial_identity:
        raise ValueError("VPP process/thread identity changed")
    rates = {}
    if first["ports"].keys() != last["ports"].keys():
        raise ValueError("interface set changed")
    for port, counters in first["ports"].items():
        if counters.keys() != last["ports"][port].keys():
            raise ValueError("counter fields changed; refuse implicit zeros")
        rates[port] = {field: rate_bounds(last["ports"][port][field] - value,
                                        first["clock"], last["clock"])
                       for field, value in counters.items()}
    result = dict(scope="interface-counter-rate-only", performance_status="ENVIRONMENT-OPEN",
                  before=first, after=last, rates_per_second=rates,
                  identity=initial_identity, latency=None, cpu_pct=None)
    a.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
