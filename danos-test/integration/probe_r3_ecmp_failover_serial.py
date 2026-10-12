#!/usr/bin/env python3
"""Bounded R3 single-path withdrawal/restore probe over identity-guarded UART.

This mutates only the R3 VPP runtime route and P2 admin state. It restores both
in a remote finally block; it is not FRR/ZAPI, transit, or performance evidence.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "danos-test/danos-robot"))
from serial_readonly import _exchange

R3_P1_MAC = "00:1f:7a:69:f7:4c"
R3_P2_MAC = "00:1f:7a:69:f7:4d"
P1 = "GigabitEthernet1/0/0"
P2 = "GigabitEthernet2/0/0"
R4_GW = "10.20.0.2"
R1_GW = "10.30.0.2"
PREFIX = "30.30.30.0/24"


def resolved_paths(fib):
    blocks = re.findall(
        r"(?m)^\s*path:\[\d+\](.*?)(?=^\s*path:\[|^\s*forwarding:|\Z)",
        fib, re.S | re.M)
    paths = set()
    for block in blocks:
        if "oper-flags:resolved" not in block:
            continue
        for gateway, interface in ((R4_GW, P1), (R1_GW, P2)):
            if re.search(re.escape(gateway) + r"\s+" + re.escape(interface) + r"\b", block):
                paths.add((gateway, interface))
    return paths


def verify_fib(fib, expected):
    match = re.search(r"\bbuckets:(\d+)\b", fib)
    if not match or int(match.group(1)) != len(expected):
        raise ValueError("FIB bucket count does not match expected path state")
    paths = resolved_paths(fib)
    if paths != expected:
        raise ValueError(f"resolved FIB paths differ: {sorted(paths)}")
    forwarding = fib.split("forwarding:", 1)[-1]
    usable = set()
    for gateway, interface in ((R4_GW, P1), (R1_GW, P2)):
        if re.search(r"ipv4 via\s+" + re.escape(gateway) + r"\s+" +
                     re.escape(interface) + r"\b", forwarding):
            usable.add((gateway, interface))
    if usable != expected:
        raise ValueError(f"forwarding DPO adjacency differs: {sorted(usable)}")
    return sorted([{"gateway": gateway, "interface": interface}
                   for gateway, interface in paths], key=lambda x: x["interface"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--expected-mgrd-sha256", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9a-f]{64}", args.expected_mgrd_sha256):
        parser.error("exact installed mgrd SHA256 required")
    args.output.mkdir(parents=True, exist_ok=False)
    marker = "DANOS_R3_ECMP_FAILOVER_" + uuid.uuid4().hex + ":"
    script = f'''
import subprocess,json,hashlib,re,time
from pathlib import Path
result={{"status":"FAIL","scope":"R3 single-next-hop reachability and actual FIB/DPO withdrawal/restore; remote-prefix transit is not tested","peers":[{{"gateway":{R4_GW!r},"interface":{P1!r} }},{{"gateway":{R1_GW!r},"interface":{P2!r} }}]}}
vpp=["/usr/bin/vppctl","-s","/run/vpp/cli.sock"]
prefix={PREFIX!r}; p1={P1!r}; p2={P2!r}; gw1={R4_GW!r}; gw2={R1_GW!r}
def run(argv, timeout=15):
    try:
        p=subprocess.run(argv,capture_output=True,text=True,timeout=timeout)
        return {{"returncode":p.returncode,"stdout":p.stdout,"stderr":p.stderr}}
    except Exception as error:
        return {{"returncode":-1,"stdout":"","stderr":str(error)}}
def checked(argv, timeout=15):
    r=run(argv,timeout)
    if r["returncode"] or re.search(r"unknown input|connection refused|failed",r["stdout"],re.I):
        raise RuntimeError("command failed: "+" ".join(argv)+" "+r["stdout"]+r["stderr"])
    return r["stdout"]
def fib_paths(raw):
    blocks=re.findall(r"(?m)^\\s*path:\\[\\d+\\](.*?)(?=^\\s*path:\\[|^\\s*forwarding:|\\Z)",raw,re.S|re.M)
    paths=set()
    for block in blocks:
        if "oper-flags:resolved" not in block: continue
        for gw,iface in ((gw1,p1),(gw2,p2)):
            if re.search(re.escape(gw)+r"\\s+"+re.escape(iface)+r"\\b",block): paths.add((gw,iface))
    count=re.search(r"\\bbuckets:(\\d+)\\b",raw)
    forwarding=raw.split("forwarding:",1)[-1]
    usable=set()
    for gw,iface in ((gw1,p1),(gw2,p2)):
        if re.search(r"ipv4 via\\s+"+re.escape(gw)+r"\\s+"+re.escape(iface)+r"\\b",forwarding):
            usable.add((gw,iface))
    return int(count.group(1)) if count else -1, paths, usable
def require_fib(expected):
    raw=checked(vpp+["show","ip","fib",prefix]); count,paths,usable=fib_paths(raw)
    if count!=len(expected) or paths!=expected or usable!=expected:
        raise RuntimeError("unexpected FIB/DPO state buckets="+str(count)+" paths="+repr(sorted(paths))+" usable="+repr(sorted(usable)))
    return raw
def require_ping(target, source):
    raw=checked(vpp+["ping",target,"source",source,"repeat","5","interval","0.05"])
    if not re.search(r"^Statistics: 5 sent, 5 received, 0(?:\\.0+)?% packet loss$",raw,re.M):
        raise RuntimeError("probe failed for "+target+": "+raw)
    return raw
def warm_neighbor(target, source):
    attempts=[]
    for attempt in range(1,11):
        observed=run(vpp+["ping",target,"source",source,"repeat","5","interval","0.05"])
        attempts.append({{"attempt":attempt,"returncode":observed["returncode"],"output":observed["stdout"]}})
        if observed["returncode"]==0 and re.search(r"^Statistics: 5 sent, 5 received, 0(?:\\.0+)?% packet loss$",observed["stdout"],re.M):
            result["restore_neighbor_attempts"]=attempts
            return observed["stdout"]
        time.sleep(.25)
    result["restore_neighbor_attempts"]=attempts
    raise RuntimeError("R1 neighbor did not answer bounded ARP warm-up probes")
def carrier_up():
    raw=checked(vpp+["show","hardware-interfaces",p2])
    return "carrier up full duplex" in raw and "Link speed: 1 Gbps" in raw
def wait_recovery():
    deadline=time.monotonic()+25
    while time.monotonic()<deadline:
        if carrier_up():
            raw=checked(vpp+["show","ip","fib",prefix]); count,paths,usable=fib_paths(raw)
            if count==2 and paths=={{(gw1,p1),(gw2,p2)}} and usable=={{(gw1,p1),(gw2,p2)}}: return raw
        time.sleep(.25)
    raise RuntimeError("P2 carrier and two resolved paths did not recover within 25 seconds")
mutated=False
try:
    identity=checked(vpp+["show","hardware-interfaces",p1])
    assert re.search(r"Ethernet address\\s+"+re.escape({R3_P1_MAC!r})+r"\\b",identity,re.I),"R3 P1 identity mismatch"
    identity2=checked(vpp+["show","hardware-interfaces",p2])
    assert re.search(r"Ethernet address\\s+"+re.escape({R3_P2_MAC!r})+r"\\b",identity2,re.I),"R3 P2 identity mismatch"
    result["mgrd_sha256"]=hashlib.sha256(Path("/usr/bin/danos-mgrd").read_bytes()).hexdigest()
    assert result["mgrd_sha256"]=={args.expected_mgrd_sha256!r},"installed binary mismatch"
    result["build_info"]=Path("/etc/danos/build-info.env").read_text()
    pid=checked(["systemctl","show","vpp","-p","MainPID","--value"]).strip()
    assert pid not in ("","0"),"VPP is not active"
    result["vpp_pid"]=pid
    result["fib_before"]=require_fib({{(gw1,p1),(gw2,p2)}})
    assert carrier_up(),"P2 must have 1 Gbps carrier before test"
    result["interfaces_before"]=checked(vpp+["show","interface"])
    result["phase"]=[]
    mutated=True
    checked(vpp+["set","interface","state",p2,"down"])
    result["interface_down"]=checked(vpp+["show","interface",p2])
    if not re.search(r"^"+re.escape(p2)+r"\\s+\\d+\\s+down\\b",result["interface_down"],re.M):
        raise RuntimeError("P2 did not enter admin-down state")
    checked(vpp+["ip","route","del",prefix,"via",gw2,p2])
    result["fib_single_path"]=require_fib({{(gw1,p1)}})
    result["phase"].append({{"target":gw1,"source":p1,"output":require_ping(gw1,p1)}})
    result["single_path_status"]="PASS"
except Exception as error:
    result["failure_reason"]=str(error)
finally:
    if mutated:
        restore_errors=[]
        up=run(vpp+["set","interface","state",p2,"up"])
        if up["returncode"]: restore_errors.append("P2 admin-up failed: "+up["stderr"]+up["stdout"])
        try:
            raw=checked(vpp+["show","ip","fib",prefix]); count,paths,usable=fib_paths(raw)
            if (gw2,p2) not in paths:
                add=run(vpp+["ip","route","add",prefix,"via",gw2,p2])
                if add["returncode"]: restore_errors.append("R1 next-hop add failed: "+add["stderr"]+add["stdout"])
            carrier_deadline=time.monotonic()+25
            while time.monotonic()<carrier_deadline and not carrier_up(): time.sleep(.25)
            if not carrier_up(): raise RuntimeError("P2 carrier unavailable before adjacency warm-up")
            result["restore_neighbor_probe"]=warm_neighbor(gw2,p2)
            result["fib_restored"]=wait_recovery()
            result["restore_probes"]=[{{"target":gw1,"source":p1,"output":require_ping(gw1,p1)}},
                                      {{"target":gw2,"source":p2,"output":require_ping(gw2,p2)}}]
        except Exception as error:
            restore_errors.append(str(error))
        final_pid=run(["systemctl","show","vpp","-p","MainPID","--value"])
        result["vpp_pid_final"]=final_pid["stdout"].strip()
        result["interfaces_after"]=run(vpp+["show","interface"])["stdout"]
        result["restore_errors"]=restore_errors
        if final_pid["returncode"] or result["vpp_pid_final"]!=result.get("vpp_pid"):
            result["restore_errors"].append("VPP process identity changed during probe")
        if not result["restore_errors"]:
            result["status"]="PASS" if result.get("single_path_status")=="PASS" else "FAIL"
try:
    result["p2_carrier_final"]=carrier_up() if result.get("vpp_pid_final")==result.get("vpp_pid") else False
except Exception as error:
    result["p2_carrier_final"]=False
    result.setdefault("restore_errors",[]).append("final carrier check failed: "+str(error))
    result["status"]="FAIL"
print({marker!r}+__import__("base64").b64encode(json.dumps(result).encode()).decode())
'''
    compile(script, "<remote-r3-ecmp-failover>", "exec")
    try:
        result = _exchange(args.device, script, marker, reply_timeout=60)
    except (OSError, ValueError, TimeoutError) as error:
        result = {"status": "UNKNOWN", "scope": "R3 single-next-hop reachability and actual FIB/DPO withdrawal/restore; remote-prefix transit is not tested",
                  "failure_reason": str(error), "restore_state": "unconfirmed; inspect R3 console before further testing"}
    raw = json.dumps(result, indent=2) + "\n"
    (args.output / "result.json").write_text(raw)
    digest = hashlib.sha256(raw.encode()).hexdigest()
    print(json.dumps({"status": result.get("status"), "single_path_status": result.get("single_path_status"),
                      "p2_carrier_final": result.get("p2_carrier_final"),
                      "restore_errors": result.get("restore_errors"), "sha256": digest,
                      "failure_reason": result.get("failure_reason")}))
    return 0 if result.get("status") == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
