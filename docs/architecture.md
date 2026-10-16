flowchart LR
    %% NORTHBOUND
    subgraph NB["Northbound — Management Plane (danos-mgmt)"]
        CLI["CLI\nv0.2"]
        GNMI["gNMI gRPC\n(HTTP/2 + mTLS)\nv0.7"]
        NETCONF["NETCONF (SSH)\nv0.7"]
        AUTHZ["AuthN/AuthZ\n(RBAC, bearer, peercred, mTLS role map)\nv0.2"]
        MODELS["YANG/OpenConfig Models\nmodel_paths + model_routes"]
    end

    %% CORE
    subgraph CORE["Core Engine (danos-core)"]
        subgraph TXS["Transaction Subsystem (B3–B5)"]
            TXM["TX Manager\n(record pool, write mutex)"]
            TXAPI["TX API\nbegin→prepare→validate→\ncommit→verify/abort/release"]
            TXST["Staging Engine\ntx_stage_object / tx_read_staged /\ntx_iterate_objects"]
        end

        subgraph OBJS["Object Store (B1)"]
            OBJ["Object Store (rwlock+hash)\ng_default_store"]
            BATCH["Atomic Batch Publish\napply_batch /\napply_transaction_batch"]
        end

        STATE["State Store (B2)\nDESIRED | PROGRAMMED | OPER"]
        WAL["WAL/Persist (B6)\nwal.c + persist.c\nfsync + commit marker\nv0.3"]
        EVENT["Event Bus\n(pub/sub)"]
        CAP["Capability Registry\nv0.3"]
        PIPE["Programming Pipeline (ADR-0007)\ndesired→diff→backend→PROGRAMMED\nv0.10"]
        RECON["Reconciler (B8)\nrun_once, backoff, antiflap,\ntombstone sweep\nv0.6"]
    end

    %% SOUTHBOUND
    subgraph SB["Southbound — Backend Adapters"]
        VPP["VPP (danos-vpp) v0.11\nBinary API + Stat Segment + Mapper"]
        NL["Netlink (danos-netlink) v0.9\nrtnetlink (real/mock)"]
        FRR["DANOS FIB adapter (danos-fib)\nFRR zebra ZAPI client"]
        OVS["OVS-DPDK\n(not started)"]
        P4["P4Runtime\n(not started)"]
    end

    %% AUX
    subgraph AUX["Aux — HA/Security/Observability"]
        OBS["Observability (danos-observability)\nPrometheus + structured log v0.6"]
        SEC["Security (danos-security)\nRBAC/audit + CoPP v0.2"]
        HA["HA (danos-ha)\nBFD (via FRR) v0.2"]
    end

    MGRD["danos-mgrd\nSystem Daemon v0.6"]
    DP["Dataplane\nLinux + DPDK + NICs"]

    %% Connections NB->CORE
    CLI --> AUTHZ
    GNMI --> AUTHZ
    NETCONF --> AUTHZ
    MODELS --> TXAPI
    AUTHZ --> MODELS

    %% TX
    TXM --> TXAPI
    TXAPI --> TXST
    TXST --> OBJ
    TXAPI --> BATCH
    BATCH --> OBJ
    BATCH --> WAL
    BATCH --> EVENT
    OBJ --> STATE
    TXAPI --> STATE

    %% RECON/PIPE
    OBJ -.-> RECON
    RECON --> STATE & PIPE & OBJ & EVENT
    PIPE --> VPP & NL

    %% FRR
    ZEBRA["FRR zebra"] -->|ZAPI| FRR
    FRR -->|route/NH transaction| TXAPI

    %% EVENTS
    EVENT --> OBS & SEC & TXST & MODELS
    OBS --> VPP & STATE & MGRD

    %% MGRD
    MGRD --> WAL & RECON & AUTHZ & MODELS & VPP & NL & OBS

    %% DP
    VPP & NL --> DP
    OVS -.-> DP
    P4 -.-> DP

## 关键数据流

1. **配置路径（Northbound → Dataplane）**  
   CLI/gNMI/NETCONF → AuthZ（RBAC/证书验证） → YANG Models（路径注册 + 路由级联） → Transaction API（begin→prepare→validate） → Staging Engine（事务隔离与 read-your-writes） → Commit → Atomic Batch Publish（乐观冲突检测；启用持久化时先落 WAL/commit marker） → Object Store → DESIRED/PROGRAMMED 状态 → Programming Pipeline → VPP/Netlink → Dataplane。OPER 表示运行态视图；后端实际状态读回仍是独立验收项，不应由 DESIRED/PROGRAMMED 推断。

2. **收敛循环（Reconciler）**  
   Reconciler 定时/触发 `run_once()`，快照 DESIRED 并与 PROGRAMMED 对比，通过 Programming Pipeline 下发补救操作。自愈支持退避（backoff）和防抖（antiflap），执行 tombstone sweep 清理过期对象。

3. **外部路由学习（FRR ZAPI）**  
   FRR zebra 通过 ZAPI 套接字连接 DANOS `danos-fib` 客户端；客户端解析路由/NH/NHGroup 消息并经 Transaction API 写入 Object Store，再由 Programming Pipeline 编程到 VPP/Netlink。它不是直接向数据面写表的 backend。启用持久化时，事务 WAL 与 commit marker 在对象批次发布前按顺序持久化。

4. **可观测性（Observability）**  
   Prometheus `/metrics` 通过 stat_provider 聚合：管道/调谐器/gNMI 计数器 + VPP Stat Segment。Event Bus 向 Observability/Security 等订阅者发布事务/对象/调谐事件。

5. **持久化与恢复（WAL/Persist）**  
   持久化启用时，事务 commit 将操作与 commit marker 写入并 fsync 后才发布对象批次；启动恢复只回放有有效 commit marker 的事务，未完成事务不会发布。未启用持久化时，不应据此声称磁盘 WAL 保证。

## 设计要点

- **事务原子性（P0-2 已完成）**：所有 DPA 变更暂存在事务中，提交时一次性批量应用（`danos_object_apply_transaction_batch`），读操作优先返回暂存视图（read-your-writes）。
- **三态模型**：DESIRED（期望） → PROGRAMMED（后端编程账本） → OPER（运行态读回）。Reconciler 驱动期望与已编程状态收敛；完整、可信的后端 OPER readback 仍需按 acceptance matrix 验收，不能把账本状态当成硬件实况。
- **事务原子性（P0-2）**：DPA 变更暂存于显式事务；暂存视图优先于 committed store（read-your-writes），跨事务隔离；commit 对冲突进行检查，并以 `danos_object_apply_transaction_batch` 一次性发布。abort 丢弃暂存修改。
- **并发与收敛**：提交路径由全局写锁串行化，对象 store 使用读写锁；不可将其描述成所有读操作都绝不等待。Reconciler 提供 backoff/antiflap 与 tombstone sweep。
- **后端可插拔**：Programming Pipeline 的 southbound backend 包括 VPP 与 Linux rtnetlink；FRR ZAPI 是路由输入/控制面适配器，经 DPA 事务进入同一编程路径。OVS-DPDK、P4Runtime 预留（未启动）。
- **安全优先**：gNMI 默认要求 mTLS，证书指纹映射角色并执行 RBAC；明文 h2c 仅能通过显式实验室 opt-in 启用。生产 PKI 部署/轮换与实机管理面启用仍按发布矩阵单独验收。bearer token、peercred 等机制不可替代生产 mTLS 身份验证。
- **可观测与持久化**：Event Bus 与 Prometheus `/metrics` 提供事件和指标；启用持久化时，WAL 操作与 commit marker 在原子批次发布前 fsync，启动时仅恢复已提交事务。
