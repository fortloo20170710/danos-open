import unittest

from verify_r3_ecmp_counter_window import P1, P2, P4, TARGETS, verify


def fib():
    return """30.30.30.0/24
 path:[1] oper-flags:resolved,
  10.20.0.2 GigabitEthernet1/0/0
 path:[2] oper-flags:resolved,
  10.30.0.2 GigabitEthernet2/0/0
forwarding:
 buckets:2
 ipv4 via 10.20.0.2 GigabitEthernet1/0/0
 ipv4 via 10.30.0.2 GigabitEthernet2/0/0
"""


def interface_table(values):
    lines = []
    for name in (P1, P2, P4):
        rx, tx = values.get(name, (0, 0))
        lines += [f"{name} 1 up 9000/0/0/0 rx packets {rx}",
                  f" rx bytes {rx * 110}", f" tx packets {tx}",
                  f" tx bytes {tx * 110}", " drops 0"]
    return "\n".join(lines)


def snapshot(phase, values):
    return {"status": "PASS", "phase": phase,
            "scope": "read-only R3 interface counters and FIB", "pid": "1576",
            "hardware_p1": "Ethernet address 00:1f:7a:69:f7:4c",
            "hardware_p2": "Ethernet address 00:1f:7a:69:f7:4d",
            "interfaces": interface_table(values), "fib": fib()}


def traffic():
    probes, warm = [], []
    for target in TARGETS:
        probes += [{"target": target, "phase": "warmup", "attempt": 1,
                    "output": "Statistics: 5 sent, 5 received, 0% packet loss"},
                   {"target": target, "phase": "warm",
                    "output": "Statistics: 100 sent, 100 received, 0% packet loss"}]
        warm.append({"target": target, "status": "PASS", "sent": 100,
                     "received": 100, "loss_percent": 0, "reply_ttl": 63})
    return {"status": "PASS", "role": "R2", "expected_mac": "00:1f:7a:69:f5:ec",
            "pid_before": "2097", "pid_after": "2097", "pid_final": "2097",
            "probes": probes, "verified_warm": warm}


class CounterWindowTests(unittest.TestCase):
    def setUp(self):
        self.before = snapshot("before", {P1: (10, 10), P2: (20, 20), P4: (30, 30)})
        self.after = snapshot("after", {P1: (115, 115), P2: (335, 335), P4: (450, 450)})
        self.traffic = traffic()

    def test_exact_ingress_and_two_egress_paths_pass(self):
        result = verify(self.before, self.after, self.traffic)
        self.assertEqual(result["egress_split"], {"P1": 105, "P2": 315})
        self.assertEqual(result["requests"], 420)

    def test_rejects_missing_egress_packets(self):
        self.after["interfaces"] = self.after["interfaces"].replace("tx packets 335", "tx packets 334")
        with self.assertRaisesRegex(ValueError, "egress counters"):
            verify(self.before, self.after, self.traffic)

    def test_rejects_drop_counter_increase(self):
        self.after["interfaces"] = self.after["interfaces"].replace("drops 0", "drops 1", 1)
        with self.assertRaisesRegex(ValueError, "drop counter increased"):
            verify(self.before, self.after, self.traffic)

    def test_rejects_unresolved_or_missing_path(self):
        self.after["fib"] = self.after["fib"].replace("oper-flags:resolved", "oper-flags:unresolved", 1)
        with self.assertRaisesRegex(ValueError, "FIB paths"):
            verify(self.before, self.after, self.traffic)

    def test_rejects_changed_vpp_identity(self):
        self.after["pid"] = "9999"
        with self.assertRaisesRegex(ValueError, "process changed"):
            verify(self.before, self.after, self.traffic)


if __name__ == "__main__":
    unittest.main()
