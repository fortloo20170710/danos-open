#!/usr/bin/env python3
"""Restore one explicit existing lab role over UART after identity checks."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import sys
import uuid

from physical_lab_profile import render

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "danos-test/danos-robot"))
from serial_readonly import _exchange


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--role", choices=("R1", "R2", "R3", "R4"), required=True)
    parser.add_argument("--expected-mac", required=True)
    parser.add_argument("--expected-mgrd-sha256", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", args.expected_mac) or not re.fullmatch(r"[0-9a-f]{64}", args.expected_mgrd_sha256):
        parser.error("exact MAC and binary SHA256 required")
    args.output.mkdir(parents=True, exist_ok=False)
    profile = render(args.role)
    profile_sha = hashlib.sha256(profile.encode()).hexdigest()
    hook_bytes = (ROOT / "danos-test/installer/replay_physical_lab.sh").read_bytes()
    hook_sha = hashlib.sha256(hook_bytes).hexdigest()
    (args.output / "physical-lab.cli").write_text(profile)
    marker = "DANOS_LAB_RESTORE_" + uuid.uuid4().hex + ":"
    script = f'''
import subprocess,json,base64,hashlib,re
from pathlib import Path
result = dict(status="FAIL",scope="explicit lab CLI profile only; no mgrd management activation")
created = False
path = Path("/etc/danos/physical-lab.cli")
try:
    identity = subprocess.run(["/usr/bin/vppctl","-s","/run/vpp/cli.sock","show","hardware-interfaces","GigabitEthernet1/0/0"],capture_output=True,text=True,timeout=10)
    result["identity"] = identity.stdout
    assert identity.returncode == 0 and re.search(r"Ethernet address\\s+" + re.escape({args.expected_mac!r}) + r"\\b",identity.stdout,re.I), "MAC mismatch"
    assert hashlib.sha256(Path("/usr/bin/danos-mgrd").read_bytes()).hexdigest() == {args.expected_mgrd_sha256!r}, "binary mismatch"
    profile = base64.b64decode({base64.b64encode(profile.encode()).decode()!r}).decode()
    if path.exists():
        assert path.read_text() == profile, "existing role differs; refusing overwrite"
    hook = Path("/usr/libexec/danos/replay-physical-lab")
    if hook.exists():
        assert hashlib.sha256(hook.read_bytes()).hexdigest() == {hook_sha!r}, "replay hook mismatch"
    else:
        hook.parent.mkdir(parents=True,exist_ok=True)
        with hook.open("xb") as stream:
            stream.write(base64.b64decode({base64.b64encode(hook_bytes).decode()!r}))
        hook.chmod(0o755)
    assert hashlib.sha256(hook.read_bytes()).hexdigest() == {hook_sha!r}
    result["hook_sha256"] = {hook_sha!r}
    post = subprocess.check_output(["systemctl","show","vpp","-p","ExecStartPost","--value"],text=True,timeout=10)
    if str(hook) not in post:
        dropin = Path("/etc/systemd/system/vpp.service.d/30-danos-lab-replay.conf")
        content = "[Service]\\nExecStartPost=/bin/sh /usr/libexec/danos/replay-physical-lab\\n"
        if dropin.exists():
            assert dropin.read_text() == content, "existing drop-in differs; refusing overwrite"
        else:
            dropin.parent.mkdir(parents=True,exist_ok=True)
            with dropin.open("x") as stream:
                stream.write(content)
        subprocess.run(["systemctl","daemon-reload"],check=True,timeout=10)
    assert str(hook) in subprocess.check_output(["systemctl","show","vpp","-p","ExecStartPost","--value"],text=True,timeout=10)
    result["restart_hook_registered"] = True
    if path.exists():
        assert path.read_text() == profile, "existing role differs; refusing overwrite"
    else:
        with path.open("x") as stream:
            stream.write(profile)
        created = True
    result["profile_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
    replay = subprocess.run(["/bin/sh",str(hook),str(path)],capture_output=True,text=True,timeout=40)
    result["replay"] = dict(returncode=replay.returncode,stdout=replay.stdout,stderr=replay.stderr)
    assert replay.returncode == 0 and "LAB-REPLAY PASS" in replay.stdout, "lab replay failed"
    result["status"] = "PASS"
except Exception as error:
    result["failure_reason"] = str(error)
    if created:
        path.unlink()
        result["new_profile_removed_after_failure"] = True
print({marker!r} + base64.b64encode(json.dumps(result).encode()).decode())
'''
    try:
        result = _exchange(args.device, script, marker)
    except (OSError, ValueError, TimeoutError) as error:
        result = dict(status="UNKNOWN", failure_reason=str(error), scope="transport failed; inspect remote state before retry")
    result.update(role=args.role, expected_mac=args.expected_mac, expected_profile_sha256=profile_sha)
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
