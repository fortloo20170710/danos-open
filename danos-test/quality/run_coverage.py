#!/usr/bin/env python3
"""Fresh GCC/CTest coverage run; never reuse counters or claim hardware coverage."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build-dir", required=True, type=Path)
    p.add_argument("--gcovr", default="gcovr")
    p.add_argument("--jobs", type=int, default=2)
    args = p.parse_args()
    if args.jobs < 1:
        p.error("--jobs must be positive")
    build = args.build_dir.resolve()
    if build.exists():
        p.error("build directory exists; use a new directory (never reuse .gcda)")
    build.mkdir(parents=True)
    reports = build / "reports"
    reports.mkdir()
    manifest = {"schema": "danos.coverage.v1", "status": "ERROR", "commands": [],
                "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "scope": "production C deterministic suite, not live/PCI coverage"}
    for key, cmd in {
        "commit": ["git", "-C", str(ROOT), "rev-parse", "HEAD"],
        "dirty": ["git", "-C", str(ROOT), "status", "--porcelain"],
        "compiler": ["gcc", "--version"], "gcov": ["gcov", "--version"],
        "gcovr": [args.gcovr, "--version"],
    }.items():
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, check=True, timeout=15)
            manifest[key] = bool(r.stdout.strip()) if key == "dirty" else r.stdout.strip()
        except (OSError, subprocess.SubprocessError) as exc:
            manifest[key] = {"error": str(exc)}

    def run(name, cmd):
        with (reports / (name + ".log")).open("wb") as log:
            r = subprocess.Popen(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                 start_new_session=True)
            try:
                rc = r.wait(timeout=900)
            except subprocess.TimeoutExpired:
                os.killpg(r.pid, signal.SIGKILL)
                r.wait()
                manifest["commands"].append({"name": name, "argv": cmd, "timeout": True})
                raise
        manifest["commands"].append({"name": name, "argv": cmd, "returncode": rc})
        return rc

    rc = 1
    try:
        rc = run("configure", ["cmake", "-S", str(ROOT), "-B", str(build),
                              "-DCMAKE_C_COMPILER=gcc", "-DCMAKE_BUILD_TYPE=Debug",
                              "-DDANOS_COVERAGE=ON"])
        if rc == 0:
            rc = run("build", ["cmake", "--build", str(build), "-j", str(args.jobs)])
        if rc == 0:
            test_rc = run("ctest", ["ctest", "--test-dir", str(build),
                                  "--output-on-failure", "-j", str(args.jobs)])
            report_rc = run("gcovr", [
                args.gcovr, "--root", str(ROOT), "--config",
                str(ROOT / "danos-test/quality/gcovr.cfg"),
                "--object-directory", str(build), str(build),
                "--xml-pretty", "--xml", str(reports / "coverage.xml"),
                "--html-details", str(reports / "coverage.html"),
                "--json-summary", str(reports / "summary.json"),
            ])
            rc = test_rc or report_rc
            if report_rc == 0:
                summary = json.loads((reports / "summary.json").read_text())
                if not summary.get("line_total", 0):
                    raise ValueError("empty production coverage report")
                manifest["summary"] = summary
            manifest["status"] = "PASS" if rc == 0 else "ERROR"
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        manifest["error"] = str(exc)
        rc = 1
    finally:
        manifest["files"] = {
            path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(reports.iterdir()) if path.is_file()
        }
        (reports / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(json.dumps({"status": manifest["status"], "reports": str(reports)}))
    return rc


if __name__ == "__main__":
    sys.exit(main())
