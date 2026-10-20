#!/bin/bash
# 生成 testpmd 部署包
# 运行前需要: 已编译好的 testpmd 二进制 (TESTPMD_PATH 指向二进制文件)
# 二进制名会被重命名为 testpmd-static

set -euo pipefail

TESTPMD_SRC="${TESTPMD_PATH:-/tmp/dpdk-install/bin/dpdk-testpmd}"
OUT_DIR="/tmp/testpmd-deploy-$(date +%Y%m%d)"
PKG_NAME="testpmd-deploy-$(date +%Y%m%d).tar.gz"

if [[ ! -f "$TESTPMD_SRC" ]]; then
    echo "ERROR: testpmd not found at $TESTPMD_SRC"
    echo "Please compile testpmd first (see docs/physical-dpdk-perf-testplan.md section 2)"
    echo "Or set TESTPMD_PATH=/path/to/testpmd"
    exit 1
fi

echo "Creating deploy package..."
mkdir -p "$OUT_DIR/testpmd-static/bin"
cp "$TESTPMD_SRC" "$OUT_DIR/testpmd-static/bin/testpmd-static"
chmod +x "$OUT_DIR/testpmd-static/bin/testpmd-static"

# 复制所有运行脚本
cp /home/aikon/danos-open/run_tx.sh "$OUT_DIR/"
cp /home/aikon/danos-open/run_rx.sh "$OUT_DIR/"
cp /home/aikon/danos-open/run_ecmp.sh "$OUT_DIR/"
cp /home/aikon/danos-open/collect_results.sh "$OUT_DIR/"
cp /home/aikon/danos-open/parse_testpmd.py "$OUT_DIR/"

# 生成 README
cat > "$OUT_DIR/README.txt" <<EOF
========================================
DANOS-Open testpmd 物理机性能测试部署包
========================================

生成时间: $(date)
Git Commit: $(git rev-parse HEAD 2>/dev/null || echo 'unknown')
testpmd 版本: \$($OUT_DIR/testpmd-static/bin/testpmd-static -v 2>&1 | head -1)

内容:
  testpmd-static/bin/testpmd-static    - testpmd 二进制 (动态链接, 兼容 Debian trixie)
  run_tx.sh                     - R2 发包端脚本 (单口/双口基线)
  run_rx.sh                     - R3/R1/R4 收包端脚本
  run_ecmp.sh                   - ECMP 双路径分布测试 (tx/rx 模式)
  collect_results.sh            - 开发机端结果采集脚本
  parse_testpmd.py              - 日志解析生成标准 JSON
  README.txt                    - 本文件

========================================
快速开始
========================================

1. 部署到物理机 (需 root, 串口登录):
   scp testpmd-deploy-YYYYMMDD.tar.gz root@R2:/root/
   scp testpmd-deploy-YYYYMMDD.tar.gz root@R3:/root/
   # R1/R4 同理

2. 物理机上解压:
   cd /root && tar -xzf testpmd-deploy-YYYYMMDD.tar.gz
   cd testpmd-deploy-YYYYMMDD
   chmod +x *.sh *.py

3. 运行前准备 (每台机器):
   systemctl stop vpp          # 停止 VPP 释放接口
   modprobe uio_pci_generic    # 确保驱动加载
   echo 28 > /proc/sys/vm/nr_hugepages  # 确保 hugepages
   # 确认接口绑定: cat /tmp/dpdk-pci-bind.log

4. 修改脚本中的配置 (关键!):
   编辑 run_tx.sh / run_rx.sh / run_ecmp.sh:
   - BDF1/BDF2: 根据 lspci -d 8086:1539 实际值修改
   - DST_MAC1/DST_MAC2: 目标 MAC (通过 vppctl show int 获取)
   - CORE: 使用的 CPU 核心 (建议 1，避开核心 0)

5. 执行测试 (串口轮流操作):
   # R2 发包
   ./run_tx.sh
   
   # R3 收包
   ./run_rx.sh
   
   # ECMP 分布测试 (R2 和 R3 近同时启动)
   # 终端1 (R2): ./run_ecmp.sh tx
   # 终端2 (R3): ./run_ecmp.sh rx

6. 结果采集 (开发机运行):
   ./collect_results.sh 192.168.1.244 192.168.1.246 192.168.1.245 192.168.1.247

========================================
输出产物
========================================
  testpmd_tx_YYYYMMDD_HHMMSS.log      - R2 发包日志
  testpmd_rx_YYYYMMDD_HHMMSS.log      - R3 收包日志
  testpmd_ecmp_tx/rx_*.log            - ECMP 测试日志
  perf_result.json                    - 标准化 JSON 结果 (提交验收矩阵)
  *.sha256                            - 证据完整性哈希

========================================
常见问题
========================================
Q: testpmd 报错 "cannot open /dev/uioX"
A: modprobe uio_pci_generic; 检查 /dev/uio* 存在

Q: "Cannot allocate hugepages"
A: echo 28 > /proc/sys/vm/nr_hugepages; grep HugePages /proc/meminfo

Q: Link down / 没收到包
A: 检查物理连线、交换机配置、对端 MAC 是否正确

Q: TX drops > 0
A: I211 单队列单核极限 ~3-4 Mpps，降低 burst 或接受 drops

========================================
证据提交
========================================
将 perf_result.json 和 *.log 归档到:
  build/physical-dpdk-perf-YYYYMMDD/

更新 docs/v0.16-acceptance-matrix.md:
  PCI/DPDK performance 行: ENVIRONMENT-OPEN -> BASELINE_ONLY
  附上 JSON 链接和限制说明
EOF

# 打包
cd /tmp
tar -czf "$PKG_NAME" -C "$OUT_DIR" .
echo ""
echo "=== Package created ==="
echo "File: /tmp/$PKG_NAME"
echo "Size: $(du -h /tmp/$PKG_NAME | cut -f1)"
echo ""
echo "Contents:"
tar -tzf "$PKG_NAME"

# 复制到项目目录方便访问
cp "/tmp/$PKG_NAME" "/home/aikon/danos-open/build/"
echo "Copied to: /home/aikon/danos-open/build/$PKG_NAME"