#!/bin/bash
# 物理机结果采集脚本 - 在开发机上运行
# 用法: ./collect_results.sh [R2_IP] [R3_IP] [R1_IP] [R4_IP]

set -euo pipefail

R2_IP="${1:-192.168.1.244}"
R3_IP="${2:-192.168.1.246}"
R1_IP="${3:-192.168.1.245}"
R4_IP="${4:-192.168.1.247}"

OUT_DIR="build/physical-dpdk-perf-$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT_DIR"

echo "=== Collecting results from physical nodes ==="
echo "Output dir: $OUT_DIR"

# R2 TX 日志
echo "Collecting from R2 ($R2_IP)..."
scp root@$R2_IP:/root/testpmd_tx_*.log "$OUT_DIR/" 2>/dev/null || true
scp root@$R2_IP:/root/testpmd_tx_*.log.sha256 "$OUT_DIR/" 2>/dev/null || true
scp root@$R2_IP:/root/testpmd_ecmp_tx_*.log "$OUT_DIR/" 2>/dev/null || true

# R3 RX 日志
echo "Collecting from R3 ($R3_IP)..."
scp root@$R3_IP:/root/testpmd_rx_*.log "$OUT_DIR/" 2>/dev/null || true
scp root@$R3_IP:/root/testpmd_rx_*.log.sha256 "$OUT_DIR/" 2>/dev/null || true
scp root@$R3_IP:/root/testpmd_ecmp_rx_*.log "$OUT_DIR/" 2>/dev/null || true

# R1 RX 日志 (可选)
echo "Collecting from R1 ($R1_IP)..."
scp root@$R1_IP:/root/testpmd_rx_*.log "$OUT_DIR/" 2>/dev/null || true

# R4 RX 日志 (可选)
echo "Collecting from R4 ($R4_IP)..."
scp root@$R4_IP:/root/testpmd_rx_*.log "$OUT_DIR/" 2>/dev/null || true

echo ""
echo "=== Collected files ==="
ls -la "$OUT_DIR"/

# 生成 JSON 结果
echo ""
echo "=== Generating perf_result.json ==="
cd /home/aikon/danos-open
python3 parse_testpmd.py \
    "$OUT_DIR"/testpmd_tx_*.log \
    "$OUT_DIR"/testpmd_rx_*.log \
    -o "$OUT_DIR/perf_result.json" \
    --commit "$(git rev-parse HEAD 2>/dev/null || echo 'unknown')" \
    --cpu "Intel Xeon E-2288G @ 3.70GHz" 2>/dev/null || true

echo ""
echo "=== Result summary ==="
cat "$OUT_DIR/perf_result.json" 2>/dev/null | python3 -m json.tool || cat "$OUT_DIR/perf_result.json"