#!/bin/bash
# Release-gate policy for environment-restricted lanes.
#
# A lane that cannot run because its environment is unavailable (no DPDK
# hardware, no runner prerequisites) is not a pass. It used to be recorded as
# ENVIRONMENT-OPEN without counting as a failure, so a release could be
# declared with a hardware lane never run.
#
# Sourced by run_v016_release_gate.sh, and unit-tested directly by
# test_v016_release_gate_config.py.

# Decide whether an open lane blocks the release.
#
# Sets OPEN_LANE_BLOCKS to 1 (blocking) or 0 (waived), and prints a one-line
# rationale. An explicit DANOS_RELEASE_ALLOW_OPEN_LANES=1 waives the block;
# the waiver is deliberate and is recorded in the gate's result file so an
# approved-with-open-lanes release stays distinguishable from a clean one.
release_gate_open_lane_decision() {
    local lane="${1:-lane}"
    if test "${DANOS_RELEASE_ALLOW_OPEN_LANES:-0}" = 1; then
        OPEN_LANE_BLOCKS=0
        OPEN_LANE_WAIVED=1
        printf '[WAIVED] %s open lane waived by DANOS_RELEASE_ALLOW_OPEN_LANES=1\n' "$lane"
        return 0
    fi
    OPEN_LANE_BLOCKS=1
    OPEN_LANE_WAIVED=0
    printf '[FAIL] %s open lane blocks release (set DANOS_RELEASE_ALLOW_OPEN_LANES=1 to waive)\n' "$lane"
    return 1
}