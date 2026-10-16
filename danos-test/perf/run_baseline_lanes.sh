#!/bin/bash
# Read archived virtual results and run a read-only local PCI preflight.
# No deployment, PCI rebinding, hugepage allocation or remote router access.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${BASELINE_OUT_DIR:-$ROOT/build/performance-baselines}"
mkdir -p "$OUT"
python3 "$ROOT/danos-test/perf/report_baselines.py" \
    --result "${BASELINE_QEMU_RESULT:-$ROOT/build/qemu-shared-b3804e0-accepted/ecmp-result.env}" \
    --result "${BASELINE_VMWARE_RESULT:-$ROOT/build/v016-vmware-vmxnet3-clean.env}" \
    --out "$OUT/virtual-baselines.json" > "$OUT/virtual-baselines.log"
set +e
DPDK_RESULT_FILE="$OUT/pci-preflight.env" bash "$ROOT/danos-test/integration/run_vpp_dpdk_lane.sh" \
    > "$OUT/pci-preflight.log" 2>&1
rc=$?
set -e
cat "$OUT/pci-preflight.log"
echo "Baseline artifacts: $OUT"
# 2 explicitly means hardware/performance evidence is still open.
exit "$rc"
