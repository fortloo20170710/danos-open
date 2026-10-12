#!/usr/bin/env python3
import unittest

from probe_r3_ecmp_failover_serial import P1, P2, R1_GW, R4_GW, verify_fib


def fib(buckets, paths, forwarding_paths=None, unresolved=()):
    entries = "\n".join(
        f"      path:[{index}] pl-index:25 ip4 weight=1 pref=0 attached-nexthop: "
        f"oper-flags:{state},\n        {gateway} {interface}"
        for index, (gateway, interface, state) in enumerate(paths))
    if forwarding_paths is None:
        forwarding_paths = [(gateway, interface) for gateway, interface, state in paths
                            if state == "resolved"]
    forwarding = "\n".join(
        f" [@{index}]: ipv4 via {gateway} {interface}"
        for index, (gateway, interface) in enumerate(forwarding_paths))
    forwarding += "\n".join(
        f" [@{index + len(forwarding_paths)}]: arp-ipv4: via {gateway} {interface}"
        for index, (gateway, interface) in enumerate(unresolved))
    return (f"30.30.30.0/24\n path-list:[25] len:{len(paths)}\n{entries}\n"
            f" forwarding: unicast-ip4-chain\n"
            f" [@0]: dpo-load-balance: [proto:ip4 index:20 buckets:{buckets}]\n"
            f"{forwarding}")


class FibEvidenceTests(unittest.TestCase):
    def test_two_resolved_paths(self):
        raw = fib(2, [(R4_GW, P1, "resolved"), (R1_GW, P2, "resolved")])
        paths = verify_fib(raw, {(R4_GW, P1), (R1_GW, P2)})
        self.assertEqual(len(paths), 2)

    def test_one_resolved_path_after_withdrawal(self):
        raw = fib(1, [(R4_GW, P1, "resolved")])
        self.assertEqual(verify_fib(raw, {(R4_GW, P1)})[0]["gateway"], R4_GW)

    def test_rejects_unresolved_forwarding_dpo_even_if_path_list_says_resolved(self):
        raw = fib(2, [(R4_GW, P1, "resolved"), (R1_GW, P2, "resolved")],
                  forwarding_paths=[(R4_GW, P1)], unresolved=[(R1_GW, P2)])
        for expected in ({(R4_GW, P1), (R1_GW, P2)}, {(R4_GW, P1)}):
            with self.subTest(expected=expected), self.assertRaises(ValueError):
                verify_fib(raw, expected)


if __name__ == "__main__":
    unittest.main()
