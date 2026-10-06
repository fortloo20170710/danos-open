import unittest
from verify_frr_dynamic_log import validate


class DynamicEvidence(unittest.TestCase):
    valid = ("zapi command=31 vrf=0\nzapi command=32 vrf=0\n"
             "programming sweep: withdrawn=1 failed=0\n"
             "fib_live_bridge: processed=2 failed=0\n")

    def test_valid(self):
        validate(self.valid)
        validate(self.valid.replace("command=31", "command=9")
                 .replace("command=32", "command=10"))

    def test_reject_incomplete_or_failed(self):
        for log in (
            self.valid.replace("command=31", "command=1"),
            self.valid.replace("command=32", "command=1"),
            self.valid.replace("withdrawn=1", "withdrawn=0"),
            self.valid.replace("withdrawn=1 failed=0", "withdrawn=1 failed=1"),
            self.valid.replace("processed=2 failed=0", "processed=2 failed=1"),
            self.valid.split("fib_live_bridge:")[0],
            self.valid + "ZAPI command 31 transaction failed: EXISTS\n",
            self.valid + "fib_live_bridge: processed=2 failed=0\n",
        ):
            with self.subTest(log=log), self.assertRaises(ValueError):
                validate(log)


if __name__ == "__main__":
    unittest.main()
