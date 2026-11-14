# DANOS-Open 项目状态

> 版本：v0.16.0-rc1 ｜ 更新：2026-11-14
>
> 本页只保留**当前状态**。按日期追加的完整验证记录已归档到
> [`docs/archive/project-status-log.md`](archive/project-status-log.md)。
> 验收门槛见 [`v0.16-acceptance-matrix.md`](v0.16-acceptance-matrix.md)，
> 发布判定逐行清单见 [`v0.16-release-decision.md`](v0.16-release-decision.md)，
> 待办见 [`outstanding-work-plan.md`](outstanding-work-plan.md) 与
> [`../TODO.md`](../TODO.md)。

## 分支整合（2026-11-14）

本轮在主线 `916491a` 上先快进文档修正 `ca37022`，再合入开发分支 `30ff960`。
`agent/advance-v016-tracks-34e6fd0a7534` 的末端 `6bb6bf3` 已包含于主线，未重复应用。
冲突解决统一了 `route_readback`，保留主线 NETCONF/事务、地址与 VRF 能力，以及开发分支的
严格映射、有界 IO、状态未知语义和发布链修复。集成候选 Release 与 ASan/UBSan CTest 均100/100、隔离真实
VPP/Linux回归通过。历史整版 gate 仍只绑定原记录的提交，不因合并自动升级。
详见 [分支整合记录](branch-integration-20261114.md)。

## 开发基线与推进顺序（2026-11-14 用户授权）

权威开发目录为 `/home/aikon/src/danos-open`。`/home/aikon/danos-open`
保留为独立历史 checkout，不自动同步、不引用其产物作为当前提交的验收证据。
本轮起点为 `d771a5a`；下面历史验证数字不自动升级为新改动的验证结果。
首批工作区完成 Release 构建、CTest 86/86、软件 release_check 与 OCI 冒烟，
证据在 `/home/aikon/src/danos-open-evidence/20261114-convergence/`。
后续独立读回分支的历史验证见 [后端读回](backend-readback.md)：当时 CTest 89 项，
新增严格 VPP map、CRC 校验读回、有界 IO、下一跳/ECMP 漂移修复和状态未知语义。
隔离 VPP 26.10 与 Linux 表257真实读回/修复脚本通过；这不是物理转发或性能资格。
新证据目录：`/home/aikon/src/danos-open-evidence/20261114-backend-readback/`。
上述证据采集于未提交工作区；后续分组提交不改变证据身份。源码与远程 CI 状态以 Git/GitHub 为准，未做正式发布。

按顺序推进：发布链与基线 → 后端正确性及实际读回 → 限定 L3 场景的恢复与
升级回滚 → 性能资格 → 场景驱动的功能扩展。执行与验收见
[开发收敛计划](development-convergence.md)。这不改变既有正式发布门槛。

## 1. 发布判定

**v0.16.0-rc1：未完成发布资格认证。** 控制面功能闭环可用，物理/虚拟
性能与恢复实验仍有关口未通过。最终 v0.16.0 发布前不得声明生产就绪。

已通过的工程验证：

- x86_64 / aarch64 构建，`-Wall -Wextra -Wpedantic -Werror`
- 当前集成候选 Release CTest **100/100**；历史主线96/96及其CI结果保持原提交绑定
- gNMI 互通 I1–I9（gnmic），32 并发连接
- 内核 netns 路由编程 K4/K5，真实 VPP API V1–V5
- YANG 模型校验、WAL 磁盘基线
- 生产 C 覆盖率 81.1%（函数 91.6%、分支 60.3%），Coverage 门禁 79%
- libFuzzer 目标：gNMI protobuf、HPACK、HTTP/2 帧头（CI 构建并冒烟运行）
- 根 `Dockerfile` 源码镜像：构建阶段 CTest **96/96**，运行时 `/metrics` 冒烟通过

## 2. 模块状态

| 模块 | 状态 | 说明 |
|------|------|------|
| danos-dpa | 稳定 | DPA 接口与错误模型 |
| danos-core | 稳定 | 对象注册表、四态模型、事务、WAL、reconciler；NHGroup/NH 墓碑会级联撤回依赖路由，reconciler/sweep 幂等重放 |
| danos-mgmt | 稳定 | gNMI gRPC/HTTP2、mTLS、Unix peercred、CLI、NETCONF over SSH（单一 `danos-netconfd` 托管 candidate/锁/跨会话 confirmed-commit；committed running 与 pending rollback 均持久化、重启后加载/回滚；损坏 running 快照 fail-closed 并可显式 override；角色按 peer uid 策略派生并失败关闭；hello 后 idle-session 默认 300s 超时） |
| danos-fib | 可用 | FRR ZAPI v6 解析与映射 |
| danos-vpp | 可用 | binary API、路由/接口编程、统计、interface/table/route dump OPER 读回 |
| danos-netlink | 可用 | IPv4 路由/接口；IPv4/IPv6 地址编程与 OPER 读回（重复添加、幂等删除、非法前缀拒绝）；路由 metric 与 ECMP 成员 OPER 读回 |
| danos-security | 可用 | RBAC、审计、CoPP；权限粒度仍为对象级 |
| danos-observability | 可用 | Prometheus `/metrics`（默认回环监听）、结构化日志 |
| danos-ha | 部分 | BFD/VRRP/supervisor 内存模型；bfdd 翻译未完成 |
| danos-platform / ovs / p4 | 未开始 | v0.16 有意冻结 |

## 3. 已知限制

- PCI/DPDK 持续性能测试仍标记 `ENVIRONMENT-OPEN`，缺少独立接收端数据。
- 物理 R2→R3 恢复、FRR→DPA→VPP 动态路由全链路、四节点统一部署尚未完成。
- VMware VMXNET3 lane 仍需在 v0.16 发布前重跑。
- IPv4 路由 OPER 读回使用 `RTM_F_FIB_MATCH` 核验前缀/表项、黑洞/单播类型、metric 与 ECMP 成员；缺失或差异都会触发同轮幂等 repair。VPP 通过 `sw_interface_dump`/`ip_table_dump`/`ip_route_dump` 提供接口、VRF 和路由 OPER 读回；VPP dump 不含 metric，非零 metric 明确返回 `NOT_SUPPORTED`（mock 与 V6 测试覆盖），metric 0 路由仍校验前缀/路径/类型。真实 VPP 的 wire 路径由 V6 lane 覆盖。
- 损坏的 `<startup>.running` 会拒绝启动；需修复/删除该文件，或显式设置 `DANOS_NETCONF_ALLOW_CORRUPT_RUNNING=1` 以空配置启动，下一次成功提交会覆盖坏文件。
- 测试证书需替换为生产 PKI。
- BFD、VLAN、VXLAN、EVPN、OVS、P4 功能未纳入 v0.16。
- 事务提交后保留 pre-commit 快照，`danos_tx_rollback` 可恢复 desired store 并重跑 reconciler；仅当快照已释放、逆批次冲突/应用失败或 backend 无法编程恢复值时返回 `DANOS_ERR_TX_ROLLBACK`（backend 失败时 store 已恢复）。进程重启不会保留内存快照；重启后的 desired state 是 WAL 已发布/已回滚状态。

## 4. 复现入口

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DDANOS_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

安全与并发验证使用独立构建目录：

```bash
cmake -B build-asan -DDANOS_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake -B build-tsan -DDANOS_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
```
