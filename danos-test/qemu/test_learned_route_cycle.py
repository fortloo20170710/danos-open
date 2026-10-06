import unittest
from run_learned_route_cycle import validate_cycle, validate_phase


class LearnedCycleTest(unittest.TestCase):
    good = ('116 bytes from 198.19.0.1: icmp_seq=1\n'
            '116 bytes from 198.19.0.1: icmp_seq=2\n'
            '116 bytes from 198.19.0.1: icmp_seq=3\n'
            'Statistics: 3 sent, 3 received, 0% packet loss\n'
            '198.19.0.0/24 fib:0 index:22\n API refs:1\n forwarding:\n'
            ' ipv4 via 172.31.0.3 GigabitEthernet0/3/0: mtu:9000\n')
    negative = 'Statistics: 3 sent, 0 received, 100% packet loss\n'
    peer = 'bgp-add-rc=0\nbgp-withdraw-rc=0\nbgp-restore-rc=0\n'

    def cycle(self):
        log = 'VPP-RESTART-TEST PASS\n'
        for stage in ('ADD', 'WITHDRAW', 'RESTORE', 'POSTFRR'):
            if stage == 'POSTFRR':
                log += 'zebra session reconnected\n'
            log += f'FRR-LEARNED-{stage}-BEGIN\n'
            log += self.negative if stage == 'WITHDRAW' else self.good
            log += f'FRR-LEARNED-{stage}-END\n'
        return log

    def test_valid(self):
        validate_cycle(self.cycle(), self.peer)

    def test_reject_zero_packets_or_wrong_target(self):
        for block, withdrawn in ((self.negative.replace('3 sent', '0 sent'), True),
                                 (self.good.replace('198.19.0.1:', '198.18.0.1:'), False),
                                 (self.good.replace('ipv4 via', 'arp-ipv4: via'), False),
                                 (self.good + self.good, False),
                                 (self.negative + self.good, True)):
            with self.subTest(block=block), self.assertRaises(ValueError):
                validate_phase(block, withdrawn)

    def test_reject_incomplete_stale_or_unordered(self):
        for log in (self.cycle().replace('zebra session reconnected', 'connecting'),
                    self.cycle().replace('VPP-RESTART-TEST PASS', 'pending'),
                    self.cycle().replace('FRR-LEARNED-ADD-BEGIN', 'OLD-ADD-BEGIN'),
                    self.cycle() + self.cycle(),
                    self.cycle() + 'VPP-RESTART-TEST PASS\n'):
            with self.subTest(log=log), self.assertRaises(ValueError):
                validate_cycle(log, self.peer)
        with self.assertRaises(ValueError):
            validate_cycle(self.cycle(), self.peer.replace('bgp-restore-rc=0', 'bgp-restore-rc=1'))


if __name__ == '__main__':
    unittest.main()
