#!/usr/bin/env python3
import unittest
from collect_vpp_cpu import cpu_percent, parse_stat


class CpuSampleTests(unittest.TestCase):
    def test_stat_with_parentheses_and_spaces(self):
        fields = ["S"] + ["0"] * 19
        fields[11], fields[12], fields[19] = "20", "30", "1234"
        self.assertEqual(parse_stat("42 (vpp (main)) " + " ".join(fields)),
                         (42, 1234, 50))

    def test_single_cpu_basis(self):
        self.assertEqual(cpu_percent((42, 1, 0), (42, 1, 100), 2, 100), 50)

    def test_multicore_usage_is_not_clamped(self):
        self.assertEqual(cpu_percent((42, 1, 0), (42, 1, 400), 2, 100), 200)

    def test_restart_and_counter_reset_rejected(self):
        for last in ((43, 1, 100), (42, 2, 100), (42, 1, 0)):
            with self.subTest(last=last), self.assertRaises(ValueError):
                cpu_percent((42, 1, 10), last, 1, 100)

    def test_invalid_clock_rejected(self):
        for elapsed in (0, -1, float("nan"), float("inf")):
            with self.subTest(elapsed=elapsed), self.assertRaises(ValueError):
                cpu_percent((42, 1, 0), (42, 1, 10), elapsed, 100)


if __name__ == "__main__":
    unittest.main()
