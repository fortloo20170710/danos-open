# danos-ha — 高可用

> 设计依据：`docs/archive/v1.1/ha/ha_design.md`（第 21 章）。

## 已实现（v0.16）

- **BFD 会话模型**：`src/bfd.c`，会话创建/更新/删除与状态机（内存态）
- **VRRP**：`src/vrrp.c`，虚拟路由实例与优先级/抢占逻辑，支持 BFD 联动字段
- **进程 supervisor**：`src/supervisor.c`，受管进程的重启策略

## 尚未完成

- 与 FRR `bfdd` 的翻译层（北向 BFD 配置 → bfdd → DPA），见 `TODO.md`
- NSR / Graceful Restart、EVPN-MH、ISSU、双路由引擎状态同步
- 当前 BFD/VRRP 状态只存在于控制面内存，未下发到数据面

## 接口边界

- 与 `danos-core`：HA 事件经事件总线订阅/发布
- 与 `danos-fib`：FRR NSR 状态同步（规划中）
- 与 `danos-vpp`：数据面状态快照/恢复（规划中）
