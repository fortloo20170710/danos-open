#!/usr/bin/env python3
"""Start/collect a bounded background R2→R3 transit watch over UART."""
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

R2_MAC = "00:1f:7a:69:f5:ec"


def remote_script(action, token, duration):
    marker = "DANOS_R2_TRANSIT_WATCH_" + uuid.uuid4().hex + ":"
    if action == "start":
        token = "/tmp/danos-r2-transit-watch-" + uuid.uuid4().hex + ".json"
    worker = f'''import json,re,subprocess,time,os
from pathlib import Path
target=("30.30.30.2","30.30.30.3","30.30.30.4","30.30.30.5")
vpp=["/usr/bin/vppctl","-s","/run/vpp/cli.sock"]
result={{"status":"RUNNING","scope":"bounded R2-originated transit observation across R3 VPP restart; one ICMP probe every 250ms, not performance"}}
def cmd(argv,timeout=4):
 try:
  p=subprocess.run(argv,capture_output=True,text=True,timeout=timeout)
  return {{"rc":p.returncode,"out":p.stdout,"err":p.stderr}}
 except Exception as e:return {{"rc":-1,"out":"","err":str(e)}}
result["pid_before"]=cmd(["systemctl","show","vpp","-p","MainPID","--value"])["out"].strip()
result["started_unix"]=time.time(); result["samples"]=[]
deadline=time.monotonic()+{duration}
while time.monotonic()<deadline:
 seq=len(result["samples"]); dest=target[seq%len(target)]; t0=time.time()
 r=cmd(vpp+["ping",dest,"repeat","1","interval","0.05"])
 stats=re.search(r"Statistics: (\\d+) sent, (\\d+) received, (\\d+(?:\\.\\d+)?)% packet loss",r["out"])
 result["samples"].append({{"seq":seq,"target":dest,"unix":t0,"returncode":r["rc"],"output":r["out"],"stderr":r["err"],"sent":int(stats[1]) if stats else None,"received":int(stats[2]) if stats else None,"loss_pct":float(stats[3]) if stats else None}})
 time.sleep(max(0,0.25-(time.time()-t0)))
result["completed_unix"]=time.time()
result["pid_after"]=cmd(["systemctl","show","vpp","-p","MainPID","--value"])["out"].strip()
result["status"]="COMPLETE"
tmp=Path({token!r}+".tmp"); dest=Path({token!r}); tmp.write_text(json.dumps(result,indent=2)+"\\n"); os.replace(tmp,dest)
'''
    if action == "start":
        script = f'''
import subprocess,json,re,base64,sys
from pathlib import Path
result={{"status":"FAIL","scope":"start bounded R2 transit watch"}}
vpp=["/usr/bin/vppctl","-s","/run/vpp/cli.sock"]
def run(argv):
 p=subprocess.run(argv,capture_output=True,text=True,timeout=12)
 if p.returncode or re.search(r"unknown input|connection refused",p.stdout,re.I):raise RuntimeError(p.stdout+p.stderr)
 return p.stdout
try:
 ident=run(vpp+["show","hardware-interfaces","GigabitEthernet1/0/0"])
 assert re.search(r"Ethernet address\\s+"+re.escape({R2_MAC!r})+r"\\b",ident,re.I),"R2 P1 MAC mismatch"
 assert "carrier up full duplex" in ident and "Link speed: 1 Gbps" in ident,"R2 P1 link not ready"
 fib=run(vpp+["show","ip","fib","30.30.30.0/24"])
 assert "10.10.0.1" in fib,"R2 route to remote loopbacks is absent"
 pid=run(["systemctl","show","vpp","-p","MainPID","--value"]).strip()
 assert pid not in ("","0"),"R2 VPP is not active"
 worker={worker!r}
 p=subprocess.Popen([sys.executable,"-c",worker],stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True,close_fds=True)
 result.update(status="STARTED",token={token!r},worker_pid=p.pid,r2_vpp_pid=pid,identity=ident,fib=fib,duration_seconds={duration})
except Exception as error:result["failure_reason"]=str(error)
print({marker!r}+base64.b64encode(json.dumps(result).encode()).decode())
'''
    else:
        if not re.fullmatch(r"/tmp/danos-r2-transit-watch-[0-9a-f]{32}\.json", token):
            raise ValueError("invalid bounded worker token")
        script = f'''
import json,base64,re,subprocess
from pathlib import Path
path=Path({token!r}); result={{"status":"RUNNING","token":str(path)}}
if path.is_file():
 try:
  result=json.loads(path.read_text()); result["worker_status"]=result.get("status"); result["status"]="COMPLETE"; result["token"]=str(path)
 except Exception as error:result={{"status":"FAIL","failure_reason":str(error),"token":str(path)}}
else:
 result["failure_reason"]="worker result not yet written; it may still be running"
print({marker!r}+base64.b64encode(json.dumps(result).encode()).decode())
'''
    compile(worker, "<r2-transit-watch-worker>", "exec")
    compile(script, "<r2-transit-watch-uart-rpc>", "exec")
    return marker, script


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--action", choices=("start", "collect"), required=True)
    parser.add_argument("--token")
    parser.add_argument("--duration", type=int, default=90)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 30 <= args.duration <= 180:
        parser.error("duration must be between 30 and 180 seconds")
    if args.action == "collect" and not args.token:
        parser.error("--token from the start result is required for collect")
    args.output.mkdir(parents=True, exist_ok=True)
    marker, script = remote_script(args.action, args.token, args.duration)
    try:
        result = _exchange(args.device, script, marker, reply_timeout=30)
    except (OSError, ValueError, TimeoutError) as error:
        result = {"status": "UNKNOWN", "failure_reason": str(error)}
    raw = json.dumps(result, indent=2) + "\n"
    name = "start.json" if args.action == "start" else "collected.json"
    (args.output / name).write_text(raw)
    digest = hashlib.sha256(raw.encode()).hexdigest()
    print(json.dumps({"status": result.get("status"), "token": result.get("token"),
                      "samples": len(result.get("samples", [])), "sha256": digest,
                      "failure_reason": result.get("failure_reason")}))
    return 0 if result.get("status") in ("STARTED", "COMPLETE") else 1


if __name__ == "__main__":
    raise SystemExit(main())
