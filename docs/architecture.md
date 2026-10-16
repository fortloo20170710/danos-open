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
        FRR["FRR FIB (danos-fib) v0.2\nZAPI (zebra)"]
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
    AUTHZ --> GNMI & CLI & NETCONF
    MODELS --> TXAPI

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
    FRR <--> TXAPI
    FRR --> DP

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
   CLI/gNMI/NETCONF → AuthZ（RBAC/证书验证） → YANG Models（路径注册 + 路由级联） → Transaction API（begin→prepare→validate） → Staging Engine（暂存，读写视图重叠） → Commit → Atomic Batch Publish（乐观冲突检测 + WAL 写入 + commit marker + 事件发布） → Object Store → State Store（DESIRED/PROGRAMMED/OPER） → Programming Pipeline → 后端适配器（VPP/Netlink） → Dataplane。

2. **收敛循环（Reconciler）**  
   Reconciler 定时/触发 `run_once()`，快照 DESIRED 并与 PROGRAMMED 对比，通过 Programming Pipeline 下发补救操作。自愈支持退避（backoff）和防抖（antiflap），执行 tombstone sweep 清理过期对象。

3. **外部路由学习（FRR ZAPI）**  
   FRR zebra 通过 ZAPI 套接字与 `danos-fib` 通信，解析路由/NH/NHGroup 消息，经 Transaction API 写入 Object Store 并在 commit 时持久化到 WAL，由 Programming Pipeline 编程到 VPP/Netlink。

4. **可观测性（Observability）**  
   Prometheus `/metrics` 通过 stat_provider 聚合：管道/调谐器/gNMI 计数器 + VPP Stat Segment。Event Bus 向 Observability/Security 等订阅者发布事务/对象/调谐事件。

5. **持久化与恢复（WAL/Persist）**  
   每笔事务 commit 前将完整事务写入 WAL 并写入 commit marker（fsync 保证顺序），mgrd 启动时调用 `persist_recover()` 回放未提交之前的持久化配置，确保崩溃后可恢复。

## 设计要点

- **事务原子性（P0-2 已完成）**：所有 DPA 变更暂存在事务中，提交时一次性批量应用（`danos_object_apply_transaction_batch`），读操作优先返回暂存视图（read-your-writes）。
- **三态模型**：DESIRED（期望） → PROGRAMMED（已编程到后端） → OPER（实际运行状态），配合 Reconciler 做期望-实际校验。
- **单写多读**：全局 commit 写锁 + 对象存储读写锁，读路径无阻塞。
- **后端可插拔**：通过 Backend Ops 抽象支持 VPP、Netlink（rtnetlink）、FRR ZAPI；OVS-DPDK、P4Runtime 预留（未启动）。
- **安全优先**：gNMI 强制 mTLS（可选实验室明文模式），支持 bearer token、peercred、证书角色映射和 RBAC/audit。
