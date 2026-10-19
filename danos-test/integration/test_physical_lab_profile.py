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

    def test_management_p3_is_never_disabled(self):
        for role in ("R1", "R2", "R3", "R4"):
            with self.subTest(role=role):
                text = render(role)
                self.assertIn("set interface state GigabitEthernet3/0/0 up", text)
                self.assertNotIn("set interface state GigabitEthernet3/0/0 down", text)

    def test_only_unused_non_management_ports_are_disabled(self):
        expected_down = {
            "R1": {2, 4},
            "R2": {2, 4},
            "R3": (),
            "R4": {2, 4},
        }
        for role, ports in expected_down.items():
            with self.subTest(role=role):
                text = render(role)
                actual = {port for port in range(1, 5)
                          if f"set interface state GigabitEthernet{port}/0/0 down" in text}
                self.assertEqual(actual, set(ports))

    def test_no_implicit_host_mutations(self):
        for role in ("R1", "R2", "R3", "R4"):
            text = render(role)
            self.assertNotIn("reboot", text)
            self.assertNotIn("vfio", text)
            self.assertNotIn("delete", text)


if __name__ == "__main__":
    unittest.main()
