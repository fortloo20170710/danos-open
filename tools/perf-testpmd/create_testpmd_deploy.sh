#!/bin/bash
# testpmd 物理机部署包生成脚本
# 在开发机上运行，生成可直接部署到 R1-R4 的 tar.gz

set -euo pipefail

OUT_DIR="/tmp/testpmd-deploy-$(date +%Y%m%d)"
mkdir -p "$OUT_DIR"

cat > "$OUT_DIR/README.txt" <<'EOF'
========================================
DANOS-Open testpmd 物理机性能测试部署包
========================================

内容:
  testpmd-static/bin/testpmd    - 静态链接 testpmd 二进制
  run_tx.sh                     - R2 发包端脚本
  run_rx.sh                     - R3/R1/R4 收包端脚本
  run_ecmp.sh                   - ECMP 双路径分布测试
  collect_results.sh            - 结果采集脚本
  parse_testpmd.py              - 日志解析生成 JSON

部署方法:
  scp testpmd-deploy-YYYYMMDD.tar.gz root@R2:/root/
  scp testpmd-deploy-YYYYMMDD.tar.gz root@R3:/root/
  # R1/R4 同理

物理机上解压并运行:
  cd /root && tar -xzf testpmd-deploy-YYYYMMDD.tar.gz
  cd testpmd-deploy-YYYYMMDD
  chmod +x *.sh *.py
  # R2: ./run_tx.sh
  # R3: ./run_rx.sh
EOF

# 生成运行脚本 (将在下一步写入)
echo "部署包目录: $OUT_DIR"
ls -la "$OUT_DIR"