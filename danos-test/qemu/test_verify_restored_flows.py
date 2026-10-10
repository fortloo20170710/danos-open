import unittest
from verify_restored_flows import verify

LINES = [f'VPP-ECMP-PATH-UP-FLOW target=30.30.30.{i} tx=5 rx=5 loss=0\n'
         for i in range(2, 6)]


class RestoredFlowsTests(unittest.TestCase):
    def test_all_destinations(self):
        verify(''.join(LINES).replace('\n', '\r\n'))

    def test_reject_incomplete_or_contradictory_evidence(self):
        variants = [LINES[0], ''.join(LINES[:-1]), ''.join(LINES + [LINES[0]]),
                    ''.join(LINES[:-1] + [LINES[0]]),
                    ''.join(LINES).replace('rx=5', 'rx=4', 1),
                    ''.join(LINES).replace('30.30.30.5', '30.30.30.6'),
                    ''.join(LINES) + 'VPP-ECMP-PATH-UP-FLOW-FAIL target=30.30.30.2\n',
                    ''.join(LINES) + 'VPP-ECMP-PATH-UP-FAIL\n']
        for text in variants:
            with self.subTest(text=text), self.assertRaises(ValueError):
                verify(text)


if __name__ == '__main__':
    unittest.main()
