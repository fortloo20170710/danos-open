# DANOS backend contract v0.16

本文冻结 desired-state/reconciler 与 Linux、VPP backend 之间的行为契约。

## 1. 调用与对象生命周期

- backend 由 `danos_backend_ops_set()` 在 reconciler 启动前安装；`name` 用于
  日志和能力报告。
- `iface_up`、地址、VRF、route add/del 的参数是调用时快照，backend 不得保存
  指针；需要异步处理时必须复制数据。
- 调用必须幂等：重复 add/update 产生相同最终状态，重复 delete 返回成功或
  明确的 not-present 语义，不得留下脏对象。
- 依赖顺序为 VRF → interface/admin/address → next-hop/NHGroup → route；删除
  顺序反向。route 没有可解析路径时不得伪造可达路径。

## 2. 状态、错误与重试

- `DANOS_OK` 表示目标状态已确认或已经满足；`DANOS_ERR_INVALID_ARG`、
  `DANOS_ERR_NOT_FOUND`、`DANOS_ERR_UNSUPPORTED` 表示不可通过重试修复的
  永久错误。
- socket/API 暂不可用、设备重启、临时资源不足属于 transient failure；backend
  返回失败后由 reconciler 按指数退避重试，不能自行无限阻塞。
- backend 不得把“发送成功”当成“编程成功”：VPP/Netlink 的 reply/ACK 必须
  校验；错误计数必须进入 programming statistics。
- 单对象失败不应阻塞同批次无依赖对象；依赖对象失败时，下游对象保持未编程
  并在下一轮重试。

## 3. 删除、回滚与重启

- desired object 消失后，programming ledger 生成 tombstone，并调用对应 del；
  删除成功后才移除 ledger。路由删除必须覆盖单路径、ECMP、NH/NHGroup。
- add/update 的部分失败不得报告成功；能安全回滚时回滚本次批次，否则保留
  可重试状态并暴露失败对象与原因。
- dataplane 重启后必须忘记 programmed 标记而保留 desired state，完成 API
  reconnect 后 bounded replay；replay 可重复且不会产生重复路径。

## 4. 能力与观测

- backend 能力必须显式报告或由 conformance test 声明：IPv4/IPv6、VRF、ECMP、
  weighted ECMP、attached next-hop、restart replay、atomicity。
- 必须记录 backend、对象类型/id、操作、结果、错误分类、重试次数和耗时；
  不得记录敏感凭据。
- conformance gate 至少覆盖 add/update/delete 幂等、ECMP、不可解析 NH、
  restart replay 和 ledger sweep；Linux 与 VPP 都必须执行同一语义集合。

## 5. v0.16 冻结范围

本版本冻结上述生命周期、错误/重试、删除/tombstone、重启 replay 和观测语义。
VLAN/VXLAN/EVPN/BFD 可增加能力和对象类型，但不得改变既有 route/NH/NHGroup
语义；扩展前必须补充 capability、依赖顺序及 conformance case。

## 6. 2026-10-07 契约回归与边界

Route/NH/NHGroup 的 DPA 存储门现验证：创建/读回、重复创建 EXISTS、
缺失 update/delete NOT_FOUND、候选读写与跨事务隔离、update/delete abort、
提交后的完整 payload 可见性、删除后的缺失以及事务记录回收。
这里的 DPA DELETE NOT_FOUND 与 backend 幂等删除是不同层级，不能混用。

retry budget 属于特定 desired payload 和依赖图摘要。NH/NHGroup 恢复或修改后，
未显式 anti-flap parked 的对象获得新预算；相同失败图仍执行退避/耗尽限制。
dataplane restart 的 forget-programmed 通知创建新的编程 epoch，清理旧预算，
包括此前 API 不可用而未进入 ledger 的对象；desired graph 不被删除。

programming_pipeline 增加 VPP mock 的静默幂等、forget/replay、NHGroup 撤销/
恢复、预算耗尽后依赖恢复、未进入 ledger 对象的 epoch reset 断言。
Linux mock 验证耗尽预算后的 NHGroup 恢复及实际 mock FIB presence。

这些确定性 mock/存储门本身不能证明真实 backend runtime 通过。独立的
QEMU/FRR/VPP runtime 证据与当前剩余的 PCI 边界见下节；冻结的是目标语义，
并不宣称所有硬件 backend 都已验收。

## 7. 2026-10-08 运行时符合性历史快照（当前状态见 §8）

当前共享分支 `8776da3` 的本机全量 CTest 为 52/52 PASS，且
`danos-test/integration/run_backend_contract.sh` 的 DPA、programming pipeline、
复合 Route/NH/NHGroup 三段确定性 contract gate 全部通过。此 gate 使用 mock
adapter，不应被表述为真实 PCI backend 验收。

最新可复核的 clean QEMU 10 ms 运行来自源码
`3881730e6b79c1dac41f58b767b86df5069f2d07`：ISO
`danos-vpp-dpdk-e1000-2port-ecmp-soak-1000-3881730-baseline.iso`，SHA256
`cf63bcd23ef01f63723cd46616ab468a068aceb5e406426e5abcff624ed659ae`。严格 verifier
对 live 日志及 `accepted-final/` 冻结证据均通过，覆盖 FRR BGP/OSPF、ZAPI route
add/withdraw/restore、ECMP next-hop down/up、FRR/zserv 重启、VPP restart/replay
和重启后双 peer 探测。四流共 4000/4000、0% 丢包，81.47 pps，bucket delta
3000/1000；RTT p50/p99 为 371.10/1694.50 us。该结果是 QEMU 功能/回归基线，
不是 PCI 吞吐或线速成绩。结构化结果、manifest 和日志 digest 见
`docs/v0.16-acceptance-matrix.md`。

2026-10-08 随后的实机观察中，串口线和网线已报告接入，但在未捕获到实机启动
事件的 180 秒监听里 `/dev/ttyUSB0` 仍为 0 bytes；日志
`build/physical-current-20261008Tserial-connected-2.serial.log` 的结果为
`SKIP`（缺少 `DANOS-INIT-ENTER`）。因此不能据此判定 I211 bind、carrier 或报文
转发。真实 PCI DPDK preflight/performance 仍为 `ENVIRONMENT-OPEN`；QEMU 与
mock 证据不替代该物理 lane，也没有 waiver 将其改为 PASS。

## 8. 2026-10-09 当前验收复核

在 clean shared commit `b86aa6d216277ae428674e36c8c37e0a74b91359` 上重新执行
`danos-test/integration/run_backend_contract.sh`，DPA 11/11、programming pipeline
和 composite Route/NH/NHGroup lifecycle 三段均 PASS。完整本机 CTest 为 53/53。
该结果确认 v0.16 contract/conformance 门仍稳定，不扩大 §5 冻结的语义范围。

同一轮复核中，identity-bound QEMU 拓扑
`build/qemu-frr-vpp-topology-705c70b-baseline` 的严格 verifier 通过 FRR BGP/OSPF、
ZAPI route add/withdraw/restore、ECMP 路径撤销/恢复、FRR/zserv 重启、VPP restart/replay
及重启后双 peer 报文探测。四流 4000/4000、0% loss，bucket delta 3000/1000；指标仅是
QEMU 功能回归，非 PCI 性能。VMware VMXNET3 polling-only 默认 verifier 也通过
2000/2000、0% loss、98.23 pps、p50/p99 206/449 us；它同样是低速报文回归，不是线速
或高负载 ECMP 成绩。结果、ISO digest 和范围说明见
`docs/v0.16-acceptance-matrix.md`。

GitHub CI、Coverage、Interop Gate 对该提交均 PASS（runs `37819360359`、
`37819360271`、`37819360543`）；Interop Gate 包含 hosted VPP V1–V5 API 互通及 K4/K5
真实 netns/ping。Hosted VPP job 不等同于物理 NIC/DPDK 性能 qualification。

本轮没有使用物理串口，也没有新增物理 runner 证据。因此真实 I211 PCI DPDK
preflight/64-byte 测量、物理安装及线速资格仍为 `ENVIRONMENT-OPEN`；mock、QEMU、
VMware polling 和 hosted VPP 的 PASS 均不得替代这些门。
