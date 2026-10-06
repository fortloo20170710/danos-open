#!/usr/bin/env python3
"""Read-only VPP evidence collection; never a forwarding/PERF PASS gate."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import stat
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
QUERIES = {
    "version": ("show", "version"),
    "interfaces": ("show", "interface"),
    "hardware": ("show", "hardware-interfaces"),
    "ipv4-fib": ("show", "ip", "fib"),
    "ipv6-fib": ("show", "ip6", "fib"),
    "load-balance": ("show", "load-balance"),
    "errors": ("show", "errors"),
    "runtime": ("show", "runtime"),
    "threads": ("show", "threads"),
}


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def command(argv, directory, name, timeout):
    start = time.monotonic()
    stdout = directory / (name + ".stdout")
    stderr = directory / (name + ".stderr")
    timed_out = False
    with stdout.open("xb") as out, stderr.open("xb") as err:
        process = subprocess.Popen(argv, stdout=out, stderr=err, start_new_session=True)
        try:
            rc = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGKILL)
            rc = process.wait()
    # Some vppctl versions return zero for a CLI error; retain all raw output.
    text = stdout.read_text(errors="replace") + stderr.read_text(errors="replace")
    cli_error = any(marker in text.lower() for marker in
                    ("unknown input", "unknown command", "connection refused",
                     "connect: no such file", "failed to connect"))
    return {
        "argv": argv, "returncode": rc, "timeout": timed_out,
        "cli_error": cli_error, "ok": rc == 0 and not timed_out and not cli_error,
        "duration_seconds": round(time.monotonic() - start, 6),
        "stdout": stdout.name, "stderr": stderr.name,
        "stdout_sha256": digest(stdout), "stderr_sha256": digest(stderr),
    }


def git_info():
    def read(args):
        try:
            r = subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True,
                               text=True, timeout=5, check=True)
            return r.stdout.strip()
        except (OSError, subprocess.SubprocessError):
            return None
    return {"commit": read(["rev-parse", "HEAD"]),
            "dirty": read(["status", "--porcelain"]) != ""}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path,
                        help="new evidence directory; existing directories are refused")
    parser.add_argument("--label", default="snapshot", help="e.g. before/add/withdraw/restore")
    parser.add_argument("--vppctl", default="vppctl")
    parser.add_argument("--cli-socket", default="/run/vpp/cli.sock")
    parser.add_argument("--container", help="existing Docker container; never starts one")
    parser.add_argument("--timeout", type=float, default=10)
    parser.add_argument("--iso", type=Path, help="optional exact project ISO to hash")
    parser.add_argument("--evidence", type=Path, action="append", default=[],
                        help="attach existing logs/schema/wire captures; never executes them")
    args = parser.parse_args(argv)
    if not 0 < args.timeout <= 60:
        parser.error("--timeout must be in (0,60]")
    files = ([args.iso] if args.iso else []) + args.evidence
    for path in files:
        if not path.is_file():
            parser.error(f"not a regular file: {path}")
    try:
        args.output.mkdir(parents=True, exist_ok=False)
        args.output.chmod(0o700)
    except OSError as exc:
        parser.error(str(exc))
    manifest = {
        "schema": "danos.vpp.runtime-evidence.v1",
        "status": "SKIP", "acceptance": "NOT_EVALUATED",
        "label": args.label,
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "collector_host": socket.gethostname(), "git": git_info(),
        "runtime": {"container": args.container, "cli_socket": args.cli_socket},
        "queries": {}, "attachments": [],
        "limits": ["read-only CLI snapshot, not packet or API-wire capture",
                   "not proof of forwarding, restart recovery or throughput",
                   "queries are sequential, not an atomic runtime snapshot"],
    }
    for index, path in enumerate(files):
        target = args.output / f"attachment-{index}-{path.name}"
        shutil.copyfile(path, target)
        manifest["attachments"].append({
            "source": str(path.resolve()), "file": target.name,
            "sha256": digest(target), "kind": "iso" if path == args.iso else "evidence",
        })
    prefix = None
    if args.container:
        docker = shutil.which("docker")
        if docker:
            prefix = [docker, "exec", args.container, args.vppctl, "-s", args.cli_socket]
        else:
            manifest["reason"] = "docker executable unavailable"
    else:
        executable = shutil.which(args.vppctl)
        try:
            sock_present = stat.S_ISSOCK(Path(args.cli_socket).stat().st_mode)
        except OSError:
            sock_present = False
        if not executable:
            manifest["reason"] = "vppctl executable unavailable"
        elif not sock_present:
            manifest["reason"] = "VPP CLI socket unavailable"
        else:
            prefix = [executable, "-s", args.cli_socket]
    if prefix:
        try:
            for name, query in QUERIES.items():
                result = command(prefix + list(query), args.output, name, args.timeout)
                manifest["queries"][name] = result
                # No usable runtime: do not repeat the same unavailable/timeout command.
                if name == "version" and not result["ok"]:
                    break
            complete = len(manifest["queries"]) == len(QUERIES)
            manifest["status"] = ("CAPTURED" if complete and all(
                q["ok"] for q in manifest["queries"].values()) else "ERROR")
        except OSError as exc:
            manifest["status"] = "ERROR"
            manifest["reason"] = str(exc)
    (args.output / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"status": manifest["status"], "acceptance": "NOT_EVALUATED",
                      "manifest": str(args.output / "manifest.json")}))
    return {"CAPTURED": 0, "ERROR": 1, "SKIP": 77}[manifest["status"]]


if __name__ == "__main__":
    raise SystemExit(main())
