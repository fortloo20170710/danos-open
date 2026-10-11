import unittest
from collect_vpp_window import parse_interfaces, rate_bounds, window_result


class WindowTests(unittest.TestCase):
    def test_failed_window_retains_snapshots_without_rates(self):
        first = dict(clock=[0, 1], ports={"p": {"rx packets": 10}}, raw="before")
        last = dict(clock=[11, 12], ports={"p": {"rx packets": 20, "tx-error": 3}}, raw="after")
        result = window_result(first, last, {"pid": 1}, {"pid": 1})
        self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["after"], last)
        self.assertNotIn("rates_per_second", result)

    def test_restart_and_counter_reset_keep_failed_evidence(self):
        first = dict(clock=[0, 1], ports={"p": {"rx packets": 10}})
        last = dict(clock=[11, 12], ports={"p": {"rx packets": 9}})
        self.assertEqual(window_result(first, last, 1, 1)["status"], "FAIL")
        last["ports"]["p"]["rx packets"] = 20
        self.assertEqual(window_result(first, last, 1, 2)["status"], "FAIL")
        self.assertEqual(window_result(first, last, 1, 1)["status"], "PASS")

    def test_nonphysical_rows_cannot_overwrite_port(self):
        ports = parse_interfaces("GigabitEthernet1/0/0 1 up 9000/0/0/0 rx packets 10\n"
                                 " drops 2\nlocal0 0 down 0/0/0/0 drops 99\n")
        self.assertEqual(ports["GigabitEthernet1/0/0"], {"rx packets": 10, "drops": 2})

    def test_bracketed_rates(self):
        self.assertEqual(rate_bounds(120, (0, 1), (11, 12)), dict(lower=10, upper=12))

    def test_tx_errors_are_not_hidden_by_unchanged_drops(self):
        ports = parse_interfaces("GigabitEthernet2/0/0 2 up 9000/0/0/0 rx packets 1\n"
                                 " drops 2\n tx-error 183\n rx-error 4\n")
        self.assertEqual(ports["GigabitEthernet2/0/0"]["tx-error"], 183)
        self.assertEqual(ports["GigabitEthernet2/0/0"]["rx-error"], 4)

    def test_reject_resets_bad_clocks_and_missing_ports(self):
        for delta, before, after in [(-1, (0, 1), (2, 3)), (1, (0, 2), (1, 3)),
                                     (1, (0, 1), (2, float("nan")))]:
            with self.assertRaises(ValueError):
                rate_bounds(delta, before, after)
        with self.assertRaises(ValueError):
            parse_interfaces("show interface: unknown input")


if __name__ == "__main__":
    unittest.main()
