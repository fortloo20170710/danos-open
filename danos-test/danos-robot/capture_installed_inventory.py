#!/usr/bin/env python3
"""Archive fixed read-only installed-runner checks after explicit MAC identity."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

from serial_readonly import read_command, read_installed_inventory


def qualify(inventory, commit, binary_sha256):
    files = inventory.get("files", {})
    metadata = dict(line.split("=", 1) for line in files.get("/etc/danos/build-info.env", "").splitlines() if "=" in line)
    services = {}
    for block in inventory["services"]["stdout"].split("\n\n"):
        values = dict(line.split("=", 1) for line in block.splitlines() if "=" in line)
        if "Id" in values:
            services[values["Id"]] = values
    disks = json.loads(inventory["disks"]["stdout"])["blockdevices"]
    root_disk = any(d.get("tran") in ("sata", "nvme") and
                    any("/" in (p.get("mountpoints") or []) for p in d.get("children", [])) for d in disks)
    pci_blocks = re.split(r"(?m)(?=^[0-9a-f]{2}:[0-9a-f]{2}\.[0-7] )", inventory["pci"]["stdout"])
    i211 = [block for block in pci_blocks if "[8086:1539]" in block]
    return {
        "commands_succeeded": all(inventory[k]["returncode"] == 0 for k in ("root", "kernel", "disks", "pci", "services", "vpp_interfaces", "vpp_version")),
        "installed_root": bool(re.fullmatch(r"/dev/\S+ ext4\s*", inventory["root"]["stdout"])) and root_disk,
        "source_identity": metadata.get("DANOS_BUILD_COMMIT") == commit and metadata.get("DANOS_BUILD_SOURCE_DIRTY") == "0",
        "binary_identity": inventory.get("mgrd_sha256") == binary_sha256,
        "vpp_frr_active": all(services.get(name, {}).get("ActiveState") == "active" and
                              services[name].get("SubState") == "running" and
                              services[name].get("MainPID", "").isdigit() and
                              int(services[name]["MainPID"]) > 0
                              for name in ("vpp.service", "frr.service")),
        "management_disabled": services.get("danos-mgrd.service", {}).get("ActiveState") == "inactive" and services.get("danos-mgrd.service", {}).get("UnitFileState") == "disabled",
        "vpp_sockets": all(inventory.get("sockets", {}).get(p) is True for p in ("/run/vpp/api.sock", "/run/vpp/stats.sock", "/run/vpp/cli.sock")),
        "four_i211_dpdk_bindings": len(i211) == 4 and all(re.search(r"Kernel driver in use: (?:uio_pci_generic|vfio-pci)\b", b) for b in i211),
        "four_vpp_ports": len(re.findall(r"(?m)^GigabitEthernet\d+/0/0\s", inventory["vpp_interfaces"]["stdout"])) == 4,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True)
    parser.add_argument("--expected-mac", required=True)
    parser.add_argument("--expected-commit", required=True)
    parser.add_argument("--expected-mgrd-sha256", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", args.expected_mac):
        parser.error("explicit expected MAC required")
    if not re.fullmatch(r"[0-9a-f]{40}", args.expected_commit) or not re.fullmatch(r"[0-9a-f]{64}", args.expected_mgrd_sha256):
        parser.error("full source commit and binary SHA256 required")
    args.output.mkdir(parents=True, exist_ok=False)
    result = dict(status="FAIL", scope="installed I211 runner identity/runtime only; traffic and management PKI unqualified")
    try:
        hardware = read_command(args.device, ["show", "hardware-interfaces", "GigabitEthernet1/0/0"])
        (args.output / "port1.txt").write_text(hardware.stdout + hardware.stderr)
        if hardware.returncode or not re.search(r"Ethernet address\s+" + re.escape(args.expected_mac) + r"\b", hardware.stdout, re.I):
            raise ValueError("physical node MAC mismatch")
        inventory = read_installed_inventory(args.device)
        raw = json.dumps(inventory, indent=2) + "\n"
        (args.output / "inventory.json").write_text(raw)
        checks = qualify(inventory, args.expected_commit, args.expected_mgrd_sha256)
        result.update(status="PASS" if all(checks.values()) else "FAIL", checks=checks,
                      expected_mac=args.expected_mac, inventory_sha256=hashlib.sha256(raw.encode()).hexdigest())
    except (OSError, ValueError, KeyError, TimeoutError) as error:
        result["failure_reason"] = str(error)
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
