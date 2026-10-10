#!/usr/bin/env python3
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HOOK = Path(__file__).with_name('replay_physical_lab.sh')


class ReplayTests(unittest.TestCase):
    def run_hook(self, profile, output='', code=0):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            client = root / 'client'
            client.write_text('#!/bin/sh\nprintf "%s" "$MOCK_OUTPUT"\nexit "$MOCK_CODE"\n')
            client.chmod(0o700)
            path = root / 'profile'
            if profile is not None:
                path.write_text(profile)
            return subprocess.run(['sh', str(HOOK), str(path)], capture_output=True,
                                  env=dict(os.environ, DANOS_LAB_VPPCTL=str(client),
                                           MOCK_OUTPUT=output, MOCK_CODE=str(code)))

    def test_missing_opt_in(self):
        self.assertEqual(self.run_hook(None).returncode, 0)

    def test_success(self):
        self.assertEqual(self.run_hook('set interface state GigabitEthernet1/0/0 up\n').returncode, 0)

    def test_zero_exit_cli_error_rejected(self):
        self.assertNotEqual(self.run_hook('ip route add 10.0.0.0/24 via 10.1.0.1\n', 'unknown input').returncode, 0)

    def test_process_error(self):
        self.assertNotEqual(self.run_hook('ip route add 10.0.0.0/24 via 10.1.0.1\n', code=1).returncode, 0)

    def test_loopback_output(self):
        self.assertEqual(self.run_hook('create loopback interface instance 0\n', 'loop0\n').returncode, 0)

    def test_reject_other_actions(self):
        self.assertNotEqual(self.run_hook('quit\n').returncode, 0)


if __name__ == '__main__':
    unittest.main()
