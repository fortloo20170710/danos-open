#!/usr/bin/env python3
import unittest
from collect_vpp_cpu import cpu_percent, parse_stat, parse_threads


class CpuSampleTests(unittest.TestCase):
    def test_real_thread_listing_with_ansi(self):
        raw = "ID Name LWP CPU Sched\n 0\x1b[0m vpp_main 2312 0 other (0)\n"
        self.assertEqual(parse_threads(raw), [dict(index=0, name="vpp_main", lwp=2312, cpu=0)])

    def test_worker_listing(self):
        self.assertEqual(len(parse_threads(
            "0 vpp_main 42 0 other\n1 vpp_wk_0 43 1 other\n")), 2)

    def test_cli_errors_and_duplicate_threads_rejected(self):
        for raw in ("show threads: unknown input", "1 vpp_wk_0 43 1 other",
                    "0 vpp_main 42 0 other\n0 vpp_wk_0 43 1 other",
                    "0 vpp_main 42 0 other\n1 vpp_wk_0 42 1 other",
                    "0 vpp_main 42 0 other\n1 unknown_thread 43 1 other"):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                parse_threads(raw)

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
