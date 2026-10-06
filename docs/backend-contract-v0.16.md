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

这些都是确定性 mock/存储门，不能证明最新 build 在真实 VPP、FRR、QEMU、
VMware 或 PCI runner 上通过。依赖真实 runtime 的 ACK/FIB、packet、恢复与
性能证据仍需按 acceptance matrix 补齐；冻结的是目标语义，不是宣称所有
条目已经在所有 backend 验收。
