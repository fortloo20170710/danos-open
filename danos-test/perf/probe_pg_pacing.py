#!/usr/bin/env python3
"""Small finite source pacing diagnostic, never a throughput qualification."""
import argparse
import json
import re
import subprocess
import time
from pathlib import Path
from render_physical_pg import render
from collect_vpp_cpu import parse_threads, parse_stat


def stream_counts(raw, names):
    rows = {}
    for line in raw.splitlines():
        m = re.match(r"^\s*(\S+)\s+(Yes|No)\s+(\d+)\s+limit", line)
        if m and m[1] in names:
            rows[m[1]] = dict(enabled=m[2] == "Yes", count=int(m[3]))
    if set(rows) != set(names):
        raise ValueError("missing source stream rows")
    return rows


def check_pacing(rows, names, enable_times, sample_end, rate, limit):
    """Conservative diagnostic guard, not a wire-rate conformance test."""
    for i, name in enumerate(names):
        count = rows[name]["count"]
        elapsed = sample_end - enable_times[i][0]
        if elapsed < 0 or count > limit:
            raise ValueError("invalid source count or sample clock")
        # Allow 100 ms of scheduling jitter plus 32 frames; never authorize
        # unlimited traffic or describe this margin as measured capacity.
        budget = rate * (elapsed + .1) + 32
        if count > budget:
            raise ValueError(f"source pacing ahead of monotonic budget: {name}")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ("source-mac", "target-mac", "source-ip", "interface"):
        p.add_argument("--" + key, required=True)
    p.add_argument("--target", action="append", required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--count", type=int, default=256,
                   help="finite frames per flow per phase (256..4096)")
    a = p.parse_args()
    if not 256 <= a.count <= 4096:
        p.error("count must be between 256 and 4096")
    # Validate everything before contacting the runtime; cap all phases.
    definitions = [(rate, render(a.interface, a.source_mac, a.target_mac,
                                a.source_ip, a.target, rate, a.count))
                   for rate in (100, 500, 1000)]
    if len(a.target) != 4:
        p.error("exactly four isolated destination flows required")
    def cli(command):
        reply = subprocess.run(["/usr/bin/vppctl", "-s", "/run/vpp/cli.sock", command],
                               capture_output=True, text=True, timeout=5, check=True)
        if re.search(r"unknown input|failed|expected stream", reply.stdout, re.I):
            raise ValueError(reply.stdout)
        return reply.stdout
    result = dict(scope="bounded-source-pacing-only", performance_status="ENVIRONMENT-OPEN",
                  phases=[], maximum_generated_frames=12 * a.count)
    owned = []
    def identity():
        threads = parse_threads(cli("show threads"))
        pid = threads[0]["lwp"]
        proc = Path("/proc") / str(pid)
        if (proc / "exe").resolve(strict=True).name != "vpp":
            raise ValueError("runtime executable is not VPP")
        stat = parse_stat((proc / "stat").read_text())
        return dict(threads=threads, process=list(stat[:2]))
    def clock_snapshot():
        start = time.monotonic()
        raw = cli("show clock")
        return dict(monotonic=[start, time.monotonic()], raw=raw)
    try:
        result["hardware_before"] = cli("show hardware-interfaces " + a.interface)
        if a.source_mac.lower() not in result["hardware_before"].lower():
            raise ValueError("source identity mismatch")
        if "no streams currently defined" not in cli("show packet-generator"):
            raise ValueError("existing PG streams; refusing mutation")
        result["identity_before"] = identity()
        for rate, definition in definitions:
            phase = dict(requested_pps_per_flow=rate, samples=[], enable_times=[],
                         errors_before=cli("show errors"), clock_before=clock_snapshot())
            result["phases"].append(phase)
            names = [f"pace{rate}_{i}" for i in range(4)]
            for i, line in enumerate(definition.splitlines()):
                owned.append(names[i])
                cli(line.replace(f"danos_physical_udp_{i}", names[i]))
            stream_counts(cli("show packet-generator"), names)
            for name in names:
                start = time.monotonic()
                cli("packet-generator enable-stream " + name)
                phase["enable_times"].append([start, time.monotonic()])
            deadline = time.monotonic() + a.count / rate + 5
            while time.monotonic() < deadline:
                start = time.monotonic()
                raw = cli("show packet-generator")
                rows = stream_counts(raw, names)
                phase["samples"].append(dict(clock=[start, time.monotonic()], rows=rows, raw=raw))
                check_pacing(rows, names, phase["enable_times"],
                             phase["samples"][-1]["clock"][1], rate, a.count)
                if all(not r["enabled"] and r["count"] == a.count for r in rows.values()):
                    break
                time.sleep(.1)
            else:
                raise ValueError("bounded phase deadline exceeded")
            phase["errors_after"] = cli("show errors")
            phase["hardware_after"] = cli("show hardware-interfaces " + a.interface)
            phase["clock_after"] = clock_snapshot()
            phase["identity_after"] = identity()
            if phase["identity_after"] != result["identity_before"]:
                raise ValueError("VPP process or thread mapping changed")
            def tx_errors(text):
                return sum(int(n) for n in re.findall(
                    r"(?m)^\s*(\d+)\s+\S+-tx\s+Tx packet drops", text))
            if tx_errors(phase["errors_after"]) > tx_errors(phase["errors_before"]):
                raise ValueError("new source TX drops; refusing next pacing phase")
            for name in list(owned):
                cli("packet-generator disable-stream " + name)
                cli("packet-generator delete " + name)
                owned.remove(name)
        result["status"] = "OBSERVED"
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        result.update(status="FAIL", failure_reason=str(exc))
    finally:
        failures = []
        if owned and "identity_before" in result:
            try:
                if identity() != result["identity_before"]:
                    failures.append("runtime changed; refusing cleanup in replacement VPP")
                    owned = []
            except (OSError, ValueError, subprocess.SubprocessError) as exc:
                failures.append("cleanup identity unavailable: " + str(exc))
                owned = []
        for name in owned:
            try:
                cli("packet-generator disable-stream " + name)
                cli("packet-generator delete " + name)
            except (OSError, ValueError, subprocess.SubprocessError) as exc:
                failures.append(str(exc))
        if failures:
            result.update(status="FAIL", cleanup_errors=failures)
        a.out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(dict(status=result["status"], phases=len(result["phases"]),
                          scope=result["scope"], performance_status="ENVIRONMENT-OPEN")))
    return 1 if result["status"] == "FAIL" else 0


if __name__ == "__main__":
    raise SystemExit(main())
