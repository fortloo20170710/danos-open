#!/usr/bin/env python3
"""Render explicit four-node lab CLI profiles; never discover/bind/reboot devices."""
import argparse


def render(role):
    if role not in ("R1", "R2", "R3", "R4"):
        raise ValueError("unknown physical lab role")
    lines = []
    if role == "R3":
        for port, address in ((1, "10.20.0.1"), (2, "10.30.0.1"), (4, "10.10.0.1")):
            lines += [f"set interface state GigabitEthernet{port}/0/0 up",
                      f"set interface ip address GigabitEthernet{port}/0/0 {address}/24"]
        lines += ["ip route add 30.30.30.0/24 via 10.20.0.2 GigabitEthernet1/0/0 via 10.30.0.2 GigabitEthernet2/0/0"]
    else:
        subnet = {"R1": "10.30.0", "R2": "10.10.0", "R4": "10.20.0"}[role]
        lines += ["set interface state GigabitEthernet1/0/0 up",
                  f"set interface ip address GigabitEthernet1/0/0 {subnet}.2/24"]
        prefixes = ("10.20.0.2/32", "10.30.0.2/32", "30.30.30.0/24") if role == "R2" else ("10.10.0.2/32",)
        lines += [f"ip route add {prefix} via {subnet}.1 GigabitEthernet1/0/0" for prefix in prefixes]
        if role != "R2":
            lines += ["create loopback interface instance 0", "set interface state loop0 up"]
            lines += [f"set interface ip address loop0 30.30.30.{i}/32" for i in range(2, 6)]
    # P3 is the out-of-band management port on every physical runner. Keep it
    # administratively up even when the selected data-plane topology does not
    # use it; taking it down can strand remote recovery/configuration access.
    lines.append("set interface state GigabitEthernet3/0/0 up")
    transit_ports = {"R1": (1,), "R2": (1,), "R3": (1, 2, 4), "R4": (1,)}[role]
    unused = tuple(port for port in range(1, 5)
                   if port not in transit_ports and port != 3)
    lines += [f"set interface state GigabitEthernet{port}/0/0 down" for port in unused]
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("role", choices=("R1", "R2", "R3", "R4"))
    args = parser.parse_args()
    print(render(args.role), end="")
