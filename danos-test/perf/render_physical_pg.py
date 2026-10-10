#!/usr/bin/env python3
"""Render bounded, disabled VPP NIC-output UDP streams for an isolated peer.

This creates no VPP state and sends no traffic. NIC TX and downstream hardware
RX evidence is required before treating generated traffic as a physical test.
"""
import argparse
import ipaddress
import re


def render(interface, source_mac, target_mac, source_ip, targets, rate, count):
    if not re.fullmatch(r"GigabitEthernet[0-9]+/[0-9]+/[0-9]+", interface):
        raise ValueError("explicit GigabitEthernet output interface required")
    for mac in (source_mac, target_mac):
        if not re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", mac):
            raise ValueError("invalid Ethernet address")
        if int(mac[:2], 16) & 1 or mac.lower() == "00:00:00:00:00:00":
            raise ValueError("nonzero unicast Ethernet address required")
    addresses = [ipaddress.IPv4Address(value) for value in [source_ip, *targets]]
    if any(ip.is_multicast or ip.is_unspecified for ip in addresses):
        raise ValueError("unicast IPv4 addresses required")
    if not 2 <= len(targets) <= 16 or len(set(targets)) != len(targets):
        raise ValueError("two to sixteen distinct destination flows required")
    if not isinstance(rate, int) or not 1 <= rate <= 1000000:
        raise ValueError("per-flow rate must be 1..1000000 pps")
    if not isinstance(count, int) or not 1 <= count <= 10000000:
        raise ValueError("finite per-flow packet count must be 1..10000000")
    streams = []
    for index, target in enumerate(targets):
        streams.append(f"""packet-generator new {{
  name danos_physical_udp_{index}
  limit {count}
  rate {rate}
  size 64-64
  interface {interface}
  node {interface}-output
  data {{
    IP4: {source_mac} -> {target_mac}
    UDP: {source_ip} -> {target}
    UDP: {20000 + index} -> 30000
    incrementing 22
  }}
}}
""")
    return "\n".join(streams)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interface", required=True)
    parser.add_argument("--source-mac", required=True)
    parser.add_argument("--target-mac", required=True)
    parser.add_argument("--source-ip", required=True)
    parser.add_argument("--target", action="append", required=True)
    parser.add_argument("--rate", type=int, default=100)
    parser.add_argument("--count", type=int, default=1000)
    args = parser.parse_args()
    try:
        text = render(args.interface, args.source_mac, args.target_mac,
                      args.source_ip, args.target, args.rate, args.count)
    except ValueError as exc:
        parser.error(str(exc))
    print(text, end="")


if __name__ == "__main__":
    main()
