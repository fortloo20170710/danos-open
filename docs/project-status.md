# DANOS-Open 项目状态

> 版本：v0.16.0-rc1 ｜ 更新：2026-10-21
>
> 本页只保留**当前状态**。按日期追加的完整验证记录已归档到
> [`docs/archive/project-status-log.md`](archive/project-status-log.md)。
> 验收门槛见 [`v0.16-acceptance-matrix.md`](v0.16-acceptance-matrix.md)，
> 待办见 [`outstanding-work-plan.md`](outstanding-work-plan.md) 与
> [`../TODO.md`](../TODO.md)。

## 1. 发布判定

**v0.16.0-rc1：未完成发布资格认证。** 控制面功能闭环可用，物理/虚拟
性能与恢复实验仍有关口未通过。最终 v0.16.0 发布前不得声明生产就绪。

已通过的工程验证：

- x86_64 / aarch64 构建，`-Wall -Wextra -Wpedantic -Werror`
- 全量 CTest 85 项（普通构建 2026-10-21；ASan+UBSan 81 项，2026-10-20；本轮两个定向 ASan 用例通过）
- gNMI 互通 I1–I9（gnmic），32 并发连接
- 内核 netns 路由编程 K4/K5，真实 VPP API V1–V5
- YANG 模型校验、WAL 磁盘基线
- 生产 C 覆盖率 81.0%（函数 91.1%、分支 60.1%），Coverage 门禁 79%
- libFuzzer 目标：gNMI protobuf、HPACK、HTTP/2 帧头（CI 构建并冒烟运行）
- 根 `Dockerfile` 源码镜像：构建阶段 CTest 81/81，运行时 `/metrics` 冒烟通过（2026-10-20 本地验证）

## 2. 模块状态

| 模块 | 状态 | 说明 |
|------|------|------|
| danos-dpa | 稳定 | DPA 接口与错误模型 |
| danos-core | 稳定 | 对象注册表、四态模型、事务、WAL、reconciler |
| danos-mgmt | 稳定 | gNMI gRPC/HTTP2、mTLS、Unix peercred、CLI、NETCONF over SSH（单一 `danos-netconfd` 托管 candidate/锁/跨会话 confirmed-commit；confirmed baseline 重启后回滚；角色按 peer uid 策略派生并失败关闭） |
| danos-fib | 可用 | FRR ZAPI v6 解析与映射 |
| danos-vpp | 可用 | binary API、路由/接口编程、统计、interface/table/route dump OPER 读回 |
| danos-netlink | 可用 | IPv4 路由/地址/接口；路由 metric 与 ECMP 成员 OPER 读回；IPv6 地址明确返回不支持 |
| danos-security | 可用 | RBAC、审计、CoPP；权限粒度仍为对象级 |
| danos-observability | 可用 | Prometheus `/metrics`（默认回环监听）、结构化日志 |
| danos-ha | 部分 | BFD/VRRP/supervisor 内存模型；bfdd 翻译未完成 |
| danos-platform / ovs / p4 | 未开始 | v0.16 有意冻结 |

## 3. 已知限制

- PCI/DPDK 持续性能测试仍标记 `ENVIRONMENT-OPEN`，缺少独立接收端数据。
- 物理 R2→R3 恢复、FRR→DPA→VPP 动态路由全链路、四节点统一部署尚未完成。
- VMware VMXNET3 lane 仍需在 v0.16 发布前重跑。
- IPv4 路由 OPER 读回使用 `RTM_F_FIB_MATCH` 核验前缀/表项、黑洞/单播类型、metric 与 ECMP 成员；缺失或差异都会触发同轮幂等 repair。VPP 通过 `sw_interface_dump`/`ip_table_dump`/`ip_route_dump` 提供接口、VRF 和路由 OPER 读回；VPP dump 不含 metric，非零 metric 明确返回 `NOT_SUPPORTED`（mock 与 V6 测试覆盖），metric 0 路由仍校验前缀/路径/类型。真实 VPP 的 wire 路径由 V6 lane 覆盖。
- 测试证书需替换为生产 PKI。
- BFD、VLAN、VXLAN、EVPN、OVS、P4 功能未纳入 v0.16。
- 事务提交后的 `danos_tx_rollback` 返回 `DANOS_ERR_TX_ROLLBACK`（无法撤销），
  调用方应在提交前使用 abort。

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
