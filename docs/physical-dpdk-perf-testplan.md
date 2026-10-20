# DANOS-Open 物理机 DPDK 性能测试方案 (testpmd)

> **目标**：在现有 4×I211 物理机上跑通 testpmd 单核 64B 基线，产出 `PPS/Mbps/CPU%/桶分布` 证据，替代 `ENVIRONMENT-OPEN` 状态
> **限制**：I211 单队列、无 RSS，单核上限 ~3-4 Mpps，**非线速**，但可作为「有硬件证据」提交 release gate
>
> 配套脚本位于 [`tools/perf-testpmd/`](../tools/perf-testpmd/)，状态为草稿，尚未产生被验收记录采纳的结果；使用前请阅读该目录的 README。

---

## 1. 硬件拓扑需求

```
┌─────────────────────────────────────────────────────────────────┐
│  发包端 (R2)                    收包端 (R3)                     │
│  ─────────────────────          ─────────────────────            │
│  I211 P1 (0000:01:00.0)  ─────►  I211 P1 (0000:01:00.0)        │
│  I211 P2 (0000:02:00.0)  ─────►  I211 P2 (0000:02:00.0)        │
│                                                                  │
│  物理连线：R2 P1 ↔ 交换机 ↔ R3 P1                                │
│            R2 P2 ↔ 交换机 ↔ R3 P2                                │
│  (或直接回环线缆 R2 P1↔R3 P1, R2 P2↔R3 P2)                       │
└─────────────────────────────────────────────────────────────────┘
```

- **R2 (发包)**：需编译/部署 testpmd txonly
- **R3 (收包)**：需编译/部署 testpmd rxonly
- **R1/R4**：可作为额外收包端验证 ECMP 分布

---

## 2. testpmd 编译指导 (需在有 meson/ninja 的机器上)

### 2.1 依赖安装 (Debian/Ubuntu)
```bash
sudo apt-get update && sudo apt-get install -y \
    meson ninja-build pkg-config libnuma-dev python3-pyelftools \
    build-essential linux-headers-$(uname -r)
```

### 2.2 下载并编译 DPDK 24.07 (匹配 VPP 26.10)
```bash
cd /tmp
wget https://fast.dpdk.org/rel/dpdk-24.07.tar.xz
tar -xf dpdk-24.07.tar.xz
cd dpdk-24.07

# 静态编译 testpmd，仅启用 igb PMD (I211)
meson setup build \
    -Dexamples=testpmd \
    -Ddefault_library=static \
    -Denable_drivers=net/igb \
    -Dc_args="-O3 -static" \
    --prefix=/tmp/dpdk-install

ninja -C build
ninja -C build install
```

### 2.3 验证二进制
```bash
/tmp/dpdk-install/bin/testpmd --version
# 输出: testpmd version 24.07.0
file /tmp/dpdk-install/bin/testpmd
# 应显示: statically linked
```

### 2.4 打包部署到物理机
```bash
tar -czf testpmd-static.tar.gz -C /tmp/dpdk-install bin/testpmd
scp testpmd-static.tar.gz root@R2:/root/
scp testpmd-static.tar.gz root@R3:/root/
# R1/R4 同理
```

> **注意**：物理机内核需一致 (Debian trixie 6.6+)，静态链接可跨同内核版本运行

---

## 3. 物理机运行前准备

### 3.1 确认 DPDK 就绪 (串口执行)
```bash
# 检查 I211 绑定
cat /tmp/dpdk-pci-bind.log
# 应显示:
# DPDK-PCI-BIND PASS bdf=0000:01:00.0 pci_id=8086:1539 driver=uio_pci_generic
# DPDK-PCI-BIND PASS bdf=0000:02:00.0 pci_id=8086:1539 driver=uio_pci_generic

# 检查 hugepages
grep HugePages /proc/meminfo
# HugePages_Total: 28  (28 × 2MiB = 56MiB，足够 testpmd)

# 停止 VPP (释放接口给 testpmd)
systemctl stop vpp
# 或: killall vpp_main
```

### 3.2 解绑 VPP/igb_uio，绑定到 testpmd 可用的驱动
```bash
# testpmd 推荐使用 vfio-pci (需内核支持) 或 igb_uio
# 当前已绑定 uio_pci_generic，testpmd 可直接使用

# 确认接口状态
ip link show | grep -E "eth[0-9]|enp"
# 物理接口名可能是 eth0/eth1 或 enp1s0/enp2s0
```

---

## 4. testpmd 运行脚本

### 4.1 R2 发包端脚本 (`run_tx.sh`)
```bash
#!/bin/bash
# R2: 发包端 - 双端口 64B UDP 单核最大速率
# 运行前: systemctl stop vpp; 确保接口未被占用

set -euo pipefail

TESTPMD="/root/testpmd-static/bin/testpmd"
LOG="/root/testpmd_tx_$(date +%Y%m%d_%H%M%S).log"

# I211 两个 BDF (根据实际 lspci 调整)
BDF1="0000:01:00.0"
BDF2="0000:02:00.0"

# 测试参数
PKT_SIZE=64          # 64B (含 CRC 实际 60B payload + 4B FCS)
BURST=32             # 发包 burst 大小
CORES="1"            # 单核 (I211 单队列)
DURATION=30          # 持续秒数

# 目标 MAC (R3 对应端口 MAC，需提前获取)
# 可通过 testpmd rxonly 先跑一次拿到 MAC，或用 vppctl show int
DST_MAC1="00:1f:7a:69:f7:4c"  # R3 P1 MAC
DST_MAC2="00:1f:7a:69:f7:4d"  # R3 P2 MAC

# IP 地址 (仅用于构造包头，不走协议栈)
SRC_IP="10.10.0.1"
DST_IP="10.10.0.2"

cat <<EOF > /tmp/testpmd_tx_cmd.txt
set fwd txonly
set verbose 1
set burst $BURST
set txpkts $PKT_SIZE
set corelist $CORES
port config all speed 1000
start tx_first 32
# 手动配置每个端口的目标 MAC
port stop 0
port stop 1
set port 0 mac $DST_MAC1
set port 1 mac $DST_MAC2
port start 0
port start 1
# 设置发包模式: 连续发包，指定时长
set txonly 1
start
sleep $DURATION
stop
show port stats all
quit
EOF

echo "=== R2 TX Test Start: $(date) ===" | tee $LOG
$TESTPMD -l $CORES -n 4 \
    -a $BDF1 -a $BDF2 \
    --socket-mem=64,0 \
    --no-pci-scan \
    --file-prefix=tx \
    -- /tmp/testpmd_tx_cmd.txt 2>&1 | tee -a $LOG

echo "=== R2 TX Test End: $(date) ===" | tee -a $LOG
```

### 4.2 R3 收包端脚本 (`run_rx.sh`)
```bash
#!/bin/bash
# R3: 收包端 - 双端口统计计数器
# 运行前: systemctl stop vpp

set -euo pipefail

TESTPMD="/root/testpmd-static/bin/testpmd"
LOG="/root/testpmd_rx_$(date +%Y%m%d_%H%M%S).log"

BDF1="0000:01:00.0"
BDF2="0000:02:00.0"
CORES="1"
DURATION=35  # 比发包端多 5 秒缓冲

cat <<EOF > /tmp/testpmd_rx_cmd.txt
set fwd rxonly
set verbose 1
set corelist $CORES
port config all speed 1000
start
sleep $DURATION
stop
show port stats all
show port xstats all
quit
EOF

echo "=== R3 RX Test Start: $(date) ===" | tee $LOG
$TESTPMD -l $CORES -n 4 \
    -a $BDF1 -a $BDF2 \
    --socket-mem=64,0 \
    --no-pci-scan \
    --file-prefix=rx \
    -- /tmp/testpmd_rx_cmd.txt 2>&1 | tee -a $LOG

echo "=== R3 RX Test End: $(date) ===" | tee -a $LOG
```

### 4.3 ECMP 双路径分布测试 (`run_ecmp.sh`)
```bash
#!/bin/bash
# R2: 双端口发包，R3: 双端口收包，验证 ECMP 桶分布
# 发包端配置两个流，分别指向不同目标 MAC (模拟两个 NH)

cat <<'EOF' > /tmp/testpmd_ecmp_tx_cmd.txt
set fwd txonly
set verbose 1
set burst 32
set txpkts 64
set corelist 1
port config all speed 1000
port stop 0
port stop 1
# 两个流分别用不同目标 MAC (对应 R3 两个端口)
# 这里用 flowgen 方式: 每个端口发不同 MAC
set port 0 mac 00:1f:7a:69:f7:4c  # R3 P1
set port 1 mac 00:1f:7a:69:f7:4d  # R3 P2
port start 0
port start 1
start tx_first 32
sleep 30
stop
show port stats all
quit
EOF
```

---

## 5. 结果采集与报告格式

### 5.1 关键指标提取 (从 testpmd 输出解析)
```bash
# 从 testpmd 输出中提取:
#   TX-pps, RX-pps, TX-bps, RX-bps
#   TX-drops, RX-drops, RX-nombuf
#   per-port stats: ipackets, opackets, ibytes, obytes, imissed, ierrors, oerrors
#   xstats: rx_good_packets, tx_good_packets, rx_good_bytes, tx_good_bytes
```

### 5.2 标准化 JSON 输出 (`perf_result.json`)
```json
{
  "schema_version": 1,
  "test_type": "dpdk_testpmd_64b",
  "timestamp": "2026-10-19T14:30:00Z",
  "commit": "af41ef6",
  "hardware": {
    "nic": "Intel I211 (8086:1539)",
    "driver": "uio_pci_generic",
    "queues": 1,
    "rss": false,
    "cpu": "Intel Xeon E-2288G @ 3.70GHz",
    "cores_used": 1,
    "hugepages": "28x2MiB"
  },
  "topology": {
    "tx_node": "R2",
    "rx_node": "R3",
    "links": [
      {"tx_port": "P1 (0000:01:00.0)", "rx_port": "P1 (0000:01:00.0)"},
      {"tx_port": "P2 (0000:02:00.0)", "rx_port": "P2 (0000:02:00.0)"}
    ]
  },
  "parameters": {
    "packet_size": 64,
    "burst_size": 32,
    "duration_sec": 30,
    "core_list": "1"
  },
  "results": {
    "tx": {
      "port_0": {"pps": 3200000, "mbps": 1.64, "drops": 0, "cpu_pct": 99.8},
      "port_1": {"pps": 3100000, "mbps": 1.59, "drops": 0, "cpu_pct": 99.8}
    },
    "rx": {
      "port_0": {"pps": 3180000, "mbps": 1.63, "drops": 0, "nombuf": 0},
      "port_1": {"pps": 3080000, "mbps": 1.58, "drops": 0, "nombuf": 0}
    },
    "ecmp_distribution": {
      "port_0_pct": 50.5,
      "port_1_pct": 49.5,
      "note": "I211 单队列无 RSS，testpmd 指定目标 MAC 强制分流"
    }
  },
  "limitations": [
    "I211 single queue, no RSS - single core bottleneck",
    "Not line rate (14.88 Mpps @ 1Gbps 64B)",
    "Single core ~3-4 Mpps max",
    "Results are functional baseline, not production qualification"
  ],
  "performance_status": "BASELINE_ONLY",
  "evidence_sha256": {
    "tx_log": "...",
    "rx_log": "..."
  }
}
```

---

## 6. 串口操作实战清单

### 6.1 串口独占时序 (单串口 4 机轮流)
```bash
# 终端 1: 连接串口 (picocom/minicom/screen)
picocom -b 115200 /dev/ttyUSB0

# 操作序列 (每机约 10 分钟):
# 1. R2: 停止 VPP -> 解压 testpmd -> 跑 TX 30s -> 保存日志 -> 重启 VPP
# 2. R3: 停止 VPP -> 解压 testpmd -> 跑 RX 35s -> 保存日志 -> 重启 VPP
# 3. R1: (可选) 跑 RX 验证 ECMP
# 4. R4: (可选) 跑 RX 验证 ECMP
# 5. R2+R3: 同步跑 ECMP 分布测试
```

### 6.2 单次测试完整命令序列 (R2 示例)
```bash
# 1. 进入串口，登录 root
# 2. 停止 VPP
systemctl stop vpp
# 3. 确认接口空闲
ip link show
# 4. 解压 testpmd
cd /root && tar -xzf testpmd-static.tar.gz
# 5. 运行 TX 测试
chmod +x run_tx.sh && ./run_tx.sh
# 6. 等待完成，收集日志
ls -la testpmd_tx_*.log
# 7. 重启 VPP
systemctl start vpp
# 8. 验证 VPP 恢复
vppctl show int
```

---

## 7. 结果上传与验收

### 7.1 日志归档
```bash
# 开发机收集所有日志
scp root@R2:/root/testpmd_tx_*.log build/physical-dpdk-perf-$(date +%Y%m%d)/
scp root@R3:/root/testpmd_rx_*.log build/physical-dpdk-perf-$(date +%Y%m%d)/
scp root@R1:/root/testpmd_rx_*.log build/physical-dpdk-perf-$(date +%Y%m%d)/
scp root@R4:/root/testpmd_rx_*.log build/physical-dpdk-perf-$(date +%Y%m%d)/
```

### 7.2 生成结构化结果
```bash
# 使用项目现有的解析脚本或手工生成 perf_result.json
# 放入 build/physical-dpdk-perf-YYYYMMDD/perf_result.json
```

### 7.3 更新验收矩阵
在 `docs/v0.16-acceptance-matrix.md` 的 PCI/DPDK performance 行更新：
- 从 `ENVIRONMENT-OPEN` → `BASELINE_ONLY (I211 single-queue, ~3.2 Mpps/core)`
- 附上 JSON 证据链接

---

## 8. 后续：若采购到 X520/XL710 多队列网卡

```bash
# 多队列网卡 testpmd 命令差异:
# -l 1-4  (使用 4 个核心)
# --rxq=4 --txq=4  (4 个 RX/TX 队列)
# set fwd mac    (MAC 转发模式，自动交换 MAC)
# set flowgen    (流生成模式，多流并发)
# 支持 RSS: ethtool -X eth0 equal 4
# 可达线速 14.88 Mpps (单口 10G) 或 148.8 Mpps (单口 100G)
```

---

## 附录：常见问题排查

| 现象 | 原因 | 解决 |
|------|------|------|
| `EAL: cannot open /dev/uio0` | uio 模块未加载 | `modprobe uio_pci_generic` |
| `Cannot allocate hugepages` | hugepages 不足 | `echo 28 > /proc/sys/vm/nr_hugepages` |
| `Link down` | 物理链路未通 | 检查线缆、交换机配置、对端 MAC |
| `TX drops > 0` | 发包过快超出硬件能力 | 降低 burst、增加核心(受限于单队列) |
| `RX nombuf > 0` | 收包缓冲池耗尽 | 增加 `--socket-mem` 或 `--mbuf-size` |
| `Segmentation fault` | 静态链接库不兼容 | 在目标机内核版本一致的环境编译 |

---

## 执行检查清单

- [ ] 编译静态 testpmd (需有 meson/ninja 的机器)
- [ ] 打包部署到 R2/R3/R1/R4
- [ ] 串口轮流操作 4 台机器
- [ ] R2 TX 单口 30s 基线
- [ ] R3 RX 单口 35s 基线
- [ ] R2 双口 + R3 双口 ECMP 分布 30s
- [ ] 所有日志回收、SHA256 计算
- [ ] 生成 perf_result.json
- [ ] 更新 v0.16-acceptance-matrix.md
- [ ] 提交 PR 关闭 PCI performance `ENVIRONMENT-OPEN` (标注 BASELINE_ONLY)