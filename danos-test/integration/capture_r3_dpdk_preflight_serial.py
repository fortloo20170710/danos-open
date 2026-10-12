#!/usr/bin/env python3
"""Capture installed R3 PCI/DPDK runtime prerequisites over identity-guarded UART."""
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
    marker = "DANOS_R3_PREFLIGHT_" + uuid.uuid4().hex + ":"
    script = f'''
import subprocess,json,hashlib,re,base64
from pathlib import Path
result={{"scope":"installed R3 PCI/DPDK preflight; no traffic or performance claim"}}
def run(argv):
    p=subprocess.run(argv,capture_output=True,text=True,timeout=15)
    return dict(returncode=p.returncode,stdout=p.stdout,stderr=p.stderr)
try:
    identity=run(["/usr/bin/vppctl","-s","/run/vpp/cli.sock","show","hardware-interfaces","GigabitEthernet1/0/0"])
    assert identity["returncode"]==0 and re.search(r"Ethernet address\\s+"+re.escape({args.expected_mac!r})+r"\\b",identity["stdout"],re.I),"R3 MAC mismatch"
    result["identity"]={{"mac":{args.expected_mac!r},"link_speed":"1 Gbps" if "Link speed: 1 Gbps" in identity["stdout"] else "unknown"}}
    result["mgrd_sha256"]=hashlib.sha256(Path("/usr/bin/danos-mgrd").read_bytes()).hexdigest()
    assert result["mgrd_sha256"]=={args.expected_mgrd_sha256!r},"installed binary mismatch"
    result["build_info"]=Path("/etc/danos/build-info.env").read_text()
    pci=run(["lspci","-Dnnk"])
    assert pci["returncode"]==0,"lspci failed"
    blocks=re.split(r"(?m)(?=^[0-9a-f]{{4}}:[0-9a-f]{{2}}:[0-9a-f]{{2}}\\.[0-7] )",pci["stdout"])
    devices=[b for b in blocks if "[8086:1539]" in b]
    result["i211_devices"]=[{{"bdf":b.split()[0],"driver":re.search(r"Kernel driver in use: ([^\\n]+)",b).group(1) if re.search(r"Kernel driver in use: ([^\\n]+)",b) else "unbound"}} for b in devices]
    result["i211_count"]=len(devices)
    result["i211_drivers"]=[d["driver"] for d in result["i211_devices"]]
    result["hugepages"]=run(["grep","-E","HugePages_(Total|Free|Rsvd)|Hugepagesize","/proc/meminfo"])
    plugins=run(["/usr/bin/vppctl","-s","/run/vpp/cli.sock","show","plugins"])
    result["dpdk_plugin_lines"]=[line.strip() for line in plugins["stdout"].splitlines() if "dpdk" in line.lower()]
    result["vpp_plugins_returncode"]=plugins["returncode"]
    result["api_socket"]=Path("/run/vpp/api.sock").is_socket()
    result["dpdk_plugin_files"]=[str(p) for p in (Path("/usr/lib/x86_64-linux-gnu/vpp_plugins"),Path("/usr/lib/vpp_plugins")) if p.exists() for p in p.glob("*dpdk*plugin*.so")]
    total=re.search(r"HugePages_Total:\\s+(\\d+)",result["hugepages"]["stdout"])
    result["checks"]={{
      "four_i211_devices":result["i211_count"]==4,
      "all_i211_userspace_bound":result["i211_count"]==4 and all(d in ("uio_pci_generic","vfio-pci","igb_uio") for d in result["i211_drivers"]),
      "hugepages_configured":bool(total and int(total.group(1))>0),
      "dpdk_plugin_loaded":plugins["returncode"]==0 and bool(result["dpdk_plugin_lines"]),
      "api_socket":result["api_socket"]
    }}
    result["preflight_status"]="PASS" if all(result["checks"].values()) else "ENVIRONMENT-OPEN"
    result["performance_status"]="ENVIRONMENT-OPEN"
except Exception as error:
    result["preflight_status"]="FAIL"
    result["failure_reason"]=str(error)
print({marker!r}+base64.b64encode(json.dumps(result).encode()).decode())
'''
    try:
        result = _exchange(args.device, script, marker, reply_timeout=60)
    except (OSError, ValueError, TimeoutError) as error:
        result = {"preflight_status": "UNKNOWN", "performance_status": "ENVIRONMENT-OPEN",
                  "failure_reason": str(error)}
    raw = json.dumps(result, indent=2) + "\n"
    (args.output / "result.json").write_text(raw)
    digest = hashlib.sha256(raw.encode()).hexdigest()
    print(json.dumps({"preflight_status": result.get("preflight_status"),
                      "performance_status": result.get("performance_status"),
                      "checks": result.get("checks"), "sha256": digest,
                      "failure_reason": result.get("failure_reason")}))
    return 0 if result.get("preflight_status") == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
