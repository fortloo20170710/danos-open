#!/bin/bash
# R2 发包端 - testpmd txonly 64B 单核基线
# 运行前提: systemctl stop vpp; 接口已绑定 uio_pci_generic; hugepages >= 28

set -euo pipefail

# ========== 配置区 (根据实际物理机修改) ==========
TESTPMD="./testpmd-static/bin/testpmd-static"
BDF1="0000:01:00.0"      # lspci -d 8086:1539 确认
BDF2="0000:02:00.0"
CORE="1"                  # 单核 (I211 单队列)
DURATION=30               # 测试时长(秒)
PKT_SIZE=64               # 64B 包大小
BURST=32                  # burst 大小

# 目标 MAC (R3 对应端口 MAC，需提前通过 vppctl show int 或 testpmd rxonly 获取)
DST_MAC1="00:1f:7a:69:f7:4c"  # R3 P1 MAC
DST_MAC2="00:1f:7a:69:f7:4d"  # R3 P2 MAC

LOG="testpmd_tx_$(date +%Y%m%d_%H%M%S).log"
CMD_FILE="/tmp/testpmd_tx_cmd.txt"
# ================================================

echo "=== R2 TX Test Start: $(date) ===" | tee "$LOG"
echo "Config: BDF1=$BDF1 BDF2=$BDF2 CORE=$CORE DURATION=${DURATION}s PKT=$PKT_SIZE BURST=$BURST" | tee -a "$LOG"

# 生成 testpmd 命令脚本
cat > "$CMD_FILE" <<EOF
set fwd txonly
set verbose 1
set burst $BURST
set txpkts $PKT_SIZE
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

# 运行 testpmd
# -l: 核心列表
# -n: 内存通道数
# -a: 允许的 PCI 设备
# --socket-mem: 每 socket hugepages (MB)
# --file-prefix: 避免与其他进程冲突
$TESTPMD -l "$CORE" -n 4 \
    -a "$BDF1" -a "$BDF2" \
    --socket-mem=64,0 \
    --no-pci-scan \
    --file-prefix=tx \
    -- "$CMD_FILE" 2>&1 | tee -a "$LOG"

echo "=== R2 TX Test End: $(date) ===" | tee -a "$LOG"
echo "Log saved: $LOG"

# 计算 SHA256
sha256sum "$LOG" > "${LOG}.sha256"
echo "SHA256: $(cat ${LOG}.sha256)"