#!/bin/bash
# Revalidate virtual evidence only. No PCI lane, deployment, or remote access.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${BASELINE_OUT_DIR:-$ROOT/build/virtual-performance-baselines}"
mkdir -p "$OUT"
python3 "$ROOT/danos-test/qemu/record_qemu_ecmp_soak_result.py" \
    --topology "${QEMU_TOPOLOGY_DIR:-$ROOT/build/qemu-shared-b3804e0-accepted}" \
    --required-count "${QEMU_ECMP_SOAK_COUNT:-1000}" --out "$OUT/qemu.env" \
    > "$OUT/qemu.log" 2>&1
VMWARE_RESULT_FILE="$OUT/vmxnet3.env" \
    bash "$ROOT/danos-test/vmware/verify_vmxnet3_packet_baseline.sh" > "$OUT/vmxnet3.log" 2>&1
python3 "$ROOT/danos-test/perf/report_baselines.py" \
    --result "$OUT/qemu.env" --result "$OUT/vmxnet3.env" \
    --out "$OUT/baselines.json" > "$OUT/baselines.log"
echo "Virtual functional evidence validated: $OUT (not a new traffic measurement)"
