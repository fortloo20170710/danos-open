#!/usr/bin/env python3
"""Unit tests for combined physical UDP source/router/receiver evidence."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from verify_physical_udp64_window import WindowError, verify_window
from run_physical_pg_serial import R2_MAC


def source_result():
    count = 256
    return {
        "status": "PASS", "scope": "bounded physical 64-byte UDP packet-generator diagnostic",
        "rates_pps_per_flow": [100, 500, 1000], "frames_per_flow_per_phase": count,
        "maximum_generated_frames": 12 * count,
        "identity_before": {"pid": "200", "exe": "/usr/bin/vpp", "start_ticks": "900"},
        "identity_after": {"pid": "200", "exe": "/usr/bin/vpp", "start_ticks": "900"},
        "hardware_before": f"Ethernet address {R2_MAC}\nLink speed: 1 Gbps\ncarrier up full duplex",
        "generated_frames": 3072, "hardware_tx_delta": 3072,
        "tx_drops_before": 0, "tx_drops_after": 0,
        "cleanup_remaining": [], "streams_after": "no streams currently defined\n",
        "phases": [{
            "requested_pps_per_flow": rate, "limit_per_flow": count,
            "vpp_cpu_before": {"pid": "200", "start_ticks": "900", "cpu_ticks": 0,
                               "clock_ticks_per_second": 100, "monotonic": 1.0},
            "vpp_cpu_after": {"pid": "200", "start_ticks": "900", "cpu_ticks": 10,
                              "clock_ticks_per_second": 100, "monotonic": 2.0},
            "vpp_cpu_elapsed_seconds": 1.0, "vpp_cpu_delta_ticks": 10,
            "vpp_cpu_clock_ticks_per_second": 100, "vpp_cpu_pct_one_core": 10.0,
            "final_counts": {f"danos64_{rate}_{i}": count for i in range(4)},
            "observation_seconds": 1.0, "hardware_tx_delta": 1024,
            "measured_tx_pps": 1024.0, "measured_mbps": 0.524288,
        } for rate in (100, 500, 1000)],
    }


def r3_snapshot(phase, counters):
    ports = ("GigabitEthernet1/0/0", "GigabitEthernet2/0/0", "GigabitEthernet4/0/0")
    lines = []
    for index, port in enumerate(ports, 1):
        rx, tx, drops = counters[port]
        lines.extend((f"{port} {index} up 9000/0/0/0 rx packets {rx}",
                      f"  tx packets {tx}", f"  drops {drops}"))
    fib = """buckets:2
      path:[1] ip4 oper-flags:resolved,
        10.20.0.2 GigabitEthernet1/0/0
      path:[2] ip4 oper-flags:resolved,
        10.30.0.2 GigabitEthernet2/0/0
 forwarding:
    ipv4 via 10.20.0.2 GigabitEthernet1/0/0
    ipv4 via 10.30.0.2 GigabitEthernet2/0/0
"""
    return {
        "status": "PASS", "phase": phase,
        "scope": "read-only R3 interface counters and FIB", "pid": "300",
        "captured_unix_time": 100 if phase == "before" else 200,
        "vpp_cpu_sample": {"pid": "300", "start_ticks": "900",
                           "cpu_ticks": 100 if phase == "before" else 200,
                           "clock_ticks_per_second": 100,
                           "monotonic": 10.0 if phase == "before" else 20.0},
        "fib": fib, "interfaces": "\n".join(lines),
        "hardware_p1": "Ethernet address 00:1f:7a:69:f7:4c",
        "hardware_p2": "Ethernet address 00:1f:7a:69:f7:4d",
    }


def receiver(role, phase, rx, timestamp):
    mac = "00:1f:7a:40:28:20" if role == "R1" else "00:1f:7a:40:01:80"
    return {"status": "PASS", "role": role, "phase": phase,
            "scope": "read-only independent receiver NIC counters", "mac": mac,
            "interface": "GigabitEthernet1/0/0", "pid": "400" if role == "R1" else "500",
            "captured_unix_time": timestamp, "rx_good_packets": rx,
            "tx_good_packets": rx, "link": "1 Gbps full duplex"}


class PhysicalUDPWindowTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        ports_before = {"GigabitEthernet1/0/0": (100, 100, 2),
                        "GigabitEthernet2/0/0": (100, 100, 3),
                        "GigabitEthernet4/0/0": (200, 200, 1)}
        ports_after = {"GigabitEthernet1/0/0": (101, 1124, 2),
                       "GigabitEthernet2/0/0": (101, 2148, 3),
                       "GigabitEthernet4/0/0": (3272, 200, 1)}
        self.values = {
            "source": source_result(),
            "r3_before": r3_snapshot("before", ports_before),
            "r3_after": r3_snapshot("after", ports_after),
            "r1_before": receiver("R1", "before", 1000, 10),
            "r1_after": receiver("R1", "after", 3048, 20),
            "r4_before": receiver("R4", "before", 2000, 10),
            "r4_after": receiver("R4", "after", 3024, 20),
        }
        self.paths = {}
        for key, value in self.values.items():
            path = self.root / (key + ".json")
            path.write_text(json.dumps(value))
            self.paths[key] = path

    def tearDown(self):
        self.temp.cleanup()

    def test_complete_end_to_end_ecmp_window_passes_but_perf_remains_open(self):
        result = verify_window(self.paths)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["generated_frames"], 3072)
        self.assertEqual(result["receiver_packets_rx"], 3072)
        self.assertEqual(result["r3_counter_deltas"]["GigabitEthernet1/0/0"]["tx packets"], 1024)
        self.assertEqual(result["r3_counter_deltas"]["GigabitEthernet2/0/0"]["tx packets"], 2048)
        self.assertEqual(result["phase_rates"][0]["vpp_cpu_pct_one_core"], 10.0)
        self.assertEqual(result["performance_status"], "ENVIRONMENT-OPEN")
        self.assertEqual(result["r3_vpp_cpu_pct_one_core"], 10.0)

    def test_r3_cpu_identity_change_is_rejected(self):
        self.values["r3_after"]["vpp_cpu_sample"]["start_ticks"] = "changed"
        self.paths["r3_after"].write_text(json.dumps(self.values["r3_after"]))
        with self.assertRaisesRegex(WindowError, "CPU sample identity"):
            verify_window(self.paths)

    def test_r3_cpu_percentage_is_not_capped_at_one_hundred(self):
        self.values["r3_after"]["vpp_cpu_sample"]["cpu_ticks"] = 1300
        self.values["r3_after"]["vpp_cpu_sample"]["monotonic"] = 20.0
        self.paths["r3_after"].write_text(json.dumps(self.values["r3_after"]))
        result = verify_window(self.paths)
        self.assertEqual(result["r3_vpp_cpu_pct_one_core"], 120.0)

    def test_partial_r3_cpu_interval_is_rejected(self):
        del self.values["r3_after"]["vpp_cpu_sample"]
        self.paths["r3_after"].write_text(json.dumps(self.values["r3_after"]))
        with self.assertRaisesRegex(WindowError, "CPU samples are incomplete"):
            verify_window(self.paths)

    def test_r2_r3_window_can_be_verified_without_claiming_receiver_delivery(self):
        result = verify_window({key: self.paths[key]
                                for key in ("source", "r3_before", "r3_after")})
        self.assertEqual(result["status"], "PASS")
        self.assertIsNone(result["receiver_packets_rx"])
        self.assertIn("R2-to-R3", result["scope"])

    def test_receiver_mismatch_is_rejected(self):
        self.values["r1_after"]["rx_good_packets"] += 1
        self.paths["r1_after"].write_text(json.dumps(self.values["r1_after"]))
        with self.assertRaisesRegex(WindowError, "independent receiver RX total"):
            verify_window(self.paths)

    def test_missing_ecmp_egress_packets_are_rejected(self):
        self.values["r3_after"]["interfaces"] = self.values["r3_after"]["interfaces"].replace(
            "tx packets 1124", "tx packets 1123", 1)
        self.paths["r3_after"].write_text(json.dumps(self.values["r3_after"]))
        with self.assertRaisesRegex(WindowError, "aggregate ECMP egress"):
            verify_window(self.paths)


if __name__ == "__main__":
    unittest.main(verbosity=2)
