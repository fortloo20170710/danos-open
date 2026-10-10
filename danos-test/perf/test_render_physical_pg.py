#!/usr/bin/env python3
import unittest
from render_physical_pg import render


class PhysicalPgTests(unittest.TestCase):
    def call(self, **overrides):
        values = dict(interface="GigabitEthernet1/0/0", source_mac="00:1f:7a:69:f5:ec",
                      target_mac="00:1f:7a:69:f7:4f", source_ip="10.10.0.2",
                      targets=["30.30.30.2", "30.30.30.3"], rate=100, count=1000)
        values.update(overrides)
        return render(**values)

    def test_explicit_output_bounded_and_disabled(self):
        text = self.call()
        self.assertEqual(text.count("packet-generator new"), 2)
        self.assertEqual(text.count("size 64-64"), 2)
        self.assertEqual(text.count("node GigabitEthernet1/0/0-output"), 2)
        self.assertIn("UDP: 20000 -> 30000", text)
        self.assertIn("UDP: 20001 -> 30000", text)
        self.assertNotIn("enable", text)
        self.assertNotIn("limit 0", text)
        self.assertNotIn("ethernet-input", text)

    def test_invalid_inputs_rejected(self):
        cases = [dict(interface="loop0"), dict(interface="GigabitEthernet1/0/0\nquit"),
                 dict(target_mac="ff:ff:ff:ff:ff:ff"), dict(source_mac="00:00:00:00:00:00"),
                 dict(source_ip="0.0.0.0"), dict(targets=["224.0.0.1", "30.30.30.3"]),
                 dict(targets=["30.30.30.2"]), dict(targets=["30.30.30.2"]*2),
                 dict(rate=0), dict(rate=1000001), dict(count=0), dict(count=10000001)]
        for case in cases:
            with self.subTest(case=case), self.assertRaises(ValueError):
                self.call(**case)


if __name__ == "__main__":
    unittest.main()
