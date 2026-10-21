#!/bin/bash
# Revalidate virtual evidence only. No PCI lane, deployment, or remote access.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${BASELINE_OUT_DIR:-$ROOT/build/virtual-performance-baselines}"
mkdir -p "$OUT"
QEMU_TOPOLOGY="${QEMU_TOPOLOGY_DIR:-$ROOT/build/qemu-shared-b3804e0-accepted}"
QEMU_RESULT_FILE="$OUT/qemu.env"
qemu_open_reason=''
if ! test -d "$QEMU_TOPOLOGY"; then
    qemu_open_reason="accepted QEMU topology directory is missing: $QEMU_TOPOLOGY"
elif ! test -s "$QEMU_TOPOLOGY/run-manifest.env" || ! test -s "$QEMU_TOPOLOGY/danos.serial.log"; then
    qemu_open_reason="accepted QEMU topology lacks its run manifest or DANOS serial evidence: $QEMU_TOPOLOGY"
fi
if test -n "$qemu_open_reason"; then
    {
        printf 'schema_version=1\n'
        printf 'status=ENVIRONMENT-OPEN\n'
        printf 'preflight_status=ENVIRONMENT-OPEN\n'
        printf 'performance_status=ENVIRONMENT-OPEN\n'
        printf 'stage=long-stability\n'
        printf 'lane=qemu-e1000\n'
        printf 'reason=%q\n' "$qemu_open_reason"
    } > "$QEMU_RESULT_FILE"
    printf '[OPEN] %s\n' "$qemu_open_reason" > "$OUT/qemu.log"
    qemu_open=1
else
    python3 "$ROOT/danos-test/qemu/record_qemu_ecmp_soak_result.py" \
        --topology "$QEMU_TOPOLOGY" \
        --required-count "${QEMU_ECMP_SOAK_COUNT:-1000}" --out "$QEMU_RESULT_FILE" \
        > "$OUT/qemu.log" 2>&1
    qemu_open=0
fi
VMWARE_ISO="${VMWARE_ISO:-$ROOT/build/danos-open-v0.16.0-rc1-vmware-vmxnet3-polling-clean.iso}"
VMWARE_PEER_LOG="${VMWARE_PEER_LOG:-$ROOT/build/vmware-vmxnet3-test/peer-clean.serial.log}"
VMWARE_DANOS_LOG="${VMWARE_DANOS_LOG:-$ROOT/build/vmware-vmxnet3-test/danos-clean.serial.log}"
VMWARE_RESULT_FILE="$OUT/vmxnet3-current.env"
vmware_open_reason=''
if ! test -f "$VMWARE_ISO"; then
    vmware_open_reason="required VMware ISO is missing: $VMWARE_ISO"
elif ! test -s "$VMWARE_PEER_LOG" || ! test -s "$VMWARE_DANOS_LOG"; then
    vmware_open_reason='required VMware peer/DANOS serial evidence is missing'
fi
if test -n "$vmware_open_reason"; then
    {
        printf 'status=ENVIRONMENT-OPEN\n'
        printf 'lane=vmware-vmxnet3-polling\n'
        printf 'reason=%q\n' "$vmware_open_reason"
    } > "$VMWARE_RESULT_FILE"
    printf '[OPEN] %s\n' "$vmware_open_reason" | tee "$OUT/vmxnet3.log"
    vmware_open=1
else
    VMWARE_ISO="$VMWARE_ISO" VMWARE_PEER_LOG="$VMWARE_PEER_LOG" \
        VMWARE_DANOS_LOG="$VMWARE_DANOS_LOG" VMWARE_RESULT_FILE="$VMWARE_RESULT_FILE" \
        bash "$ROOT/danos-test/vmware/verify_vmxnet3_packet_baseline.sh" \
        > "$OUT/vmxnet3.log" 2>&1
    vmware_open=0
fi
python3 "$ROOT/danos-test/perf/report_baselines.py" \
    --result "$QEMU_RESULT_FILE" --result "$VMWARE_RESULT_FILE" \
    --out "$OUT/baselines.json" > "$OUT/baselines.log"
if test "$qemu_open" -eq 1 || test "$vmware_open" -eq 1; then
    echo "Virtual lane revalidation is incomplete (QEMU/VMware evidence ENVIRONMENT-OPEN): $OUT"
    exit 2
fi
echo "Virtual functional evidence validated: $OUT (not a new traffic measurement)"
