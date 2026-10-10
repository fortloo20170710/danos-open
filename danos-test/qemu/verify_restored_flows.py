#!/usr/bin/env python3
"""Require complete four-destination post-restoration evidence, not one sample."""
import argparse
from pathlib import Path
import re
import sys


def verify(text):
    if re.search(r'VPP-ECMP-PATH-UP(?:-FLOW)?-FAIL', text):
        raise ValueError('restored path failure marker present')
    records = re.findall(r'^VPP-ECMP-PATH-UP-FLOW target=(\S+) tx=(\d+) rx=(\d+) loss=(\S+)\s*$',
                         text.replace('\r', ''), re.M)
    expected = {f'30.30.30.{i}' for i in range(2, 6)}
    if len(records) != 4 or {row[0] for row in records} != expected:
        raise ValueError('exactly four unique restored destination records required')
    if any((tx, rx, loss) != ('5', '5', '0') for _, tx, rx, loss in records):
        raise ValueError('every restored destination must receive all five probes')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    args = parser.parse_args()
    try:
        verify(args.log.read_text())
    except (OSError, ValueError) as error:
        print(f'[FAIL] restored flows: {error}', file=sys.stderr)
        return 1
    print('[PASS] all four restored destinations: 20/20 probes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
