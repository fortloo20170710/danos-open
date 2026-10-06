#!/usr/bin/env python3
"""Run/verify VPP-originated learned-route probes; not transit throughput."""
import argparse
import os
from pathlib import Path
import re
import sys
import time
from verify_learned_route_fib import validate as validate_fib
from verify_topology_identity import validate as validate_identity

STAGES = ('ADD', 'WITHDRAW', 'RESTORE', 'POSTFRR')
PREFIX = '198.19.0.0/24'
TARGET = '198.19.0.1'
INTERFACE = 'GigabitEthernet0/3/0'


def validate_phase(block: str, withdrawn: bool) -> None:
    stats = re.findall(r'Statistics: (\d+) sent, (\d+) received, ([\d.]+)% packet loss', block)
    expected = ('3', '0', '100') if withdrawn else ('3', '3', '0')
    if len(stats) != 1 or tuple(stats[0][:2]) != expected[:2] or float(stats[0][2]) != float(expected[2]):
        raise ValueError('missing exact three-packet phase result')
    if withdrawn:
        if re.search(rf'(?m)^{re.escape(PREFIX)} fib:', block):
            raise ValueError('withdrawn prefix still has a FIB entry')
    else:
        for seq in range(1, 4):
            if not re.search(rf'bytes from {re.escape(TARGET)}: icmp_seq={seq}\b', block):
                raise ValueError('reply target/sequence does not match learned endpoint')
        validate_fib(block, PREFIX, '172.31.0.3', INTERFACE)


def validate_cycle(serial: str, frr2: str) -> None:
    if not re.search(r'bgp-add-rc=0.*bgp-withdraw-rc=0.*bgp-restore-rc=0', frr2, re.S):
        raise ValueError('FRR BGP add/withdraw/restore operations missing or unordered')
    cursor = serial.rfind('VPP-RESTART-TEST PASS')
    if cursor < 0:
        raise ValueError('no VPP restart completion before learned-route phases')
    for stage in STAGES:
        begin, end = f'FRR-LEARNED-{stage}-BEGIN', f'FRR-LEARNED-{stage}-END'
        if serial.count(begin) != 1 or serial.count(end) != 1:
            raise ValueError(f'{stage} phase missing or duplicated')
        first, last = serial.find(begin), serial.find(end)
        if not cursor < first < last:
            raise ValueError('learned-route phase ordering invalid')
        validate_phase(serial[first:last], stage == 'WITHDRAW')
        if stage == 'POSTFRR' and 'zebra session reconnected' not in serial[cursor:first]:
            raise ValueError('no zebra reconnect between restore and final probe')
        cursor = last


def run(topology: Path, timeout: float) -> None:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'live'))
    from verify_usb_keyboard_qemu import send_text
    deadline = time.monotonic() + timeout

    def live():
        for node in ('danos', 'frr-1', 'frr-2'):
            pid = int((topology / f'{node}.pid').read_text())
            os.kill(pid, 0)
            if str(topology).encode() not in Path(f'/proc/{pid}/cmdline').read_bytes():
                raise ValueError('pid does not belong to this topology')
        if time.monotonic() >= deadline:
            raise TimeoutError('learned-route observation deadline elapsed')

    def wait(log, marker):
        while True:
            live()
            if marker in log.read_text(errors='replace'):
                return
            time.sleep(1)

    serial = topology / 'danos.serial.log'
    peer = topology / 'frr-2.serial.log'
    wait(serial, 'LIVE-SHELL-READY')
    validate_identity(topology)
    wait(serial, 'VPP-RESTART-TEST PASS')
    for stage, log, marker in (
            ('ADD', peer, 'bgp-add-rc=0'), ('WITHDRAW', peer, 'bgp-withdraw-rc=0'),
            ('RESTORE', peer, 'bgp-restore-rc=0'),
            ('POSTFRR', topology / 'frr-1.serial.log', 'FRR-RESTART-TEST PASS')):
        wait(log, marker)
        if stage == 'ADD' and 'bgp-withdraw-rc=' in peer.read_text(errors='replace'):
            raise ValueError('add window already ended; do not reuse stale phases')
        begin, end = f'FRR-LEARNED-{stage}-BEGIN', f'FRR-LEARNED-{stage}-END'
        command = (f'echo {begin} >/dev/ttyS0; '
                   f'vppctl -s /run/vpp/cli.sock ping {TARGET} source {INTERFACE} repeat 3 >/dev/ttyS0; '
                   f'vppctl -s /run/vpp/cli.sock show ip fib {PREFIX} >/dev/ttyS0; '
                   f'echo {end} >/dev/ttyS0\n')
        send_text(str(topology / 'danos.monitor'), command)
        wait(serial, end)
        block = serial.read_text(errors='replace').split(begin)[-1].split(end)[0]
        validate_phase(block, stage == 'WITHDRAW')
        print(f'[PASS] learned-route {stage} three-packet/FIB phase', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--topology', required=True, type=Path)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--timeout', type=float, default=900)
    args = parser.parse_args()
    topology = args.topology.resolve()
    try:
        if args.run:
            run(topology, args.timeout)
        validate_identity(topology)
        validate_cycle((topology / 'danos.serial.log').read_text(errors='replace'),
                       (topology / 'frr-2.serial.log').read_text(errors='replace'))
    except (OSError, ValueError, TimeoutError) as error:
        print(f'[FAIL] learned-route cycle: {error}', file=sys.stderr)
        sys.exit(1)
    print('[PASS] VPP-originated learned-route lifecycle (not transit/PCI throughput)')
