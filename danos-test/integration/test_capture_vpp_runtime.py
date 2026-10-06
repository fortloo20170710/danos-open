"""Collector logic tests with an explicit fake CLI, not runtime acceptance."""
import importlib.util
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "collector", Path(__file__).with_name("capture_vpp_runtime.py"))
collector = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(collector)


class CaptureTest(unittest.TestCase):
    def test_missing_runtime_skip_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "capture"
            self.assertEqual(collector.main(["--output", str(out),
                                            "--vppctl", "/definitely-missing/vppctl"]), 77)
            manifest = json.loads((out / "manifest.json").read_text())
            self.assertEqual(manifest["acceptance"], "NOT_EVALUATED")
            self.assertFalse(manifest["queries"])
            with self.assertRaises(SystemExit):
                collector.main(["--output", str(out)])

    def run_fake(self, body):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        cli = root / "vppctl"
        cli.write_text("#!/bin/sh\n" + body)
        cli.chmod(0o700)
        sock = socket.socket(socket.AF_UNIX)
        self.addCleanup(sock.close)
        sock.bind(str(root / "cli.sock"))
        evidence = root / "live.log"
        evidence.write_text("fixture only\n")
        with patch.object(collector, "git_info",
                          return_value={"commit": "fixture", "dirty": True}):
            rc = collector.main(["--output", str(root / "out"), "--vppctl", str(cli),
                                 "--cli-socket", str(root / "cli.sock"),
                                 "--timeout", "0.1", "--label", "fixture",
                                 "--evidence", str(evidence)])
        manifest = json.loads((root / "out/manifest.json").read_text())
        return rc, manifest, root / "out"

    def test_read_only_queries_and_hashes(self):
        rc, manifest, out = self.run_fake('echo "fixture $*"\n')
        self.assertEqual(rc, 0)
        self.assertEqual(manifest["status"], "CAPTURED")
        self.assertEqual(manifest["acceptance"], "NOT_EVALUATED")
        self.assertEqual(len(manifest["queries"]), len(collector.QUERIES))
        for name, query in manifest["queries"].items():
            self.assertEqual(query["argv"][3:], list(collector.QUERIES[name]))
            self.assertTrue(query["ok"])
            self.assertEqual(query["stdout_sha256"], collector.digest(out / query["stdout"]))
        attachment = manifest["attachments"][0]
        self.assertEqual(attachment["sha256"], collector.digest(out / attachment["file"]))

    def test_zero_exit_cli_error_is_not_success(self):
        rc, manifest, _ = self.run_fake('echo "unknown input"\n')
        self.assertEqual(rc, 1)
        self.assertEqual(manifest["status"], "ERROR")
        self.assertTrue(manifest["queries"]["version"]["cli_error"])
        self.assertEqual(len(manifest["queries"]), 1)

    def test_timeout_kills_command_and_keeps_manifest(self):
        rc, manifest, _ = self.run_fake("sleep 5\n")
        self.assertEqual(rc, 1)
        self.assertTrue(manifest["queries"]["version"]["timeout"])

    def test_legacy_gates_are_refused(self):
        wrapper = Path(__file__).with_name("run_vpp_verify.sh")
        for mode in ("perf", "conformance", "e2e", "api-connect", "stat-connect"):
            result = subprocess.run(["bash", str(wrapper), mode], capture_output=True,
                                    text=True, timeout=5)
            self.assertEqual(result.returncode, 2)
            self.assertNotIn("[PASS]", result.stdout)


if __name__ == "__main__":
    unittest.main()
