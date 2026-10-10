#!/usr/bin/env python3
import unittest
from physical_lab_profile import render


class ProfileTests(unittest.TestCase):
    def test_reject_unknown_role(self):
        with self.assertRaises(ValueError):
            render("auto")

    def test_explicit_ecmp(self):
        text = render("R3")
        self.assertIn("10.20.0.1/24", text)
        self.assertIn("10.30.0.1/24", text)
        self.assertIn("10.10.0.1/24", text)
        self.assertEqual(text.count("ip route add"), 1)
        self.assertIn("via 10.30.0.2 GigabitEthernet2/0/0", text)
        self.assertNotIn("loopback", text)

    def test_peer_addresses_and_return(self):
        for role, subnet in (("R1", "10.30.0"), ("R4", "10.20.0")):
            text = render(role)
            self.assertIn(f"{subnet}.2/24", text)
            self.assertIn(f"10.10.0.2/32 via {subnet}.1", text)
            for i in range(2, 6):
                self.assertIn(f"loop0 30.30.30.{i}/32", text)

    def test_ingress(self):
        text = render("R2")
        self.assertIn("30.30.30.0/24 via 10.10.0.1", text)
        self.assertNotIn("loopback", text)

    def test_no_implicit_host_mutations(self):
        for role in ("R1", "R2", "R3", "R4"):
            text = render(role)
            self.assertNotIn("reboot", text)
            self.assertNotIn("vfio", text)
            self.assertNotIn("delete", text)


if __name__ == "__main__":
    unittest.main()
