#!/bin/bash
# R3/R1/R4 收包端 - testpmd rxonly 统计计数器
# 运行前提: systemctl stop vpp; 接口已绑定 uio_pci_generic; hugepages >= 28

set -euo pipefail

# ========== 配置区 ==========
TESTPMD="./testpmd-static/bin/testpmd-static"
BDF1="0000:01:00.0"
BDF2="0000:02:00.0"
CORE="1"
DURATION=35               # 比发包端多 5 秒缓冲
LOG="testpmd_rx_$(date +%Y%m%d_%H%M%S).log"
CMD_FILE="/tmp/testpmd_rx_cmd.txt"
# ============================

echo "=== RX Test Start: $(date) ===" | tee "$LOG"
echo "Config: BDF1=$BDF1 BDF2=$BDF2 CORE=$CORE DURATION=${DURATION}s" | tee -a "$LOG"

cat > "$CMD_FILE" <<EOF
set fwd rxonly
set verbose 1
set corelist $CORE
port config all speed 1000
start
sleep $DURATION
stop
show port stats all
show port xstats all
quit
EOF

$TESTPMD -l "$CORE" -n 4 \
    -a "$BDF1" -a "$BDF2" \
    --socket-mem=64,0 \
    --no-pci-scan \
    --file-prefix=rx \
    -- "$CMD_FILE" 2>&1 | tee -a "$LOG"

echo "=== RX Test End: $(date) ===" | tee -a "$LOG"
echo "Log saved: $LOG"

sha256sum "$LOG" > "${LOG}.sha256"
echo "SHA256: $(cat ${LOG}.sha256)"