#!/usr/bin/env python3
"""Run a bounded four-flow 64-byte UDP packet-generator batch on physical R2."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "danos-test/danos-robot"))
sys.path.insert(0, str(ROOT / "danos-test/perf"))
from serial_readonly import _exchange
from render_physical_pg import render

R2_MAC = "00:1f:7a:69:f5:ec"
R3_P4_MAC = "00:1f:7a:69:f7:4f"
R2_INTERFACE = "GigabitEthernet1/0/0"
TARGETS = tuple(f"30.30.30.{host}" for host in range(2, 6))
RATES = (100, 500, 1000)


def build_remote(marker: str, count: int = 256) -> str:
    if not marker or "\n" in marker:
        raise ValueError("a single-line RPC marker is required")
    if type(count) is not int or not 256 <= count <= 4096:
        raise ValueError("per-flow count must be 256..4096")
    definitions = {}
    for rate in RATES:
        lines = render(R2_INTERFACE, R2_MAC, R3_P4_MAC, "10.10.0.2",
                       TARGETS, rate, count).splitlines()
        definitions[str(rate)] = [
            line.replace(f"danos_physical_udp_{index}", f"danos64_{rate}_{index}")
            for index, line in enumerate(lines)
        ]
    script = f'''import base64,json,re,subprocess,time,os
marker={marker!r}; definitions={definitions!r}; expected_mac={R2_MAC!r}
interface={R2_INTERFACE!r}; cap={count}; vpp=['/usr/bin/vppctl','-s','/run/vpp/cli.sock']
result={{'status':'FAIL','scope':'bounded physical 64-byte UDP packet-generator diagnostic',
 'rates_pps_per_flow':{list(RATES)!r},'frames_per_flow_per_phase':cap,
 'maximum_generated_frames':12*cap,'phases':[]}}
def run(argv,timeout=8):
 p=subprocess.run(argv,capture_output=True,text=True,timeout=timeout)
 if p.returncode: raise RuntimeError('command failed '+repr(argv)+': '+p.stdout[-300:]+p.stderr[-300:])
 if re.search(r'unknown input|connection refused|failed to',p.stdout,re.I): raise RuntimeError('VPP rejected '+repr(argv)+': '+p.stdout[-400:])
 return p.stdout
def cli(command): return run(vpp+[command])
def identity():
 pid=run(['systemctl','show','vpp','-p','MainPID','--value']).strip()
 if not pid.isdigit() or pid=='0': raise RuntimeError('VPP is inactive')
 proc='/proc/'+pid
 return {{'pid':pid,'exe':os.readlink(proc+'/exe'),'start_ticks':open(proc+'/stat').read().split()[21]}}
def cpu_sample(pid):
 fields=open('/proc/'+pid+'/stat').read().rsplit(') ',1)[1].split()
 return {{'pid':pid,'start_ticks':fields[19],
  'cpu_ticks':int(fields[11])+int(fields[12]),
  'clock_ticks_per_second':os.sysconf('SC_CLK_TCK'),'monotonic':time.monotonic()}}
def tx_drops(raw): return sum(int(v) for v in re.findall(r'(?m)^\\s*(\\d+)\\s+\\S+-tx\\s+Tx packet drops',raw))
def tx_good(raw):
 m=re.search(r'(?m)^\\s*tx_good_packets\\s+(\\d+)\\s*$',raw)
 if not m: raise RuntimeError('hardware tx_good_packets unavailable')
 return int(m[1])
def stream_rows(raw,names):
 rows={{}}
 for line in raw.splitlines():
  m=re.match(r'^\\s*(\\S+)\\s+(Yes|No)\\s+(\\d+)\\s+limit\\b',line)
  if m and m[1] in names: rows[m[1]]={{'enabled':m[2]=='Yes','count':int(m[3])}}
 if set(rows)!=set(names): raise RuntimeError('packet-generator rows incomplete: '+repr(rows))
 return rows
owned=[]
try:
 result['identity_before']=identity()
 if not result['identity_before']['exe'].endswith('/vpp'): raise RuntimeError('unexpected R2 VPP executable')
 hardware=cli('show hardware-interfaces '+interface); result['hardware_before']=hardware
 if not re.search(r'Ethernet address\\s+'+re.escape(expected_mac)+r'\\b',hardware,re.I): raise RuntimeError('R2 P1 MAC mismatch')
 if 'Link speed: 1 Gbps' not in hardware or 'carrier up full duplex' not in hardware: raise RuntimeError('R2 P1 link not ready at 1 Gbps full duplex')
 if 'no streams currently defined' not in cli('show packet-generator'): raise RuntimeError('existing packet-generator stream; refusing overlap')
 result['hardware_tx_before']=tx_good(hardware); result['tx_drops_before']=tx_drops(cli('show errors'))
 for rate_text,commands in definitions.items():
  rate=int(rate_text); names=[f'danos64_{{rate}}_{{i}}' for i in range(4)]
  phase={{'requested_pps_per_flow':rate,'limit_per_flow':cap,
   'tx_drops_before':tx_drops(cli('show errors')),
   'hardware_tx_before':tx_good(cli('show hardware-interfaces '+interface))}}
  result['phases'].append(phase)
  for command,name in zip(commands,names): owned.append(name); cli(command)
  initial=stream_rows(cli('show packet-generator'),names)
  if any(row['enabled'] or row['count']!=0 for row in initial.values()): raise RuntimeError('new stream did not start idle')
  phase['vpp_cpu_before']=cpu_sample(result['identity_before']['pid'])
  enabled={{}}
  phase_start=time.monotonic()
  for name in names:
   enabled[name]=time.monotonic(); cli('packet-generator enable-stream '+name)
  deadline=time.monotonic()+cap/rate+5; samples=0; rows={{}}
  while time.monotonic()<deadline:
   rows=stream_rows(cli('show packet-generator'),names); now=time.monotonic(); samples+=1
   for name,row in rows.items():
    age=now-enabled[name]; allowed=rate*(age+0.1)+32
    if row['count']>allowed: raise RuntimeError('stream exceeded monotonic pacing bound: '+name+' count='+str(row['count'])+' elapsed='+format(age,'.6f')+'s allowed='+format(allowed,'.1f'))
   if tx_drops(cli('show errors'))>phase['tx_drops_before']: raise RuntimeError('new R2 TX drop; stopping remaining phases')
   if all(not row['enabled'] and row['count']==cap for row in rows.values()): break
   time.sleep(0.1)
  else: raise RuntimeError('finite stream phase exceeded deadline: '+rate_text)
  phase_end=time.monotonic()
  phase['observation_seconds']=phase_end-phase_start; phase['sample_count']=samples
  phase['vpp_cpu_after']=cpu_sample(result['identity_before']['pid'])
  cpu_before=phase['vpp_cpu_before']; cpu_after=phase['vpp_cpu_after']
  if cpu_before['start_ticks']!=result['identity_before']['start_ticks'] or cpu_after['start_ticks']!=cpu_before['start_ticks']:
   raise RuntimeError('VPP CPU sample identity changed')
  cpu_elapsed=cpu_after['monotonic']-cpu_before['monotonic']
  cpu_ticks=cpu_after['cpu_ticks']-cpu_before['cpu_ticks']
  if cpu_elapsed<=0 or cpu_ticks<0 or cpu_after['clock_ticks_per_second']!=cpu_before['clock_ticks_per_second']:
   raise RuntimeError('invalid VPP CPU sample interval')
  phase['vpp_cpu_elapsed_seconds']=cpu_elapsed
  phase['vpp_cpu_delta_ticks']=cpu_ticks
  phase['vpp_cpu_clock_ticks_per_second']=cpu_before['clock_ticks_per_second']
  phase['vpp_cpu_pct_one_core']=cpu_ticks*100/(cpu_before['clock_ticks_per_second']*cpu_elapsed)
  phase['final_counts']={{name:rows[name]['count'] for name in names}}
  phase['hardware_tx_after']=tx_good(cli('show hardware-interfaces '+interface))
  phase['hardware_tx_delta']=phase['hardware_tx_after']-phase['hardware_tx_before']
  phase['measured_tx_pps']=phase['hardware_tx_delta']/phase['observation_seconds']
  phase['measured_mbps']=phase['measured_tx_pps']*64*8/1000000
  phase['tx_drops_after']=tx_drops(cli('show errors'))
  if identity()!=result['identity_before']: raise RuntimeError('R2 VPP identity changed')
  for name in names: cli('packet-generator disable-stream '+name); cli('packet-generator delete '+name); owned.remove(name)
 result['hardware_after']=cli('show hardware-interfaces '+interface)
 result['hardware_tx_after']=tx_good(result['hardware_after'])
 result['hardware_tx_delta']=result['hardware_tx_after']-result['hardware_tx_before']
 result['tx_drops_after']=tx_drops(cli('show errors')); result['identity_after']=identity()
 result['streams_after']=cli('show packet-generator')
 result['generated_frames']=sum(sum(p['final_counts'].values()) for p in result['phases'])
 if result['identity_after']!=result['identity_before']: raise RuntimeError('R2 VPP identity changed at completion')
 if result['generated_frames']!=12*cap or result['hardware_tx_delta']!=12*cap: raise RuntimeError('stream frame count and R2 NIC TX delta disagree')
 if result['tx_drops_after']!=result['tx_drops_before']: raise RuntimeError('R2 TX-drop counter changed')
 if 'no streams currently defined' not in result['streams_after']: raise RuntimeError('owned packet-generator streams remain')
 result['status']='PASS'
except Exception as error: result['failure_reason']=str(error)
finally:
 try:
  if owned and identity()==result.get('identity_before'):
   for name in list(owned):
    try: cli('packet-generator disable-stream '+name); cli('packet-generator delete '+name); owned.remove(name)
    except Exception: pass
  result['cleanup_remaining']=owned
  if result.get('identity_before') and identity()==result['identity_before']:
   result['streams_final']=cli('show packet-generator'); result['hardware_final']=cli('show hardware-interfaces '+interface)
 except Exception as error: result['cleanup_error']=str(error)
print(marker+base64.b64encode(json.dumps(result,separators=(',',':')).encode()).decode())
'''
    compile(script, "<physical-pg-rpc>", "exec")
    return script


def validate_result(result: dict, count: int = 256) -> None:
    if type(count) is not int or not 256 <= count <= 4096:
        raise ValueError("per-flow count must be 256..4096")
    if result.get("status") != "PASS":
        raise ValueError(result.get("failure_reason", "physical packet-generator run failed"))
    if result.get("scope") != "bounded physical 64-byte UDP packet-generator diagnostic":
        raise ValueError("unexpected packet-generator result scope")
    before, after = result.get("identity_before"), result.get("identity_after")
    if (not isinstance(before, dict) or before != after or
            not re.fullmatch(r"[1-9][0-9]*", str(before.get("pid", ""))) or
            not str(before.get("exe", "")).endswith("/vpp") or not before.get("start_ticks")):
        raise ValueError("R2 VPP identity is invalid or changed")
    hardware = result.get("hardware_before", "")
    if not re.search(r"Ethernet address\s+" + re.escape(R2_MAC) + r"\b", hardware, re.I):
        raise ValueError("R2 source hardware identity is missing or mismatched")
    if "Link speed: 1 Gbps" not in hardware or "carrier up full duplex" not in hardware:
        raise ValueError("R2 source link is not verified at 1 Gbps full duplex")
    expected = len(RATES) * len(TARGETS) * count
    if result.get("generated_frames") != expected or result.get("hardware_tx_delta") != expected:
        raise ValueError("generated frames do not match hardware TX delta")
    if type(result.get("tx_drops_before")) is not int or result.get("tx_drops_after") != result.get("tx_drops_before"):
        raise ValueError("R2 TX-drop counter changed or is invalid")
    if result.get("cleanup_remaining") != [] or "no streams currently defined" not in result.get("streams_after", ""):
        raise ValueError("packet-generator cleanup is incomplete")
    phases = result.get("phases")
    if not isinstance(phases, list) or len(phases) != len(RATES):
        raise ValueError("packet-generator phase list is incomplete")
    for phase, rate in zip(phases, RATES):
        if phase.get("requested_pps_per_flow") != rate or phase.get("limit_per_flow") != count:
            raise ValueError("packet-generator phase configuration mismatch")
        counts = phase.get("final_counts")
        names = {f"danos64_{rate}_{i}" for i in range(len(TARGETS))}
        if not isinstance(counts, dict) or set(counts) != names or any(v != count for v in counts.values()):
            raise ValueError("phase did not complete exact per-flow frame limits")
        elapsed = phase.get("observation_seconds")
        if type(elapsed) not in (int, float) or not math.isfinite(elapsed) or elapsed <= 0:
            raise ValueError("phase observation time is invalid")
        cpu_before, cpu_after = phase.get("vpp_cpu_before"), phase.get("vpp_cpu_after")
        cpu_fields = ("vpp_cpu_before", "vpp_cpu_after", "vpp_cpu_elapsed_seconds",
                      "vpp_cpu_delta_ticks", "vpp_cpu_clock_ticks_per_second",
                      "vpp_cpu_pct_one_core")
        if any(key in phase for key in cpu_fields):
            if not isinstance(cpu_before, dict) or not isinstance(cpu_after, dict):
                raise ValueError("VPP CPU samples are incomplete")
            if (str(cpu_before.get("pid")) != str(before["pid"]) or
                str(cpu_after.get("pid")) != str(before["pid"]) or
                not cpu_before.get("start_ticks") or
                cpu_before.get("start_ticks") != before["start_ticks"] or
                cpu_after.get("start_ticks") != cpu_before.get("start_ticks")):
                raise ValueError("VPP CPU sample identity is invalid or changed")
            for sample in (cpu_before, cpu_after):
                ticks = sample.get("cpu_ticks")
                hz = sample.get("clock_ticks_per_second")
                monotonic = sample.get("monotonic")
                if (type(ticks) is not int or ticks < 0 or type(hz) is not int or hz <= 0 or
                        type(monotonic) not in (int, float) or not math.isfinite(monotonic)):
                    raise ValueError("VPP CPU sample values are invalid")
            cpu_elapsed = cpu_after["monotonic"] - cpu_before["monotonic"]
            cpu_delta = cpu_after["cpu_ticks"] - cpu_before["cpu_ticks"]
            hz = cpu_before["clock_ticks_per_second"]
            cpu_pct = phase.get("vpp_cpu_pct_one_core")
            expected_cpu_pct = cpu_delta * 100 / (hz * cpu_elapsed) if cpu_elapsed > 0 and cpu_delta >= 0 else -1
            if (cpu_after["clock_ticks_per_second"] != hz or
                    phase.get("vpp_cpu_delta_ticks") != cpu_delta or cpu_delta < 0 or
                    type(phase.get("vpp_cpu_clock_ticks_per_second")) is not int or
                    phase["vpp_cpu_clock_ticks_per_second"] != hz or
                    type(phase.get("vpp_cpu_elapsed_seconds")) not in (int, float) or
                    not math.isclose(phase["vpp_cpu_elapsed_seconds"], cpu_elapsed, rel_tol=1e-9, abs_tol=1e-9) or
                    cpu_elapsed <= 0 or type(cpu_pct) not in (int, float) or
                    not math.isfinite(cpu_pct) or cpu_pct < 0 or
                    not math.isclose(cpu_pct, expected_cpu_pct, rel_tol=1e-9, abs_tol=1e-9)):
                raise ValueError("VPP CPU measurement is inconsistent")
        phase_frames = len(names) * count
        if phase.get("hardware_tx_delta") != phase_frames:
            raise ValueError("phase hardware TX delta does not match generated frames")
        measured_pps = phase.get("measured_tx_pps")
        measured_mbps = phase.get("measured_mbps")
        if (type(measured_pps) not in (int, float) or not math.isfinite(measured_pps) or
                abs(measured_pps - phase_frames / elapsed) > max(0.01, measured_pps * 1e-6)):
            raise ValueError("measured TX pps disagrees with hardware frames and duration")
        expected_mbps = measured_pps * 64 * 8 / 1_000_000
        if (type(measured_mbps) not in (int, float) or not math.isfinite(measured_mbps) or
                abs(measured_mbps - expected_mbps) > max(0.000001, expected_mbps * 1e-6)):
            raise ValueError("measured Mbps disagrees with measured TX pps")


def validate_remote_result(result: dict, count: int) -> dict:
    """Preserve the complete remote diagnostic when a captured result fails validation."""
    try:
        validate_result(result, count)
    except ValueError as error:
        result["status"] = "FAIL"
        result.setdefault("failure_reason", str(error))
        result["validation_failure"] = str(error)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--count", type=int, default=256)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 256 <= args.count <= 4096:
        parser.error("count must be 256..4096")
    args.output.mkdir(parents=True, exist_ok=False)
    marker = "DANOS_PHYSICAL_PG_" + uuid.uuid4().hex + ":"
    try:
        result = validate_remote_result(
            _exchange(args.device, build_remote(marker, args.count), marker, reply_timeout=60),
            args.count)
    except (OSError, ValueError, TimeoutError) as error:
        result = {"status": "FAIL", "scope": "bounded physical 64-byte UDP packet-generator diagnostic",
                  "failure_reason": str(error)}
    payload = json.dumps(result, indent=2) + "\n"
    (args.output / "result.json").write_text(payload)
    print(json.dumps({"status": result.get("status"), "generated_frames": result.get("generated_frames"),
                      "hardware_tx_delta": result.get("hardware_tx_delta"),
                      "tx_drops_before": result.get("tx_drops_before"),
                      "tx_drops_after": result.get("tx_drops_after"),
                      "sha256": hashlib.sha256(payload.encode()).hexdigest(),
                      "failure_reason": result.get("failure_reason")}))
    return 0 if result.get("status") == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
