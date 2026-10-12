#!/usr/bin/env python3
"""Restart R3 VPP once and verify lab-profile and both peer adjacencies replay."""
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
    parser.add_argument("--expected-mac", required=True)
    parser.add_argument("--expected-mgrd-sha256", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", args.expected_mac):
        parser.error("exact R3 P1 MAC required")
    if not re.fullmatch(r"[0-9a-f]{64}", args.expected_mgrd_sha256):
        parser.error("exact installed mgrd SHA256 required")
    args.output.mkdir(parents=True, exist_ok=False)

    profile = render("R3")
    profile_sha = hashlib.sha256(profile.encode()).hexdigest()
    hook_bytes = (ROOT / "danos-test/installer/replay_physical_lab.sh").read_bytes()
    hook_sha = hashlib.sha256(hook_bytes).hexdigest()
    marker = "DANOS_R3_RESTART_" + uuid.uuid4().hex + ":"
    script = f'''
import subprocess,json,base64,hashlib,re,time
from pathlib import Path
result=dict(status="FAIL",scope="R3 VPP service restart and lab CLI replay; no mgrd WAL or lossless claim")
vpp=["/usr/bin/vppctl","-s","/run/vpp/cli.sock"]
def command(argv,timeout=15):
    r=subprocess.run(argv,capture_output=True,text=True,timeout=timeout)
    assert r.returncode==0,(argv,r.stdout,r.stderr)
    return r.stdout
try:
    ident=command(vpp+["show","hardware-interfaces","GigabitEthernet1/0/0"])
    result["identity"]=ident
    assert re.search(r"Ethernet address\\s+"+re.escape({args.expected_mac!r})+r"\\b",ident,re.I),"R3 MAC mismatch"
    assert hashlib.sha256(Path("/usr/bin/danos-mgrd").read_bytes()).hexdigest()=={args.expected_mgrd_sha256!r},"mgrd binary mismatch"
    profile=Path("/etc/danos/physical-lab.cli")
    assert hashlib.sha256(profile.read_bytes()).hexdigest()=={profile_sha!r},"R3 lab profile mismatch"
    hook=Path("/usr/libexec/danos/replay-physical-lab")
    assert hashlib.sha256(hook.read_bytes()).hexdigest()=={hook_sha!r},"replay helper mismatch"
    assert str(hook) in command(["systemctl","show","vpp","-p","ExecStartPost","--value"]),"VPP replay hook absent"
    result["pid_before"]=command(["systemctl","show","vpp","-p","MainPID","--value"]).strip()
    result["fib_before"]=command(vpp+["show","ip","fib","30.30.30.0/24"])
    result["restart_requested_unix"]=time.time()
    command(["systemctl","restart","vpp"],45)
    result["restart_command_completed_unix"]=time.time()
    result["pid_after"]=command(["systemctl","show","vpp","-p","MainPID","--value"]).strip()
    assert result["pid_before"] not in ("","0") and result["pid_after"] not in ("","0") and result["pid_after"]!=result["pid_before"],"VPP PID did not change"
    deadline=time.monotonic()+25
    while True:
        result["interfaces_after"]=command(vpp+["show","interface"])
        result["fib_after"]=command(vpp+["show","ip","fib","30.30.30.0/24"])
        if all(re.search(r"(?m)^GigabitEthernet"+str(p)+r"/0/0\\s+\\d+\\s+up\\b",result["interfaces_after"]) for p in (1,2,4)) and "buckets:2" in result["fib_after"] and result["fib_after"].count("oper-flags:resolved")>=2:
            break
        assert time.monotonic()<deadline,"lab profile or ECMP route failed to replay"
        time.sleep(.25)
    result["hardware_after"]={{}}
    for port in (1,2,4):
        hardware=command(vpp+["show","hardware-interfaces",f"GigabitEthernet{{port}}/0/0"])
        result["hardware_after"][str(port)]=hardware
        assert "carrier up full duplex" in hardware and "Link speed: 1 Gbps" in hardware,f"port {{port}} link not ready"
    result["probes"]=[]
    for target in ("10.20.0.2","10.30.0.2"):
        ready=False
        for attempt in range(1,11):
            output=command(vpp+["ping",target,"repeat","5","interval","0.05"])
            result["probes"].append(dict(target=target,phase="warmup",attempt=attempt,output=output))
            if re.search(r"^Statistics: 5 sent, 5 received, 0(?:\\.0+)?% packet loss$",output,re.M):
                ready=True
                break
            time.sleep(.25)
        assert ready,f"peer {{target}} did not converge"
        output=command(vpp+["ping",target,"repeat","100","interval","0.01"])
        result["probes"].append(dict(target=target,phase="warm",output=output))
    result["pid_final"]=command(["systemctl","show","vpp","-p","MainPID","--value"]).strip()
    assert result["pid_final"]==result["pid_after"],"VPP process changed during probes"
    result["status"]="PASS"
except Exception as error:
    result["failure_reason"]=str(error)
print({marker!r}+base64.b64encode(json.dumps(result).encode()).decode())
'''
    try:
        result = _exchange(args.device, script, marker, reply_timeout=60)
    except (OSError, ValueError, TimeoutError) as error:
        result = dict(status="UNKNOWN", failure_reason=str(error), scope="UART result unavailable; inspect R3 before retry")
    result.update(role="R3", expected_mac=args.expected_mac,
                  profile_sha256=profile_sha, replay_helper_sha256=hook_sha)
    for probe in result.get("probes", []):
        raw = probe["output"].encode()
        name = probe["target"] + "-" + probe["phase"] + ("-" + str(probe["attempt"]) if "attempt" in probe else "") + ".log"
        (args.output / name).write_bytes(raw)
        probe["sha256"] = hashlib.sha256(raw).hexdigest()
        if probe["phase"] == "warm":
            try:
                probe["verified"] = verify(raw, probe["target"], 100, 64)
            except (ValueError, AssertionError) as error:
                result.update(status="FAIL", failure_reason=str(error))
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k not in ("identity", "fib_before", "fib_after", "interfaces_after", "hardware_after", "probes")}))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
