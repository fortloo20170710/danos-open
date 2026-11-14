# 开发收敛执行计划

2026-11-14：用户指定权威开发目录 `/home/aikon/src/danos-open` 并授权按评估顺序推进。
起点 `d771a5a`。另一 checkout 不自动同步；danos 2608 的产物和证据不混用。

## 1. 基线与发布链

- Release 和 Interop 共用 `danos-build/stage_mgrd_oci.sh` 创建全新最小 context。
- Release 从 tools/gnmic 的 go.mod/go.sum 构建独立客户端，并供应 TLS 互通依赖。
- `workflow_dispatch` 只做软件验证与打包演练，不创建 GitHub Release。
- tag 路径必须额外通过统一 QEMU/VMware/PCI gate；缺环境仍阻断正式发布。
- package.json 记录 commit、dirty、补丁及未跟踪文件摘要、依赖文件和产物 SHA256。
  它不是签名、完整可复现构建证明或发布资格报告。基础镜像/apt 尚未快照锁定。
- 日志及摘要作为 CI artifact 保存90天；正式发布证据仍需另行长期归档。

本地软件打包演练：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
(cd tools/gnmic && GOFLAGS=-mod=readonly go build -o ../../build/gnmic github.com/openconfig/gnmic)
mkdir -p build/release-evidence
set -o pipefail
bash danos-test/integration/release_check.sh 2>&1 | tee build/release-evidence/software.log
bash danos-build/stage_mgrd_oci.sh build/release-evidence/oci-context
docker build -f danos-build/Dockerfile.mgrd -t danos-mgrd:local-dry-run build/release-evidence/oci-context
docker save danos-mgrd:local-dry-run | gzip > build/release-evidence/danos-mgrd.tar.gz
python3 danos-build/record_package_manifest.py --artifact build/release-evidence/danos-mgrd.tar.gz --out build/release-evidence/package.json
```

context 必须不存在；重跑时选择新目录，不覆盖上次证据。

## 2. 后端正确性

按可独立验收的增量修复：大路由表编号；VRF 不支持时明确拒绝；ECMP 编码；
撤销失败保留 ledger；VPP 映射缺失拒绝；显式 backend 模式；有界 IO；实际读回。
每个增量必须有失败路径回归保护，不把 mock 成功作为真实转发证据。

退出：错 gateway、ECMP 缺成员、外部删路可见；失败可重试；恢复后的状态与独立流量一致。

## 3. L3 交付与恢复

确定主后端及支持矩阵，分别验证 mgrd/FRR/VPP 重启、配置冷恢复、安装、升级和回滚。
NETCONF 若作为正式能力，须统一跨会话 store/锁和 confirmed-commit 恢复语义；
否则收紧能力宣告。场景范围不默认为已通过资格。

## 4. 性能与扩展

在专用环境完成持续流量、独立 RX、路由 churn、资源长稳和恢复窗口测试。
缺硬件保持 OPEN。通过后按场景考虑 BFD/VLAN，不同时扩展全部后端和 overlay。

## 本轮已完成的增量

- 发布流程供应锁定 gnmic 与 socat，复用最小 OCI staging；手动演练不发布。
- 真实 Netlink 路由 add/delete/read 使用 32 位 RTA_TABLE，不再将 >=256 映射 main。
- ECMP 编码为 RTA_MULTIPATH，支持 resolved-route 合同内最多64个等权成员。
- 真实 Netlink VRF 生命周期尚未实现，明确返回 NOT_SUPPORTED；mock 仍可用于模型测试。
- 接口 admin-down 失败保留 ledger 与 OPER，下次 sweep 继续重试。
- 新增真实 adapter 序列化回归与接口撤销故障注入。序列化测试不代表真实转发验收。

后续已实现：严格 VPP 映射、受支持 schema 的真实读回、有界 IO、IPv4 下一跳/ECMP
漂移检测和修复；gNMI/CLI 缺少观察时不再回退期望状态。隔离 VPP/Linux 真实功能回归通过，
详见 [后端读回](backend-readback.md)。
仍待：默认 Netlink 模式显式化、更广对象/IPv6语义、读回批量化、安装恢复和真实性能资格。
当前不能宣布整版生产就绪。
