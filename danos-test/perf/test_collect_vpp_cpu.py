#!/usr/bin/env python3
import unittest
import json
import subprocess
import tempfile
from pathlib import Path
from unittest.mock import patch
import collect_vpp_cpu
from collect_vpp_cpu import cpu_percent, parse_stat, parse_threads


class CpuSampleTests(unittest.TestCase):
    def test_command_window_records_exit_and_cpu(self):
        for code in (0, 1):
            with self.subTest(code=code), tempfile.TemporaryDirectory() as temp:
                output = Path(temp) / 'cpu.json'
                cli = subprocess.CompletedProcess([], 0, '0 vpp_main 42 0 other\n', '')
                traffic = subprocess.CompletedProcess([], code, 'packet evidence', '')
                with patch('sys.argv', ['cpu', '--pid', '42', '--duration', '30',
                                       '--out', str(output), '--command', 'traffic', '--count', '100']), \
                     patch.object(Path, 'resolve', return_value=Path('/usr/bin/vpp')), \
                     patch.object(Path, 'read_text', return_value='stat'), \
                     patch.object(collect_vpp_cpu, 'parse_stat', side_effect=[(42, 9, 100), (42, 9, 200)]), \
                     patch.object(collect_vpp_cpu.os, 'sysconf', return_value=100), \
                     patch.object(collect_vpp_cpu.time, 'monotonic', side_effect=[10, 12]), \
                     patch.object(collect_vpp_cpu.subprocess, 'run', side_effect=[cli, traffic, cli]), \
                     patch('builtins.print'):
                    self.assertEqual(collect_vpp_cpu.main(), code)
                result = json.loads(output.read_text())
                self.assertEqual(result['cpu_pct'], 50)
                self.assertEqual(result['scope'], 'cpu-command-window')
                self.assertEqual(result['command_exit_code'], code)
                self.assertEqual(result['performance_status'], 'ENVIRONMENT-OPEN')

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
