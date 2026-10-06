#!/usr/bin/env python3
"""API-driven real FIB lifecycle gate on an explicitly isolated two-loopback VPP."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

PREFIX = "198.51.100.0/24"


def check_fib(text, table, paths):
    present = re.search(r"^198\.51\.100\.0/24(?:\s|$)", text, re.M)
    if paths == 0:
        if present:
            raise ValueError("deleted route remains in FIB")
        return
    header = f"ipv4-VRF:{table}," if table == 0 else "danos-live,"
    if not present or header not in text:
        raise ValueError("route/table absent from real FIB")
    if not re.search(rf"path-list:.*\blen:{paths}\b", text):
        raise ValueError("unexpected API path count")
    if "API refs:" not in text:
        raise ValueError("route lacks API source")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--container", required=True)
    p.add_argument("--socket-dir", required=True, type=Path)
    p.add_argument("--binary", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    a = p.parse_args()
    if not a.binary.is_file():
        p.error("missing project vpp_live_test binary")
    if a.output.exists():
        p.error("output exists; never overwrite evidence")
    a.output.mkdir(parents=True, mode=0o700)
    result = {"schema": "danos.vpp.control-gate.v1", "status": "FAIL", "phases": [],
              "container": a.container, "binary_sha256": hashlib.sha256(a.binary.read_bytes()).hexdigest(),
              "scope": "API/real FIB lifecycle only; not FRR, restart, packets or DPDK"}
    root = Path(__file__).resolve().parents[2]
    result["commit"] = subprocess.check_output(
        ["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
    result["dirty"] = bool(subprocess.check_output(
        ["git", "-C", str(root), "status", "--porcelain"], text=True).strip())
    index = 0

    def run(label, cmd, env=None):
        nonlocal index
        index += 1
        r = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=30)
        name = f"{index:02d}-{label}.log"
        (a.output / name).write_text(r.stdout + r.stderr)
        result["phases"].append({"label": label, "argv": cmd, "returncode": r.returncode,
                                 "log": name})
        if r.returncode or "unknown input" in (r.stdout + r.stderr).lower():
            raise ValueError(f"{label} command failed")
        return r.stdout

    def fib(table, label, paths):
        text = run(label, ["docker", "exec", a.container, "vppctl", "-s",
                          "/run/danos-vpp/cli.sock", "show", "ip", "fib",
                          "table", str(table), PREFIX])
        check_fib(text, table, paths)

    try:
        run("identity", ["docker", "inspect", "--format", "{{.Image}} {{.State.Status}}", a.container])
        run("version", ["docker", "exec", a.container, "vppctl", "-s",
                        "/run/danos-vpp/cli.sock", "show", "version"])
        for table in (0, 777):
            for label, count, keep in (("add", 2, True), ("repeat", 2, True),
                                       ("withdraw-nh", 1, True), ("restore-nh", 2, True),
                                       ("delete", 2, False)):
                env = os.environ.copy()
                for key in ("VPP_LIVE_VRF", "VPP_LIVE_SINGLE_PATH", "VPP_LIVE_KEEP_ROUTE"):
                    env.pop(key, None)
                env["VPP_API_SOCK"] = str(a.socket_dir.resolve() / "api.sock")
                env["VPP_STAT_SOCK"] = str(a.socket_dir.resolve() / "stats.sock")
                if table: env["VPP_LIVE_VRF"] = "1"
                if count == 1: env["VPP_LIVE_SINGLE_PATH"] = "1"
                if keep: env["VPP_LIVE_KEEP_ROUTE"] = "1"
                run(f"{table}-{label}-api", [str(a.binary.resolve())], env)
                fib(table, f"{table}-{label}-fib", count if keep else 0)
            if table:
                tables = run("table-delete-proof", ["docker", "exec", a.container,
                    "vppctl", "-s", "/run/danos-vpp/cli.sock", "show", "ip", "fib"])
                if "danos-live," in tables:
                    raise ValueError("VRF table remains after API deletion")
        result["status"] = "PASS"
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        result["error"] = str(exc)
    finally:
        result["sha256"] = {f.name: hashlib.sha256(f.read_bytes()).hexdigest()
                            for f in a.output.iterdir() if f.is_file()}
        (a.output / "manifest.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps({"status": result["status"], "output": str(a.output)}))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
