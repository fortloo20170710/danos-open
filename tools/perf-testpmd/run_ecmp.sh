#!/bin/bash
# ECMP 双路径分布测试
# R2: 双端口发不同 MAC；R3: 双端口收包统计桶分布
# 需在 R2、R3 同步启动 (串口轮流操作，或开两个终端)

set -euo pipefail

# ========== 配置区 ==========
TESTPMD="./testpmd-static/bin/testpmd-static"
BDF1="0000:01:00.0"
BDF2="0000:02:00.0"
CORE="1"
DURATION=30
LOG="testpmd_ecmp_$(date +%Y%m%d_%H%M%S).log"
CMD_FILE="/tmp/testpmd_ecmp_cmd.txt"

# 角色: tx 或 rx (运行时传参: ./run_ecmp.sh tx 或 ./run_ecmp.sh rx)
ROLE="${1:-tx}"
# ============================

echo "=== ECMP $ROLE Test Start: $(date) ===" | tee "$LOG"

if [[ "$ROLE" == "tx" ]]; then
    # R2 发包端: 端口0发MAC1，端口1发MAC2
    DST_MAC1="00:1f:7a:69:f7:4c"  # R3 P1
    DST_MAC2="00:1f:7a:69:f7:4d"  # R3 P2

    cat > "$CMD_FILE" <<EOF
set fwd txonly
set verbose 1
set burst 32
set txpkts 64
set corelist $CORE
port config all speed 1000
port stop 0
port stop 1
set port 0 mac $DST_MAC1
set port 1 mac $DST_MAC2
port start 0
port start 1
start tx_first 32
sleep $DURATION
stop
show port stats all
show port xstats all
quit
EOF

else
    # R3 收包端: 双端口 rxonly
    cat > "$CMD_FILE" <<EOF
set fwd rxonly
set verbose 1
set corelist $CORE
port config all speed 1000
start
sleep $((DURATION + 5))
stop
show port stats all
show port xstats all
quit
EOF
fi

$TESTPMD -l "$CORE" -n 4 \
    -a "$BDF1" -a "$BDF2" \
    --socket-mem=64,0 \
    --no-pci-scan \
    --file-prefix=ecmp_${ROLE} \
    -- "$CMD_FILE" 2>&1 | tee -a "$LOG"

echo "=== ECMP $ROLE Test End: $(date) ===" | tee -a "$LOG"
sha256sum "$LOG" > "${LOG}.sha256"
echo "SHA256: $(cat ${LOG}.sha256)"