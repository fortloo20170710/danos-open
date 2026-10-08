#!/usr/bin/env python3
"""Ensure release-gate overrides cannot disable or shrink the ECMP soak."""

import os
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / "danos-test/integration/run_v016_release_gate.sh"
GATE_TEXT = GATE.read_text()
VMWARE_VERIFIER = ROOT / "danos-test/vmware/verify_vmxnet3_packet_baseline.sh"
VMWARE_VERIFIER_TEXT = VMWARE_VERIFIER.read_text()


def check_rejected(name: str, value: str, expected: str) -> None:
    env = os.environ.copy()
    env[name] = value
    result = subprocess.run(
        ["bash", str(GATE)],
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=True,
        timeout=5,
        check=False,
    )
    output = result.stdout + result.stderr
    assert result.returncode == 2, (
        f"{name}={value!r} returned {result.returncode}, expected 2:\n{output}"
    )
    assert expected in output, f"{name}={value!r} missing {expected!r}:\n{output}"


def check_open_lane_blocks() -> None:
    """An environment-restricted lane must block the release by default.

    The gate records a skipped hardware lane as ENVIRONMENT-OPEN but used to
    leave `failures` at zero, so a release could be declared with the DPDK
    lane never run. The policy lives in release_gate_policy.sh so it can be
    asserted directly, without running every lane the gate would otherwise
    execute.
    """
    policy = ROOT / "danos-test/integration/release_gate_policy.sh"

    def decide(env_extra: dict) -> tuple[int, int, str]:
        env = os.environ.copy()
        env.pop("DANOS_RELEASE_ALLOW_OPEN_LANES", None)
        env.update(env_extra)
        script = (
            f'. "{policy}"\n'
            "release_gate_open_lane_decision pci-dpdk-preflight\n"
            'echo "BLOCKS=$OPEN_LANE_BLOCKS WAIVED=$OPEN_LANE_WAIVED"\n'
        )
        result = subprocess.run(
            ["bash", "-c", script],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        output = result.stdout + result.stderr
        blocks = waived = -1
        for line in output.splitlines():
            if line.startswith("BLOCKS="):
                blocks = int(line.split("BLOCKS=")[1].split()[0])
                waived = int(line.split("WAIVED=")[1].split()[0])
        return blocks, waived, output

    blocks, waived, output = decide({})
    assert blocks == 1 and waived == 0, (
        "an open lane must block the release by default:\n" + output
    )
    assert "blocks release" in output, output

    blocks, waived, output = decide({"DANOS_RELEASE_ALLOW_OPEN_LANES": "1"})
    assert blocks == 0 and waived == 1, (
        "an explicit waiver must unblock the lane and be marked waived:\n"
        + output
    )
    assert "WAIVED" in output, output

    # Any value other than exactly 1 must not waive: a typo must not silently
    # release a gate.
    blocks, _, output = decide({"DANOS_RELEASE_ALLOW_OPEN_LANES": "yes"})
    assert blocks == 1, (
        "only DANOS_RELEASE_ALLOW_OPEN_LANES=1 may waive an open lane:\n" + output
    )


def check_default_i211_traffic_iso_is_current_clean_profile() -> None:
    expected_iso = "danos-open-v0.16.0-rc1-i211-dpdk-traffic-runner-r5.iso"
    expected_commit = "efef90f7e3c3b12bf912e826277d5ba8b148e18f"
    assert expected_iso in GATE_TEXT, "release gate default must use the accepted r5 traffic ISO"
    assert expected_commit in GATE_TEXT, "release gate must pin r5's clean source commit"
    assert "traffic-runner-r1.iso" not in GATE_TEXT, "release gate default has regressed to stale r1"


def check_default_qemu_topology_is_latest_accepted_lifecycle() -> None:
    expected = "build/qemu-frr-vpp-topology-705c70b-baseline"
    stale = "build/qemu-frr-vpp-topology-140-soak-serialized"
    assert expected in GATE_TEXT, (
        "release gate default must use the accepted identity-bound QEMU topology"
    )
    assert stale not in GATE_TEXT, (
        "release gate default regressed to stale topology-140 evidence"
    )


def check_vmware_verifier_defaults_match_accepted_clean_baseline() -> None:
    expected_defaults = (
        "build/vmware-vmxnet3-test/peer-clean.serial.log",
        "build/vmware-vmxnet3-test/danos-clean.serial.log",
        "danos-open-v0.16.0-rc1-vmware-vmxnet3-polling-clean.iso",
    )
    for value in expected_defaults:
        assert value in VMWARE_VERIFIER_TEXT, (
            f"VMware verifier default must use accepted clean baseline: {value}"
        )
    stale_defaults = (
        "build/vmware-vmxnet3-test/peer.serial.log",
        "build/vmware-vmxnet3-test/danos.serial.log",
        "danos-open-v0.16.0-rc1-vmware-vmxnet3-polling.iso}",
    )
    for value in stale_defaults:
        assert value not in VMWARE_VERIFIER_TEXT, (
            f"VMware verifier default regressed to stale evidence: {value}"
        )


def main() -> None:
    check_rejected(
        "QEMU_ECMP_SOAK_REQUIRED", "0", "requires QEMU_ECMP_SOAK_REQUIRED=1"
    )
    for value in ("0", "999", "nope"):
        check_rejected(
            "QEMU_ECMP_SOAK_COUNT",
            value,
            "requires at least 1000 packets per ECMP flow",
        )
    check_open_lane_blocks()
    check_default_i211_traffic_iso_is_current_clean_profile()
    check_default_qemu_topology_is_latest_accepted_lifecycle()
    check_vmware_verifier_defaults_match_accepted_clean_baseline()
    print(
        "PASS: release gate rejects disabled, undersized and invalid ECMP soak "
        "overrides, blocks on an open hardware lane unless waived, and pins "
        "QEMU, VMware and I211 defaults to accepted clean evidence"
    )


if __name__ == "__main__":
    main()
