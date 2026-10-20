#!/bin/bash
# 一键编译 testpmd 静态二进制 (需 Docker)
# 运行后产出: /tmp/dpdk-out/testpmd

set -euo pipefail

IMAGE="dpdk-testpmd-builder"
OUT_DIR="/tmp/dpdk-out"

echo "=== Building testpmd static binary via Docker ==="
mkdir -p "$OUT_DIR"

# 编译镜像
docker build -t "$IMAGE" -f /home/aikon/danos-open/Dockerfile.testpmd /home/aikon/danos-open

# 运行提取二进制
docker run --rm -v "$OUT_DIR:/out" "$IMAGE" cp /testpmd /out/testpmd

# 验证
echo ""
echo "=== Verification ==="
file "$OUT_DIR/testpmd"
"$OUT_DIR/testpmd" --version
ls -lh "$OUT_DIR/testpmd"

echo ""
echo "✅ Binary ready at: $OUT_DIR/testpmd"
echo "Next: Run deploy package creation"