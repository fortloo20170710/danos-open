#!/usr/bin/env python3
"""Capture identity-bound, read-only R3 interface/FIB counters over UART."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "danos-test/danos-robot"))
from serial_readonly import _exchange


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--phase", choices=("before", "after"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)

    marker = "DANOS_R3_COUNTER_SNAPSHOT_" + uuid.uuid4().hex + ":"
    script = f'''
import subprocess,json,re,time,os
result={{"status":"FAIL","phase":{args.phase!r},"scope":"read-only R3 interface counters and FIB"}}
vpp=["/usr/bin/vppctl","-s","/run/vpp/cli.sock"]
def run(argv):
    p=subprocess.run(argv,capture_output=True,text=True,timeout=12)
    if p.returncode or re.search(r"unknown input|connection refused",p.stdout,re.I):
        raise RuntimeError("command failed: "+" ".join(argv)+" "+p.stdout+p.stderr)
    return p.stdout
def cpu_sample(pid):
    fields=open('/proc/'+pid+'/stat').read().rsplit(') ',1)[1].split()
    return {{'pid':pid,'start_ticks':fields[19],
            'cpu_ticks':int(fields[11])+int(fields[12]),
            'clock_ticks_per_second':os.sysconf('SC_CLK_TCK'),
            'monotonic':time.monotonic()}}
try:
    result["captured_unix_time"] = time.time()
    result["captured_monotonic"] = time.monotonic()
    result["pid"] = run(["systemctl","show","vpp","-p","MainPID","--value"]).strip()
    assert result["pid"] not in ("","0"),"VPP is not active"
    result["vpp_cpu_sample"] = cpu_sample(result["pid"])
    for port,mac in ((1,"00:1f:7a:69:f7:4c"),(2,"00:1f:7a:69:f7:4d")):
        raw=run(vpp+["show","hardware-interfaces",f"GigabitEthernet{{port}}/0/0"])
        assert re.search(r"Ethernet address\\s+"+re.escape(mac)+r"\\b",raw,re.I),f"R3 P{{port}} MAC mismatch"
        result[f"hardware_p{{port}}"]=raw
    result["interfaces"]=run(vpp+["show","interface"])
    result["fib"]=run(vpp+["show","ip","fib","30.30.30.0/24"])
    result["status"]="PASS"
except Exception as error:
    result["failure_reason"]=str(error)
print({marker!r}+__import__("base64").b64encode(json.dumps(result).encode()).decode())
'''
    try:
        host_capture_started = time.monotonic()
        result = _exchange(args.device, script, marker, reply_timeout=30)
        host_capture_finished = time.monotonic()
        result["host_capture_started_monotonic"] = host_capture_started
        result["host_capture_finished_monotonic"] = host_capture_finished
    except (OSError, ValueError, TimeoutError) as error:
        result = {"status": "UNKNOWN", "phase": args.phase,
                  "scope": "read-only R3 interface counters and FIB",
                  "failure_reason": str(error)}
    payload = json.dumps(result, indent=2) + "\n"
    (args.output / "result.json").write_text(payload)
    digest = hashlib.sha256(payload.encode()).hexdigest()
    print(json.dumps({"status": result.get("status"), "phase": args.phase,
                      "vpp_pid": result.get("pid"), "sha256": digest,
                      "failure_reason": result.get("failure_reason")}))
    return 0 if result.get("status") == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
