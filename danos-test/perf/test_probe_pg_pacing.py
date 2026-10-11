import unittest
from probe_pg_pacing import stream_counts


class PacingTests(unittest.TestCase):
    def test_exact_counts_and_enabled_state(self):
        self.assertEqual(stream_counts("pace1000_0 No 256 limit 256, rate 1e3\n",
                                       ["pace1000_0"]),
                         {"pace1000_0": dict(enabled=False, count=256)})

    def test_missing_or_truncated_rows_are_rejected(self):
        for raw in ("no streams currently defined", "pace100 No 256 limit 256"):
            with self.assertRaises(ValueError):
                stream_counts(raw, ["pace1000_0"])


if __name__ == "__main__":
    unittest.main()
