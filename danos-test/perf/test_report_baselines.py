#!/usr/bin/env python3
import tempfile
import unittest
from pathlib import Path
from report_baselines import baseline

SAMPLE = '''status=PASS
lane=qemu-e1000
packets_tx=4000
packets_rx=4000
duration_ms=50000
pps=80
packet_size_bytes=''
mbps=''
rtt_p50_us=400
rtt_p99_us=3500
cpu_pct=''
ecmp_bucket_0=3000
ecmp_bucket_1=1000
'''


class Baselines(unittest.TestCase):
    def read(self, data):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'sample.env'
            path.write_text(data)
            return baseline(path)

    def test_missing_is_not_zero_or_pass(self):
        result = self.read(SAMPLE)
        self.assertIsNone(result['cpu_pct'])
        self.assertIsNone(result['mac_frame_mbps'])
        self.assertFalse(result['physical_performance_accepted'])
        self.assertEqual(result['ecmp_bucket_share_pct'], [75, 25])
        self.assertEqual(result['ecmp_counter_coverage_pct'], 100)

    def test_mac_throughput_and_partial_bucket_coverage(self):
        result = self.read(SAMPLE.replace("packet_size_bytes=''", 'packet_size_bytes=98')
                           .replace('ecmp_bucket_0=3000', 'ecmp_bucket_0=9'))
        self.assertAlmostEqual(result['mac_frame_mbps'], 0.06272)
        self.assertAlmostEqual(result['ecmp_counter_coverage_pct'], 25.225)

    def test_missing_lane_evidence_is_open_not_pass_or_fail(self):
        result = self.read('status=ENVIRONMENT-OPEN\nlane=vmware-vmxnet3-polling\n'
                           "reason='original ISO unavailable'\n")
        self.assertEqual(result['status'], 'ENVIRONMENT-OPEN')
        self.assertEqual(result['missing'], ['original ISO unavailable'])
        self.assertIsNone(result['packets_tx'])
        self.assertFalse(result['physical_performance_accepted'])

    def test_open_lane_requires_reason(self):
        with self.assertRaisesRegex(ValueError, 'requires a reason'):
            self.read('status=ENVIRONMENT-OPEN\nlane=vmware-vmxnet3-polling\n')

    def test_reject_inconsistent_nonfinite_and_duplicate_data(self):
        for data in (SAMPLE.replace('pps=80', 'pps=nan'), SAMPLE.replace('pps=80', 'pps=100'),
                     SAMPLE.replace('packets_rx=4000', 'packets_rx=4001'),
                     SAMPLE.replace('ecmp_bucket_0=3000', 'ecmp_bucket_0=4000'),
                     SAMPLE + 'status=PASS\n', SAMPLE.replace('status=PASS', 'status=FAIL')):
            with self.subTest(data=data), self.assertRaises(ValueError):
                self.read(data)


if __name__ == '__main__':
    unittest.main()
