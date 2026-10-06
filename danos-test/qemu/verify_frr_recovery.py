#!/usr/bin/env python3
"""Require service restart plus actual zebra reconnect and backend replay."""
import re
import sys
from pathlib import Path


def validate_vpp_packets(log: str) -> None:
    begin = log.rfind("VPP-RESTART-TEST BEGIN")
    if begin < 0:
        raise ValueError("no actual VPP restart begin")
    recovery = log[begin:]
    completion = recovery.find("VPP-RESTART-TEST PASS")
    if completion < 0:
        raise ValueError("latest VPP restart is incomplete")
    if re.search(r"VPP-RESTART-(?:PEER|TEST) FAIL", recovery):
        raise ValueError("VPP restart packet/replay failure")
    for address in ("10.10.0.2", "10.20.0.2"):
        marker = f"VPP-RESTART-PEER PASS address={address} packets=3 loss=0"
        if marker not in recovery[:completion]:
            raise ValueError(f"no lossless {address} probe between restart and completion")


def validate(frr: str, bridge: str) -> None:
    begin = frr.rfind("FRR-RESTART-BEGIN")
    if begin < 0:
        raise ValueError("no FRR restart begin")
    restart = frr[begin:]
    if not re.search(r"FRR-RESTART-REQUEST rc=0\b.*FRR-RESTART-TEST PASS", restart, re.S):
        raise ValueError("restart request/completion missing or out of order")
    if "FRR-RESTART-TEST FAIL" in restart:
        raise ValueError("FRR restart failure marker")
    disconnects = list(re.finditer(
        r"zapi peer EOF|zebra session disconnected; reconnecting|zebra receive failed:", bridge
    ))
    if not disconnects:
        raise ValueError("no zebra disconnect observed")
    recovery = bridge[disconnects[-1].start():]
    reconnect = recovery.find("zebra session reconnected")
    if reconnect < 0:
        raise ValueError("latest disconnect has no successful re-registration")
    replay = recovery[reconnect:]
    if not re.search(r"VPP backend recovery: replay attempted=[1-9][0-9]* failed=0\b", replay):
        raise ValueError("no positive successful backend replay after reconnect")
    if not re.search(r"zapi command=31\b.*vpp route add", replay, re.S):
        raise ValueError("no route notification/programming after reconnect")
    if re.search(r"ZAPI command \d+ (transaction|programming) failed:", replay):
        raise ValueError("route processing failed after reconnect")


if __name__ == "__main__":
    try:
        bridge = Path(sys.argv[2]).read_text(errors="replace")
        validate(Path(sys.argv[1]).read_text(errors="replace"), bridge)
        validate_vpp_packets(bridge)
    except (OSError, ValueError, IndexError) as error:
        print(f"[FAIL] FRR recovery evidence: {error}", file=sys.stderr)
        sys.exit(1)
    print("[PASS] FRR restart, zebra reconnect and post-reconnect route/replay evidence")
