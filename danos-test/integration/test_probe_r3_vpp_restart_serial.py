import unittest

from probe_r3_vpp_restart_serial import (
    OBSERVED_OPTIONAL_PORTS,
    TRANSIT_PORTS,
    required_link_failures,
    remote_required_link_gate_source,
)


def hardware(link):
    if link:
        return "Link speed: 1 Gbps\ncarrier up full duplex"
    return "Link speed: unknown\ncarrier down"


class R3RestartLinkGateTests(unittest.TestCase):
    def test_only_transit_ports_gate_restart_acceptance(self):
        self.assertEqual(TRANSIT_PORTS, (1, 2))
        self.assertEqual(OBSERVED_OPTIONAL_PORTS, (4,))
        states = {"1": hardware(True), "2": hardware(True), "4": hardware(False)}
        self.assertEqual(required_link_failures(states), [])

    def test_missing_transit_carrier_still_fails(self):
        states = {"1": hardware(True), "2": hardware(False), "4": hardware(False)}
        self.assertEqual(required_link_failures(states), [2])

    def test_generated_remote_gate_executes_without_local_symbols(self):
        namespace = {"result": {"hardware_after": {"1": hardware(True),
                                                     "2": hardware(True),
                                                     "4": hardware(False)}}}
        exec(remote_required_link_gate_source(), namespace)
        self.assertEqual(namespace["failed_links"], [])


if __name__ == "__main__":
    unittest.main()
