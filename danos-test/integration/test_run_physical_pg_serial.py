#!/usr/bin/env python3
"""Offline schema and safety tests for the physical R2 packet-generator RPC."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from run_physical_pg_serial import (R2_MAC, R3_P4_MAC, build_remote,
                                    validate_remote_result, validate_result)


class PhysicalPacketGeneratorSerialTest(unittest.TestCase):
    def valid_result(self):
        count = 256
        return {
            "status": "PASS",
            "scope": "bounded physical 64-byte UDP packet-generator diagnostic",
            "hardware_before": "Ethernet address " + R2_MAC + "\nLink speed: 1 Gbps\ncarrier up full duplex",
            "identity_before": {"pid": "1234", "exe": "/usr/bin/vpp", "start_ticks": "98765"},
            "identity_after": {"pid": "1234", "exe": "/usr/bin/vpp", "start_ticks": "98765"},
            "generated_frames": 3072,
            "hardware_tx_delta": 3072,
            "tx_drops_before": 99,
            "tx_drops_after": 99,
            "cleanup_remaining": [],
            "streams_after": "no streams currently defined\n",
            "phases": [
                {"requested_pps_per_flow": rate, "limit_per_flow": count,
                 "observation_seconds": 1.0,
                 "vpp_cpu_before": {"pid": "1234", "start_ticks": "98765", "cpu_ticks": 100,
                                    "clock_ticks_per_second": 100, "monotonic": 10.0},
                 "vpp_cpu_after": {"pid": "1234", "start_ticks": "98765", "cpu_ticks": 110,
                                   "clock_ticks_per_second": 100, "monotonic": 11.0},
                 "vpp_cpu_elapsed_seconds": 1.0, "vpp_cpu_delta_ticks": 10,
                 "vpp_cpu_clock_ticks_per_second": 100, "vpp_cpu_pct_one_core": 10.0,
                 "hardware_tx_delta": 4 * count,
                 "measured_tx_pps": 4 * count,
                 "measured_mbps": 4 * count * 64 * 8 / 1_000_000,
                 "final_counts": {f"danos64_{rate}_{i}": count for i in range(4)}}
                for rate in (100, 500, 1000)
            ],
        }

    def test_remote_program_is_finite_identity_guarded_and_cleans_streams(self):
        script = build_remote("TEST_MARKER:")
        self.assertIn(R2_MAC, script)
        self.assertIn(R3_P4_MAC, script)
        self.assertIn("size 64-64", script)
        self.assertIn("maximum_generated_frames", script)
        self.assertIn("start_ticks", script)
        self.assertIn("vpp_cpu_pct_one_core", script)
        self.assertIn("packet-generator delete", script)
        self.assertIn("no streams currently defined", script)
        compile(script, "<test-physical-pg-rpc>", "exec")

    def test_count_bounds_and_marker_are_validated(self):
        for marker, count in (("", 256), ("bad\nmarker", 256), ("M:", 255), ("M:", 4097), ("M:", True)):
            with self.subTest(marker=marker, count=count), self.assertRaises(ValueError):
                build_remote(marker, count)

    def test_valid_result_passes(self):
        validate_result(self.valid_result())

    def test_failed_remote_result_retains_phase_diagnostics(self):
        result = self.valid_result()
        result["status"] = "FAIL"
        result["failure_reason"] = "stream exceeded monotonic pacing bound"
        retained = validate_remote_result(result, 256)
        self.assertEqual(retained["failure_reason"], "stream exceeded monotonic pacing bound")
        self.assertEqual(len(retained["phases"]), 3)
        self.assertEqual(retained["status"], "FAIL")

    def test_frame_count_must_match_independent_source_nic(self):
        for field, value in (("generated_frames", 3071), ("hardware_tx_delta", 3071)):
            result = self.valid_result()
            result[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "do not match"):
                validate_result(result)

    def test_vpp_identity_must_remain_stable(self):
        result = self.valid_result()
        result["identity_after"]["start_ticks"] = "different"
        with self.assertRaisesRegex(ValueError, "identity is invalid or changed"):
            validate_result(result)

    def test_source_nic_identity_and_link_are_required(self):
        for replacement in ("wrong MAC", "Ethernet address " + R2_MAC):
            result = self.valid_result()
            result["hardware_before"] = replacement
            with self.subTest(replacement=replacement), self.assertRaises(ValueError):
                validate_result(result)

    def test_all_temporary_streams_must_be_cleaned(self):
        result = self.valid_result()
        result["cleanup_remaining"] = ["danos64_100_0"]
        with self.assertRaisesRegex(ValueError, "cleanup is incomplete"):
            validate_result(result)

    def test_drop_counter_must_be_unchanged(self):
        result = self.valid_result()
        result["tx_drops_after"] += 1
        with self.assertRaisesRegex(ValueError, "TX-drop counter changed"):
            validate_result(result)

    def test_phase_rate_is_derived_from_hardware_delta_and_elapsed_time(self):
        result = self.valid_result()
        result["phases"][0]["measured_tx_pps"] = 500
        with self.assertRaisesRegex(ValueError, "measured TX pps disagrees"):
            validate_result(result)

    def test_phase_hardware_delta_must_match_generated_frames(self):
        result = self.valid_result()
        result["phases"][0]["hardware_tx_delta"] -= 1
        with self.assertRaisesRegex(ValueError, "phase hardware TX delta"):
            validate_result(result)

    def test_phase_mbps_must_match_measured_rate(self):
        result = self.valid_result()
        result["phases"][0]["measured_mbps"] = 1.0
        with self.assertRaisesRegex(ValueError, "measured Mbps disagrees"):
            validate_result(result)

    def test_vpp_cpu_percentage_may_exceed_one_hundred_for_multiple_threads(self):
        result = self.valid_result()
        phase = result["phases"][0]
        phase["vpp_cpu_after"]["cpu_ticks"] = 310
        phase["vpp_cpu_delta_ticks"] = 210
        phase["vpp_cpu_pct_one_core"] = 210.0
        validate_result(result)

    def test_vpp_cpu_sample_tampering_is_rejected(self):
        mutations = (
            lambda p: p.update(vpp_cpu_pct_one_core=11.0),
            lambda p: p["vpp_cpu_after"].update(pid="9999"),
            lambda p: p.update(vpp_cpu_delta_ticks=-1),
        )
        for mutate in mutations:
            result = self.valid_result()
            mutate(result["phases"][0])
            with self.subTest(mutate=mutate), self.assertRaises(ValueError):
                validate_result(result)


if __name__ == "__main__":
    unittest.main(verbosity=2)
