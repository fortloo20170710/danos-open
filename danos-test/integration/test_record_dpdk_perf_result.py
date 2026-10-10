#!/usr/bin/env python3
"""Deterministic tests for PCI DPDK measurement qualification."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


TOOL = Path(__file__).with_name("record_dpdk_perf_result.py")

PREFLIGHT = """status=ENVIRONMENT-OPEN
schema_version=1
preflight_status=PASS
commit=deadbeef
iso_build_commit=deadbeef
iso_source_dirty=0
runner_commit=feedface
iso_sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
target_bdf=0000:01:00.0
pci_vendor_id=0x8086
pci_device_id=0x1539
pci_driver=uio_pci_generic
pci_bound_driver=uio_pci_generic
"""

MEASUREMENT = """lane=pci-dpdk
stage=single-core-64b
commit=deadbeef
iso_sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
packet_size_bytes=64
flows=2
packets_tx=10000
packets_rx=10000
loss_pct=0
duration_ms=10000
pps=1000
mbps=0.512
rtt_p50_us=800
rtt_p99_us=1200
cpu_pct=22.5
ecmp_bucket_0=5000
ecmp_bucket_1=5000
restart_replay=SKIP
"""


class RecordDpdkPerfResultTest(unittest.TestCase):
    def run_case(self, measurement=MEASUREMENT, extra=(), preflight_data=PREFLIGHT):
        with tempfile.TemporaryDirectory(prefix="dpdk-result-test-") as tmp:
            root = Path(tmp)
            preflight = root / "preflight.env"
            measured = root / "measurement.env"
            output = root / "result.env"
            preflight.write_text(preflight_data)
            measured.write_text(measurement)
            command = [sys.executable, str(TOOL), "--preflight", str(preflight),
                       "--measurement", str(measured), "--out", str(output), *extra]
            proc = subprocess.run(command, text=True, capture_output=True, check=False)
            return proc, output.read_text()

    def test_matching_malformed_identity_is_rejected(self):
        digest = "0123456789abcdef" * 4
        for invalid in ("deadbeef", "g" * 64, "0" * 63, "0" * 65):
            with self.subTest(digest=invalid):
                proc, output = self.run_case(
                    measurement=MEASUREMENT.replace(digest, invalid),
                    preflight_data=PREFLIGHT.replace(digest, invalid))
                self.assertEqual(proc.returncode, 1)
                self.assertIn("ISO SHA256 is malformed", output)
        for invalid in ("xyzabcd", "0" * 6, "0" * 41):
            with self.subTest(commit=invalid):
                proc, output = self.run_case(
                    measurement=MEASUREMENT.replace("deadbeef", invalid),
                    preflight_data=PREFLIGHT.replace("deadbeef", invalid))
                self.assertEqual(proc.returncode, 1)
                self.assertIn("ISO build commit is malformed", output)

    def test_valid_measurements_without_thresholds_remain_open(self):
        proc, output = self.run_case()
        self.assertEqual(proc.returncode, 2)
        self.assertIn("status=ENVIRONMENT-OPEN", output)
        self.assertIn("performance_status=ENVIRONMENT-OPEN", output)

    def test_preflight_schema_version_is_required_and_supported(self):
        for replacement in ("", "schema_version=2\n"):
            with self.subTest(replacement=replacement), tempfile.TemporaryDirectory(
                prefix="dpdk-schema-test-"
            ) as tmp:
                root = Path(tmp)
                preflight = root / "preflight.env"
                measurement = root / "measurement.env"
                result = root / "result.env"
                preflight.write_text(PREFLIGHT.replace("schema_version=1\n", replacement))
                measurement.write_text(MEASUREMENT)
                proc = subprocess.run([
                    sys.executable, str(TOOL), "--preflight", str(preflight),
                    "--measurement", str(measurement), "--out", str(result),
                ], text=True, capture_output=True, check=False)
                output = result.read_text()
                self.assertEqual(proc.returncode, 1)
                self.assertIn("preflight schema_version must be 1", output)
                self.assertIn("schema_version=1", output)
                self.assertIn("status=FAIL", output)

    def test_valid_measurements_and_thresholds_pass(self):
        proc, output = self.run_case(extra=("--min-pps", "900", "--max-loss-pct", "0",
                                           "--max-cpu-pct", "50"))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("status=PASS", output)
        self.assertIn("iso_build_commit=deadbeef", output)
        self.assertIn("runner_commit=feedface", output)
        self.assertIn("pci_vendor_id=0x8086", output)

    def test_threshold_failure_is_not_pass(self):
        proc, output = self.run_case(extra=("--min-pps", "1100", "--max-loss-pct", "0",
                                           "--max-cpu-pct", "50"))
        self.assertEqual(proc.returncode, 1)
        self.assertIn("status=FAIL", output)

    def test_reported_pps_must_match_packet_count_and_duration(self):
        measurement = MEASUREMENT.replace("pps=1000\n", "pps=500\n")
        proc, output = self.run_case(measurement=measurement)
        self.assertEqual(proc.returncode, 1)
        self.assertIn("pps disagrees with transmitted packets and duration_ms", output)

    def test_reported_mbps_must_match_64_byte_packet_rate(self):
        measurement = MEASUREMENT.replace("mbps=0.512\n", "mbps=1.024\n")
        proc, output = self.run_case(measurement=measurement)
        self.assertEqual(proc.returncode, 1)
        self.assertIn("mbps disagrees with pps and 64-byte packet size", output)

    def test_performance_thresholds_must_be_in_valid_ranges(self):
        proc, output = self.run_case(extra=("--min-pps", "900", "--max-loss-pct", "101",
                                           "--max-cpu-pct", "50"))
        self.assertEqual(proc.returncode, 1)
        self.assertIn("max_loss_pct must be in [0,100]", output)

    def test_identity_mismatch_is_rejected(self):
        proc, output = self.run_case(measurement=MEASUREMENT.replace("deadbeef", "cafebabe"))
        self.assertEqual(proc.returncode, 1)
        self.assertIn("measurement commit does not match preflight", output)
        self.assertIn("status=FAIL", output)

    def test_nonfinite_thresholds_produce_structured_failure(self):
        for index in (1, 3, 5):
            for value in ("NaN", "sNaN", "Infinity", "-Infinity"):
                extra = ["--min-pps", "900", "--max-loss-pct", "0",
                         "--max-cpu-pct", "50"]
                extra[index] = value
                # '=' form also keeps negative infinity from being parsed as an option.
                arguments = tuple(extra[i] + "=" + extra[i + 1]
                                  for i in (0, 2, 4))
                with self.subTest(index=index, value=value):
                    proc, output = self.run_case(extra=arguments)
                    self.assertEqual(proc.returncode, 1)
                    self.assertIn("status=FAIL", output)
                    self.assertIn("acceptance thresholds must be finite", output)
                    self.assertNotIn("Traceback", proc.stderr)

    def test_preflight_commit_must_match_iso_identity(self):
        with tempfile.TemporaryDirectory(prefix="dpdk-result-test-") as tmp:
            root = Path(tmp)
            preflight = root / "preflight.env"
            measured = root / "measurement.env"
            output_path = root / "result.env"
            preflight.write_text(PREFLIGHT.replace("iso_build_commit=deadbeef", "iso_build_commit=cafebabe"))
            measured.write_text(MEASUREMENT)
            proc = subprocess.run([
                sys.executable, str(TOOL), "--preflight", str(preflight),
                "--measurement", str(measured), "--out", str(output_path),
            ], text=True, capture_output=True, check=False)
            output = output_path.read_text()
        self.assertEqual(proc.returncode, 1)
        self.assertIn("does not match embedded ISO build commit", output)

    def test_dirty_iso_cannot_qualify(self):
        with tempfile.TemporaryDirectory(prefix="dpdk-result-test-") as tmp:
            root = Path(tmp)
            preflight = root / "preflight.env"
            measured = root / "measurement.env"
            output_path = root / "result.env"
            preflight.write_text(PREFLIGHT.replace("iso_source_dirty=0", "iso_source_dirty=1"))
            measured.write_text(MEASUREMENT)
            proc = subprocess.run([
                sys.executable, str(TOOL), "--preflight", str(preflight),
                "--measurement", str(measured), "--out", str(output_path),
            ], text=True, capture_output=True, check=False)
            output = output_path.read_text()
        self.assertEqual(proc.returncode, 1)
        self.assertIn("clean source tree", output)

    def test_missing_ecmp_bucket_is_rejected(self):
        proc, output = self.run_case(measurement=MEASUREMENT.replace("ecmp_bucket_1=5000\n", ""))
        self.assertEqual(proc.returncode, 1)
        self.assertIn("missing: ecmp_bucket_1", output)

    def test_single_flow_cannot_qualify_two_ecmp_buckets(self):
        proc, output = self.run_case(measurement=MEASUREMENT.replace("flows=2\n", "flows=1\n"))
        self.assertEqual(proc.returncode, 1)
        self.assertIn("at least two 5-tuple flows", output)
        self.assertIn("status=FAIL", output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
