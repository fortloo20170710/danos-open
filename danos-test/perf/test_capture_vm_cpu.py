#!/usr/bin/env python3
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import capture_vm_cpu as capture


class CaptureTests(unittest.TestCase):
    def test_executable_is_vm_only(self):
        capture.check_executable(Path('/usr/bin/qemu-system-x86_64'), 'qemu-e1000')
        capture.check_executable(Path('/usr/lib/vmware-vmx'), 'vmware-vmxnet3')
        for name, lane in [('vpp', 'qemu-e1000'), ('python3', 'vmware-vmxnet3'),
                           ('vmware-vmx', 'qemu-e1000')]:
            with self.assertRaises(ValueError):
                capture.check_executable(Path(name), lane)

    def test_real_command_failure_and_timeout(self):
        with tempfile.TemporaryFile() as log:
            self.assertEqual(capture.run_command([sys.executable, '-c', 'raise SystemExit(3)'], 5, log), 3)
            with self.assertRaises(subprocess.TimeoutExpired):
                capture.run_command([sys.executable, '-c', 'import time; time.sleep(10)'], .05, log)

    def test_window_and_restart_rejection(self):
        for last, expected in [((42, 9, 300), 0), ((42, 10, 300), 1)]:
            with self.subTest(last=last), tempfile.TemporaryDirectory() as temp:
                output = Path(temp) / 'cpu.json'
                with patch('sys.argv', ['capture', '--pid', '42', '--lane', 'qemu-e1000',
                                       '--out', str(output), '--', 'traffic']), \
                     patch.object(Path, 'resolve', return_value=Path('/usr/bin/qemu-system-x86_64')), \
                     patch.object(Path, 'read_text', return_value='stat'), \
                     patch.object(capture, 'parse_stat', side_effect=[(42, 9, 100), last]), \
                     patch.object(capture.os, 'sysconf', return_value=100), \
                     patch.object(capture.time, 'monotonic', side_effect=[10, 12]), \
                     patch.object(capture, 'run_command', return_value=0), patch('builtins.print'):
                    self.assertEqual(capture.main(), expected)
                result = json.loads(output.read_text())
                self.assertEqual(result['scope'], 'host-vm-command-window')
                self.assertEqual(result['performance_status'], 'ENVIRONMENT-OPEN')
                if expected == 0:
                    self.assertEqual(result['cpu_pct'], 100)
                else:
                    self.assertNotIn('cpu_pct', result)


if __name__ == '__main__':
    unittest.main()
