#!/usr/bin/env python3
"""Capture identity-bound, read-only R1/R4 DPDK NIC counters over UART."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "danos-test/danos-robot"))
from serial_readonly import _exchange

RECEIVERS = {
    "R1": {"mac": "00:1f:7a:40:28:20", "interface": "GigabitEthernet1/0/0"},
    "R4": {"mac": "00:1f:7a:40:01:80", "interface": "GigabitEthernet1/0/0"},
}


def build_remote(role: str, phase: str, marker: str) -> str:
    if role not in RECEIVERS or phase not in ("before", "after"):
        raise ValueError("receiver role/phase is invalid")
    expected = RECEIVERS[role]
    script = f'''
import base64,json,re,subprocess,time
role={role!r}; phase={phase!r}; expected={expected!r}
result={{"status":"FAIL","role":role,"phase":phase,"scope":"read-only independent receiver NIC counters"}}
vpp=["/usr/bin/vppctl","-s","/run/vpp/cli.sock"]
def run(argv):
 p=subprocess.run(argv,capture_output=True,text=True,timeout=12)
 if p.returncode or re.search(r"unknown input|connection refused",p.stdout,re.I):
  raise RuntimeError("command failed: "+" ".join(argv)+" "+p.stdout[-500:]+p.stderr[-300:])
 return p.stdout
try:
 result["captured_unix_time"]=time.time()
 result["captured_monotonic"] = time.monotonic()
 result["pid"]=run(["systemctl","show","vpp","-p","MainPID","--value"]).strip()
 assert result["pid"] not in ("","0"),"VPP is not active"
 hw=run(vpp+["show","hardware-interfaces",expected["interface"]])
 assert re.search(r"Ethernet address\\s+"+re.escape(expected["mac"])+r"\\b",hw,re.I),"receiver NIC MAC mismatch"
 assert "carrier up full duplex" in hw and "Link speed: 1 Gbps" in hw,"receiver link is not 1 Gbps/full-duplex"
 rx=re.search(r"(?m)^\\s*rx_good_packets\\s+(\\d+)\\s*$",hw)
 tx=re.search(r"(?m)^\\s*tx_good_packets\\s+(\\d+)\\s*$",hw)
 assert rx and tx,"hardware good-packet counters unavailable"
 interfaces=run(vpp+["show","interface"])
 row=re.search(r"(?m)^"+re.escape(expected["interface"])+r"\\s+\\d+\\s+up\\b[^\\n]*",interfaces)
 assert row,"receiver VPP interface is not up"
 result.update(mac=expected["mac"],interface=expected["interface"],link="1 Gbps full duplex",
               rx_good_packets=int(rx[1]),tx_good_packets=int(tx[1]),hardware=hw,interfaces=interfaces,status="PASS")
except Exception as error:result["failure_reason"]=str(error)
print({marker!r}+base64.b64encode(json.dumps(result).encode()).decode())
'''
    compile(script, "<receiver-counter-snapshot>", "exec")
    return script


def validate_result(result: dict, role: str, phase: str) -> None:
    expected = RECEIVERS[role]
    if result.get("status") != "PASS":
        raise ValueError(result.get("failure_reason", "receiver snapshot did not pass"))
    for key, value in (("role", role), ("phase", phase), ("mac", expected["mac"]),
                       ("interface", expected["interface"])):
        if result.get(key) != value:
            raise ValueError(f"receiver snapshot {key} mismatch")
    if not result.get("pid") or result["pid"] == "0":
        raise ValueError("receiver VPP PID is invalid")
    timestamp = result.get("captured_unix_time")
    if type(timestamp) not in (int, float) or not math.isfinite(timestamp):
        raise ValueError("receiver capture timestamp is invalid")
    for key in ("rx_good_packets", "tx_good_packets"):
        if type(result.get(key)) is not int or result[key] < 0:
            raise ValueError(f"receiver {key} is invalid")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--role", choices=tuple(RECEIVERS), required=True)
    parser.add_argument("--phase", choices=("before", "after"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    marker = "DANOS_RECEIVER_COUNTER_" + uuid.uuid4().hex + ":"
    try:
        host_capture_started = time.monotonic()
        result = _exchange(args.device, build_remote(args.role, args.phase, marker),
                           marker, reply_timeout=30)
        host_capture_finished = time.monotonic()
        validate_result(result, args.role, args.phase)
        result["host_capture_started_monotonic"] = host_capture_started
        result["host_capture_finished_monotonic"] = host_capture_finished
    except (OSError, ValueError, TimeoutError) as error:
        result = {"status": "FAIL", "role": args.role, "phase": args.phase,
                  "scope": "read-only independent receiver NIC counters",
                  "failure_reason": str(error)}
    payload = json.dumps(result, indent=2) + "\n"
    (args.output / "result.json").write_text(payload)
    digest = hashlib.sha256(payload.encode()).hexdigest()
    print(json.dumps({"status": result.get("status"), "role": args.role,
                      "phase": args.phase, "mac": result.get("mac"),
                      "rx_good_packets": result.get("rx_good_packets"),
                      "sha256": digest, "failure_reason": result.get("failure_reason")}))
    return 0 if result.get("status") == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
