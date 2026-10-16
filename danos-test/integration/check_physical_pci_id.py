#!/usr/bin/env python3
"""Reject virtual NIC identities from the physical PCI lane (no device writes)."""
import argparse
import re


def physical_id(vendor, device):
    vendor, device = vendor.lower().removeprefix('0x'), device.lower().removeprefix('0x')
    if not all(re.fullmatch(r'[0-9a-f]{4}', x) for x in (vendor, device)):
        raise ValueError('missing or malformed PCI vendor/device identity')
    # e1000/e1000e IDs overlap real Intel models: conservatively reserve these
    # identities for the virtual lane; use an unambiguous physical runner.
    if vendor in {'15ad', '1af4', '1b36', '1234', '1414'} or (vendor == '8086' and device in {'100e', '100f', '10d3'}):
        raise ValueError('virtual or virtualization-ambiguous NIC belongs in a functional lane')
    return vendor + ':' + device


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('vendor')
    parser.add_argument('device')
    args = parser.parse_args()
    try:
        print(physical_id(args.vendor, args.device))
    except ValueError as exc:
        parser.exit(2, '[SKIP] ' + str(exc) + '\n')
