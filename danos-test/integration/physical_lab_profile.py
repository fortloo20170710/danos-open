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
    unused = (3,) if role == "R3" else (2, 3, 4)
    lines += [f"set interface state GigabitEthernet{port}/0/0 down" for port in unused]
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("role", choices=("R1", "R2", "R3", "R4"))
    args = parser.parse_args()
    print(render(args.role), end="")
