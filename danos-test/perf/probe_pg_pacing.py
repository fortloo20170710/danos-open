#!/usr/bin/env python3
"""Small finite source pacing diagnostic, never a throughput qualification."""
import argparse
import json
import re
import subprocess
import time
from pathlib import Path
from render_physical_pg import render


def stream_counts(raw, names):
    rows = {}
    for line in raw.splitlines():
        m = re.match(r"^\s*(\S+)\s+(Yes|No)\s+(\d+)\s+limit", line)
        if m and m[1] in names:
            rows[m[1]] = dict(enabled=m[2] == "Yes", count=int(m[3]))
    if set(rows) != set(names):
        raise ValueError("missing source stream rows")
    return rows


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ("source-mac", "target-mac", "source-ip", "interface"):
        p.add_argument("--" + key, required=True)
    p.add_argument("--target", action="append", required=True)
    p.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    # Validate everything before contacting the runtime; cap all phases.
    definitions = [(rate, render(a.interface, a.source_mac, a.target_mac,
                                a.source_ip, a.target, rate, 256))
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
                  phases=[], maximum_generated_frames=3072)
    owned = []
    try:
        result["hardware_before"] = cli("show hardware-interfaces " + a.interface)
        if a.source_mac.lower() not in result["hardware_before"].lower():
            raise ValueError("source identity mismatch")
        if "no streams currently defined" not in cli("show packet-generator"):
            raise ValueError("existing PG streams; refusing mutation")
        for rate, definition in definitions:
            phase = dict(requested_pps_per_flow=rate, samples=[], enable_times=[],
                         errors_before=cli("show errors"))
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
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                start = time.monotonic()
                raw = cli("show packet-generator")
                rows = stream_counts(raw, names)
                phase["samples"].append(dict(clock=[start, time.monotonic()], rows=rows, raw=raw))
                if all(not r["enabled"] and r["count"] == 256 for r in rows.values()):
                    break
                time.sleep(.1)
            else:
                raise ValueError("bounded phase deadline exceeded")
            phase["errors_after"] = cli("show errors")
            phase["hardware_after"] = cli("show hardware-interfaces " + a.interface)
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
