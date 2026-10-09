#!/usr/bin/env python3
"""Bounded read-only VPP UART snapshots; quiescence is NOT endpoint PASS."""
import argparse
import fcntl
import os
import re
import select
import time


def counters(raw):
    text = raw.decode(errors="replace").replace("\r", "")
    ports = {}
    current = None
    for line in text.splitlines():
        match = re.match(r"^(GigabitEthernet\S+)\s+\d+\s+(?:up|down)\s", line)
        if match:
            current = match.group(1)
            ports[current] = {"rx": 0, "tx": 0, "drops": 0}
        for label, field in (("rx packets", "rx"), ("tx packets", "tx"), ("drops", "drops")):
            match = re.search(r"\b" + label + r"\s+(\d+)", line)
            if match and current:
                ports[current][field] = int(match.group(1))
    return ports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device")
    parser.add_argument("log")
    parser.add_argument("--seconds", type=int, default=900)
    parser.add_argument("--interval", type=int, default=20)
    args = parser.parse_args()
    if args.seconds <= 0 or args.interval <= 0:
        parser.error("positive durations required")
    fd = os.open(args.device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        # Exclusive creation preserves previous evidence; caller configures UART.
        with open(args.log, "xb") as log:
            start = time.monotonic()
            previous = None
            idle = 0
            activity = False
            sample = 0
            while time.monotonic() - start < args.seconds:
                marker = f"TRANSIT_SAMPLE_END_{sample}"
                cmd = (f"echo TRANSIT_SAMPLE_{sample}; date -u; "
                       "vppctl -s /run/vpp/cli.sock show interface; "
                       "vppctl -s /run/vpp/cli.sock show ip neighbors; "
                       "vppctl -s /run/vpp/cli.sock show errors; "
                       f"echo {marker}\n").encode()
                os.write(fd, cmd)
                raw = bytearray()
                deadline = min(time.monotonic() + 6, start + args.seconds)
                while time.monotonic() < deadline and len(raw) < 200000:
                    if select.select([fd], [], [], 0.2)[0]:
                        try:
                            raw.extend(os.read(fd, 8192))
                        except BlockingIOError:
                            continue
                    if ("\r\n" + marker).encode() in raw:
                        break
                log.write(raw)
                log.flush()
                complete = ("\r\n" + marker).encode() in raw
                ports = counters(raw) if complete else {}
                print(f"sample={sample} complete={complete} counters={ports}", flush=True)
                if ports and previous is not None:
                    if ports != previous:
                        activity = True
                        idle = 0
                    else:
                        idle += 1
                if ports:
                    previous = ports
                if activity and idle >= 3:
                    print("WINDOW_QUIESCENT; endpoint statistics still required", flush=True)
                    return
                sample += 1
                remaining = args.seconds - (time.monotonic() - start)
                if remaining > 0:
                    time.sleep(min(args.interval, remaining))
            print("WINDOW_LIMIT_REACHED; endpoint statistics still required", flush=True)
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
