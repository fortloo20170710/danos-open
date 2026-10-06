#!/bin/bash
# Legacy entry point retained as a read-only evidence collector.
# Historical mock/kernel tests MUST NOT be called real VPP/DPDK acceptance.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
case "${1:-capture}" in
    --list)
        echo "capture (default): read-only runtime evidence, NOT an acceptance gate"
        echo "Use capture_vpp_runtime.py --help for output/socket/container/ISO options."
        echo "Legacy conformance/perf/e2e modes are refused: they did not test real VPP."
        ;;
    capture|all)
        if [ "$#" -gt 0 ]; then shift; fi
        exec python3 "$SCRIPT_DIR/capture_vpp_runtime.py" "$@"
        ;;
    *)
        echo "ERROR: '${1}' is not a real VPP acceptance gate." >&2
        echo "Use existing QEMU/FRR or DPDK lanes with their exact manifests and packet evidence." >&2
        echo "Read-only collection: bash $0 capture --output build/vpp-evidence-new" >&2
        exit 2
        ;;
esac
