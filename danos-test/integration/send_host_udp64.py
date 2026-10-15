#!/usr/bin/env python3
"""Send a finite, paced UDP64 batch from an independent Linux host.

The sender never configures routes. It refuses to send unless every target's
existing route uses the explicitly selected DUT gateway, interface, and source
address. Receiver NIC counters are required for forwarding acceptance.
"""
import argparse
import datetime as dt
import ipaddress
import json
from pathlib import Path
import re
import socket
import struct
import subprocess
import time

DEFAULT_TARGETS = ("30.30.30.2", "30.30.30.3", "30.30.30.4", "30.30.30.5")
MAX_COUNT_PER_FLOW = 4096
MAX_RATE_PER_FLOW = 1000
PAYLOAD_BYTES = 22  # 14-byte Ethernet + 20-byte IPv4 + 8-byte UDP + payload = 64.


def route_matches(output: str, *, gateway: str, interface: str, source: str) -> bool:
    """Check `ip route get` output without accepting a default-router path."""
    fields = output.split()
    values = {}
    for key in ("via", "dev", "src"):
        try:
            values[key] = fields[fields.index(key) + 1]
        except (ValueError, IndexError):
            return False
    return (values["via"] == gateway and values["dev"] == interface and
            values["src"] == source)


def inspect_routes(targets, *, gateway: str, interface: str, source: str):
    routes = {}
    for target in targets:
        try:
            result = subprocess.run(
                ["ip", "-4", "route", "get", target, "from", source],
                check=False, capture_output=True, text=True, timeout=3)
        except (OSError, subprocess.SubprocessError) as error:
            routes[target] = {"returncode": None, "output": str(error),
                              "matches": False}
            continue
        output = result.stdout.strip()
        routes[target] = {"returncode": result.returncode, "output": output,
                          "matches": result.returncode == 0 and route_matches(
                              output, gateway=gateway, interface=interface,
                              source=source)}
    return routes


def validate_args(source, gateway, targets, rate, count, interface):
    source_ip = ipaddress.ip_address(source)
    gateway_ip = ipaddress.ip_address(gateway)
    if source_ip.version != 4 or gateway_ip.version != 4:
        raise ValueError("source and gateway must be IPv4")
    if not targets or len(targets) > 16 or len(set(targets)) != len(targets):
        raise ValueError("provide 1..16 distinct destination addresses")
    if any(ipaddress.ip_address(target).version != 4 for target in targets):
        raise ValueError("all targets must be IPv4")
    if type(rate) is not int or not 1 <= rate <= MAX_RATE_PER_FLOW:
        raise ValueError(f"rate must be 1..{MAX_RATE_PER_FLOW} pps per flow")
    if type(count) is not int or not 1 <= count <= MAX_COUNT_PER_FLOW:
        raise ValueError(f"count must be 1..{MAX_COUNT_PER_FLOW} frames per flow")
    if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,15}", interface):
        raise ValueError("interface must be a valid Linux interface name")


def interface_carrier(interface: str) -> bool:
    try:
        return Path("/sys/class/net", interface, "carrier").read_text().strip() == "1"
    except OSError:
        return False


def build_payload(sequence: int, flow_index: int) -> bytes:
    return struct.pack("!IH", sequence, flow_index) + b"DANOS-HOST-UDP64"


def send_batch(*, source: str, gateway: str, interface: str, targets,
               rate: int, count: int, output: Path) -> dict:
    validate_args(source, gateway, targets, rate, count, interface)
    routes = inspect_routes(targets, gateway=gateway, interface=interface,
                            source=source)
    carrier = interface_carrier(interface)
    result = {
        "schema_version": 1,
        "status": "ENVIRONMENT-OPEN" if (not carrier or
                  not all(r["matches"] for r in routes.values())) else "FAIL",
        "scope": "independent-host paced UDP64 source batch",
        "sender": {"source_ipv4": source, "interface": interface,
        "required_gateway": gateway, "carrier_up": carrier},
        "targets": list(targets), "udp_destination_port": 30000,
        "l2_frame_bytes_including_fcs": 64, "udp_payload_bytes": PAYLOAD_BYTES,
        "requested_pps_per_flow": rate, "count_per_flow": count,
        "requested_total_frames": len(targets) * count,
        "routes": routes, "sent_per_target": {target: 0 for target in targets},
        "performance_status": "ENVIRONMENT-OPEN",
        "limitations": ["sendto completion is not hardware TX proof",
                        "receiver NIC counters are required",
                        "latency and line rate are not measured"],
    }
    if not carrier or not all(r["matches"] for r in routes.values()):
        reason = []
        if not carrier:
            reason.append("source interface has no carrier")
        if not all(r["matches"] for r in routes.values()):
            reason.append("route guard mismatch")
        result["failure_reason"] = "; ".join(reason) + "; no traffic was sent"
    else:
        sockets = []
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sockets.append(sock)
            sock.bind((source, 0))
            result["source_udp_port"] = sock.getsockname()[1]
            started = time.monotonic()
            result["started_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
            for sequence in range(count):
                deadline = started + sequence / rate
                delay = deadline - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                for flow_index, target in enumerate(targets):
                    payload = build_payload(sequence, flow_index)
                    sent = sock.sendto(payload, (target, 30000))
                    if sent != PAYLOAD_BYTES:
                        raise OSError(f"short UDP send to {target}: {sent} bytes")
                    result["sent_per_target"][target] += 1
            result["finished_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
            elapsed = time.monotonic() - started
            result["elapsed_seconds"] = elapsed
            result["sendto_completed"] = sum(result["sent_per_target"].values())
            result["observed_sendto_pps"] = result["sendto_completed"] / elapsed
            result["status"] = "PASS"
        except (OSError, subprocess.SubprocessError) as error:
            result["failure_reason"] = str(error)
        finally:
            for sock in sockets:
                sock.close()
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", default="192.168.1.243")
    parser.add_argument("--gateway", default="192.168.1.244")
    parser.add_argument("--interface", default="enp4s0")
    parser.add_argument("--target", action="append", dest="targets")
    parser.add_argument("--rate-pps-per-flow", type=int, default=100)
    parser.add_argument("--count-per-flow", type=int, default=256)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    targets = args.targets or DEFAULT_TARGETS
    try:
        result = send_batch(source=args.source, gateway=args.gateway,
                            interface=args.interface, targets=targets,
                            rate=args.rate_pps_per_flow,
                            count=args.count_per_flow, output=args.output)
    except ValueError as error:
        parser.error(str(error))
    print(json.dumps({key: result.get(key) for key in
                      ("status", "requested_total_frames", "sendto_completed",
                       "elapsed_seconds", "observed_sendto_pps", "failure_reason")}))
    return 0 if result["status"] == "PASS" else 2


if __name__ == "__main__":
    raise SystemExit(main())
