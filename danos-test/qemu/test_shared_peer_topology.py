"""Check launcher wiring only; fake QEMU calls are never runtime acceptance."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class SharedPeerTopologyTest(unittest.TestCase):
    def launch(self, mode, seed_mode=None, dirty='0'):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ('danos.iso', 'frr-1.qcow2', 'frr-2.qcow2', 'r1-seed.iso', 'r2-seed.iso'):
                (root / name).touch()
            (root / 'topology-profile.env').write_text(f'frr_shared_peer_l2={seed_mode or mode}\n')
            # Mock only the external ISO reader invocation. It returns a fixture
            # source identity distinct from runner HEAD, not real ISO evidence.
            wrapper = root / 'python3'
            wrapper.write_text(f'#!{sys.executable}\nimport os,sys\n'
                               'if sys.argv[1].endswith("/read_live_iso_identity.py"):\n'
                               f' print("iso_build_commit={"b" * 40}\\niso_source_dirty={dirty}")\n'
                               'else:\n'
                               f' os.execv({sys.executable!r},[{sys.executable!r},*sys.argv[1:]])\n')
            wrapper.chmod(0o755)
            stub = root / 'qemu-system-x86_64'
            stub.write_text('#!/usr/bin/env python3\nimport json,os,sys\n'
                            'with open(os.environ["DANOS_QEMU_TEST_ARGS"],"a") as out:\n'
                            ' out.write(json.dumps(sys.argv[1:])+"\\n")\n')
            stub.chmod(0o755)
            env = dict(os.environ, PATH=f'{root}:{os.environ["PATH"]}',
                       DANOS_QEMU_TEST_ARGS=str(root / 'args'),
                       QEMU_TOPOLOGY_DIR=str(root), DANOS_ISO=str(root / 'danos.iso'),
                       FRR_SHARED_PEER_L2=mode, FRR_PEER_PORT='32001',
                       FRR_PEER2_PORT='32002', QEMU_CAPTURE='0', QEMU_MONITOR='0')
            script = Path(__file__).with_name('run_frr_vpp_topology.sh')
            proc = subprocess.run(['bash', str(script)], env=env,
                                  capture_output=True, text=True, timeout=10)
            args = [json.loads(line) for line in (root / 'args').read_text().splitlines()] \
                if (root / 'args').exists() else []
            manifest = (root / 'run-manifest.env').read_text() if (root / 'run-manifest.env').exists() else ''
            return proc, args, manifest

    def test_original_wiring_is_default(self):
        proc, args, manifest = self.launch('0')
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(len(args), 3)
        self.assertFalse(any('hubport' in arg for call in args for arg in call))
        self.assertIn('socket,id=peer,listen=127.0.0.1:32001', args[1])
        self.assertIn('socket,id=peer,connect=127.0.0.1:32001', args[2])
        self.assertIn('frr_shared_peer_l2=0', manifest)

    def test_shared_segment_is_loopback_only(self):
        proc, args, manifest = self.launch('1')
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(len(args), 3)
        for value in ('hubport,id=lan2-backend,hubid=1,netdev=lan2',
                      'hubport,id=peer1-backend,hubid=1,netdev=peer1',
                      'hubport,id=peer2-backend,hubid=1,netdev=peer2',
                      'hubport,id=lan2-nic,hubid=1'):
            self.assertIn(value, args[0])
        self.assertIn('socket,id=peer,connect=127.0.0.1:32001', args[1])
        self.assertIn('socket,id=peer,connect=127.0.0.1:32002', args[2])
        self.assertTrue(any('netdev=lan2-nic,addr=0x3' in arg for arg in args[0]))
        self.assertIn('frr_shared_peer_l2=1', manifest)
        self.assertIn(f'git_commit={"b" * 40}', manifest)
        self.assertIn('runner_commit=', manifest)

    def test_invalid_mode_does_not_start_guests(self):
        proc, args, _ = self.launch('invalid')
        self.assertNotEqual(proc.returncode, 0)
        self.assertEqual(args, [])

    def test_mismatched_seed_does_not_start_guests(self):
        proc, args, _ = self.launch('1', '0')
        self.assertNotEqual(proc.returncode, 0)
        self.assertEqual(args, [])

    def test_dirty_iso_does_not_start_guests(self):
        proc, args, _ = self.launch('1', dirty='1')
        self.assertNotEqual(proc.returncode, 0)
        self.assertEqual(args, [])


if __name__ == '__main__':
    unittest.main()
