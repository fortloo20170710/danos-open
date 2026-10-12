#!/usr/bin/env python3
"""Bounded ICMP checks for the explicitly identified R1/R4 lab peer."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import sys
import uuid

from physical_lab_profile import render
from verify_physical_ping_log import verify

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "danos-test/danos-robot"))
from serial_readonly import _exchange


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--role", required=True, choices=("R1", "R4"))
    parser.add_argument("--expected-mac", required=True)
    parser.add_argument("--restart-vpp", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", args.expected_mac):
        parser.error("exact MAC required")
    args.output.mkdir(parents=True, exist_ok=False)
    targets = ["10.30.0.1" if args.role == "R1" else "10.20.0.1", "10.10.0.2"]
    profile_sha = hashlib.sha256(render(args.role).encode()).hexdigest()
    marker = "DANOS_PEER_PROBE_" + uuid.uuid4().hex + ":"
    script = f'''
import subprocess,json,base64,re,hashlib,time
from pathlib import Path
result = dict(status="FAIL",probes=[],restart_requested={args.restart_vpp!r})
def command(argv,limit=15):
    r = subprocess.run(argv,capture_output=True,text=True,timeout=limit)
    assert r.returncode == 0, (argv,r.stdout,r.stderr)
    assert "unknown input" not in r.stdout.lower(), r.stdout
    return r.stdout
vpp = ["/usr/bin/vppctl","-s","/run/vpp/cli.sock"]
try:
    result["identity"] = command(vpp+["show","hardware-interfaces","GigabitEthernet1/0/0"])
    assert re.search(r"Ethernet address\\s+"+re.escape({args.expected_mac!r})+r"\\b",result["identity"],re.I), "MAC mismatch"
    result["pid_before"] = command(["systemctl","show","vpp","-p","MainPID","--value"]).strip()
    assert result["pid_before"] not in ("", "0")
    if {args.restart_vpp!r}:
        assert hashlib.sha256(Path("/etc/danos/physical-lab.cli").read_bytes()).hexdigest() == {profile_sha!r}, "profile mismatch"
        command(["systemctl","restart","vpp"],30)
    result["pid_after"] = command(["systemctl","show","vpp","-p","MainPID","--value"]).strip()
    assert result["pid_after"] not in ("", "0")
    assert (result["pid_before"] != result["pid_after"]) == {args.restart_vpp!r}
    start = time.monotonic()
    link_samples = []
    while True:
        hardware = command(vpp+["show","hardware-interfaces","GigabitEthernet1/0/0"])
        ready = re.search(r"carrier up full duplex\\b", hardware) is not None and "Link speed: 1 Gbps" in hardware
        link_samples.append(dict(elapsed_seconds=time.monotonic()-start,ready=ready))
        result["hardware"] = hardware
        result["link_samples"] = link_samples
        if ready:
            break
        assert time.monotonic()-start < 20, "peer link did not reach 1Gbps full duplex"
        time.sleep(.25)
    result["link_ready_seconds"] = time.monotonic()-start
    result["fib_before"] = command(vpp+["show","ip","fib"])
    result["interfaces_before"] = command(vpp+["show","interface"])
    for target in {targets!r}:
        ready_start = time.monotonic()
        for attempt in range(1,11):
            cold = command(vpp+["ping",target,"repeat","5","interval","0.05"])
            result["probes"].append(dict(target=target,phase="warmup",attempt=attempt,output=cold,
                                          elapsed_seconds=time.monotonic()-ready_start))
            if re.search(r"^Statistics: 5 sent, 5 received, 0(?:\\.0+)?% packet loss$",cold,re.M):
                break
            assert attempt < 10 and time.monotonic()-ready_start < 12, "neighbor/path readiness deadline exceeded"
            time.sleep(.25)
        start = time.monotonic()
        warm = command(vpp+["ping",target,"repeat","100","interval","0.01"])
        result["probes"].append(dict(target=target,phase="warm",output=warm,duration_seconds=time.monotonic()-start))
    result["interfaces_after"] = command(vpp+["show","interface"])
    result["pid_final"] = command(["systemctl","show","vpp","-p","MainPID","--value"]).strip()
    assert result["pid_final"] == result["pid_after"], "VPP process changed during probes"
    result["status"] = "CAPTURED"
except Exception as error:
    result["failure_reason"] = str(error)
print({marker!r}+base64.b64encode(json.dumps(result).encode()).decode())
'''
    try:
        result = _exchange(args.device, script, marker, reply_timeout=60)
    except (OSError, ValueError, TimeoutError) as error:
        result = dict(status="UNKNOWN", failure_reason=str(error), probes=[])
    result.update(role=args.role, expected_mac=args.expected_mac,
                  scope="bounded peer ICMP and optional lab CLI service replay; not ECMP distribution, mgrd WAL or performance")
    # Preserve every completed warmup/warm log, including failed captures.
    for probe in result.get("probes", []):
        raw = probe["output"].encode()
        name = probe["target"] + "-" + probe["phase"] + ("-" + str(probe["attempt"]) if "attempt" in probe else "") + ".log"
        (args.output / name).write_bytes(raw)
        probe["sha256"] = hashlib.sha256(raw).hexdigest()
    if result["status"] == "CAPTURED":
        try:
            warm = [p for p in result["probes"] if p["phase"] == "warm"]
            assert [p["target"] for p in warm] == targets
            result["verified_warm"] = [verify(p["output"].encode(), p["target"], 100,
                                                    64 if p["target"] == targets[0] else 63) for p in warm]
            result["status"] = "PASS"
        except (ValueError, AssertionError) as error:
            result.update(status="FAIL", failure_reason=str(error))
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k not in ("identity", "hardware", "fib_before", "interfaces_before", "interfaces_after", "probes")}))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
