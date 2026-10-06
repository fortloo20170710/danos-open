import unittest
from verify_frr_recovery import validate, validate_vpp_packets


class RecoveryTest(unittest.TestCase):
    frr = "FRR-RESTART-BEGIN\nFRR-RESTART-REQUEST rc=0\nFRR-RESTART-TEST PASS\n"
    bridge = ("zapi peer EOF while reading header\nzebra session reconnected\n"
              "VPP backend recovery: replay attempted=4 failed=0\n"
              "zapi command=31 vrf=0\nvpp route add vrf=0\n")

    def test_valid(self):
        validate(self.frr, self.bridge)

    def test_vpp_packet_recovery_order(self):
        begin = "VPP-RESTART-TEST BEGIN\n"
        probes = ("VPP-RESTART-PEER PASS address=10.10.0.2 packets=3 loss=0\n"
                  "VPP-RESTART-PEER PASS address=10.20.0.2 packets=3 loss=0\n")
        done = "VPP-RESTART-TEST PASS\n"
        valid = begin + probes + done
        validate_vpp_packets(valid)
        for log in (probes + begin + done, begin + done + probes,
                    valid + begin, valid + "VPP-RESTART-PEER FAIL address=10.10.0.2\n",
                    valid.replace("packets=3", "packets=0"), valid.replace("loss=0", "loss=1")):
            with self.subTest(log=log), self.assertRaises(ValueError):
                validate_vpp_packets(log)

    def test_reject_incomplete_or_failed(self):
        cases = [
            (self.frr.replace("rc=0", "rc=1"), self.bridge),
            ("FRR-RESTART-TEST PASS\n", self.bridge),
            (self.frr, self.bridge.replace("zapi peer EOF", "idle")),
            (self.frr, self.bridge.replace("zebra session reconnected", "connecting")),
            (self.frr, self.bridge.replace("attempted=4", "attempted=0")),
            (self.frr, self.bridge.replace("command=31", "command=1")),
            (self.frr, self.bridge + "zapi peer EOF\n"),
            (self.frr + "FRR-RESTART-TEST FAIL\n", self.bridge),
            (self.frr, self.bridge + "ZAPI command 31 programming failed: failed=1\n"),
        ]
        for frr, bridge in cases:
            with self.subTest(frr=frr, bridge=bridge), self.assertRaises(ValueError):
                validate(frr, bridge)


if __name__ == "__main__":
    unittest.main()
