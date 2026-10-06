import unittest
from verify_learned_route_fib import validate


class LearnedFibTest(unittest.TestCase):
    prefix = "198.18.0.0/24"
    gateway = "172.31.0.3"
    interface = "GigabitEthernet0/3/0"
    good = ("198.18.0.0/24 fib:0 index:19 locks:2\n API refs:1\n"
            " forwarding: unicast-ip4-chain\n"
            " [0] [@5]: ipv4 via 172.31.0.3 GigabitEthernet0/3/0: mtu:9000\n")

    def check(self, text):
        validate(text, self.prefix, self.gateway, self.interface)

    def test_resolved(self):
        self.check(self.good)

    def test_reject_unresolved_wrong_or_unowned(self):
        for text in (self.good.replace("ipv4 via", "arp-ipv4: via"),
                     self.good.replace("ipv4 via", "dpo-drop ipv4 via"),
                     self.good.replace("API refs:1", "CLI refs:1"),
                     self.good.replace("172.31.0.3", "10.20.0.2"),
                     self.good.replace("GigabitEthernet0/3/0", "GigabitEthernet0/2/0"),
                     self.good.replace("forwarding:", "paths:"),
                     self.good.replace("198.18.0.0/24", "198.19.0.0/24")):
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.check(text)

    def test_latest_entry_wins(self):
        with self.assertRaises(ValueError):
            self.check(self.good + self.good.replace("ipv4 via", "arp-ipv4: via"))

    def test_other_route_cannot_supply_adjacency(self):
        with self.assertRaises(ValueError):
            self.check(self.good.replace("ipv4 via", "arp-ipv4: via") +
                       self.good.replace("198.18.0.0/24", "198.19.0.0/24"))


if __name__ == "__main__":
    unittest.main()
