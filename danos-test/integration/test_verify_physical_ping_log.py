#!/usr/bin/env python3
import unittest

from verify_physical_ping_log import verify


LOG = (b"116 bytes from 10.20.0.2: icmp_seq=1 ttl=63 time=.1 ms\r\n"
       b"116 bytes from 10.20.0.2: icmp_seq=2 ttl=63 time=.2 ms\r\n"
       b"\r\nStatistics: 2 sent, 2 received, 0% packet loss\r\n")


class EvidenceTests(unittest.TestCase):
    def test_valid(self):
        result = verify(LOG, "10.20.0.2", 2, 63)
        self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["ecmp"], "NOT_TESTED")
        self.assertEqual(result["identity"], "NOT_VERIFIED")

    def test_reject_bad_evidence(self):
        variants = [LOG.replace(b"seq=2", b"seq=1"),
                    LOG.replace(b"ttl=63", b"ttl=64"),
                    LOG.replace(b"from 10.20.0.2", b"from 10.20.0.3"),
                    LOG.replace(b"2 received", b"1 received"),
                    LOG.split(b"Statistics:")[0],
                    LOG + b"Statistics: 2 sent, 2 received, 0% packet loss\n",
                    LOG + b"Destination Host Unreachable\n",
                    LOG + b"116 bytes from malformed response\n"]
        for raw in variants:
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                verify(raw, "10.20.0.2", 2, 63)


if __name__ == "__main__":
    unittest.main()
