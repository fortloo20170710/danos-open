#!/usr/bin/env python3
"""Tests for UART counter parsing; no hardware required."""
import unittest

from capture_vpp_transit_window import counters


class CounterTests(unittest.TestCase):
    def test_two_ports(self):
        raw = (b"GigabitEthernet1/0/0 1 up 9000/0/0/0 rx packets 100\r\n"
               b" tx packets 99\r\n drops 1\r\n"
               b"GigabitEthernet4/0/0 4 down 9000/0/0/0\r\n")
        self.assertEqual(counters(raw), {
            "GigabitEthernet1/0/0": {"rx": 100, "tx": 99, "drops": 1},
            "GigabitEthernet4/0/0": {"rx": 0, "tx": 0, "drops": 0},
        })

    def test_command_echo_not_counter(self):
        self.assertEqual(counters(b"echo show interface; echo END\r\n"), {})

    def test_error_line_not_port_header(self):
        self.assertEqual(counters(b"4 GigabitEthernet4/0/0-output interface is down error\n"), {})

    def test_corrupt_prefix(self):
        self.assertEqual(counters(b"\xff\x00\r\nGigabitEthernet1/0/0 1 up 9000/0/0/0 tx packets 42\n")
                         ["GigabitEthernet1/0/0"]["tx"], 42)


if __name__ == "__main__":
    unittest.main()
