# DANOS-Open 待办清单(v0.16 收敛基线)

> 当前状态、版本路线和验收口径见 `docs/project-status.md`。本清单只保留尚未完成的可执行事项。

## v0.16 工程收敛
- [x] 统一 README、TODO、project status 与 v0.16 acceptance matrix 的当前状态口径
- [ ] 将当前 gNMI 修复拆分提交并完成协议回归

- [x] 增加 Interface IPv4/IPv6 地址模型及 northbound 测试（Linux/VPP 地址消息均已有类型化覆盖）
- [x] 增加特权集成测试 lane，并区分环境阻塞与代码失败（K4/K5 lane 已存在；本地缺少权限）
- [x] 增加 TSAN 门禁和正式 fuzz target（TSAN CMake/CI 配置及 gNMI 并发验证已完成；libFuzzer target/CI 已加入，当前工作区未安装 clang）
- [x] 完成真实磁盘 WAL fsync 基线（支持 `DANOS_WAL_BENCH_PATH`，CI 使用 `/var/tmp`）
- [x] 固定并验证 VPP runtime 版本（Debian trixie 源码构建，VPP 26.10-rc0~545-gad99177fe；mlx4/mlx5 disabled）

## 环境/特权依赖
- [x] K5-ping:root 容器中完成双 namespace 真实报文转发
      (run_v0.10_kernel.sh 的 K5 部分;沙箱 userns 拒绝对 moved veth 加地址)
- [x] VPP runtime/API/stat/J5 基线验收（有网络的 Debian trixie 特权容器）
      `run_v0.4_interop.sh --with-vpp`;adapter 已就位
      (danos_backend_ops "vpp",v0.11)
- [x] 真实 VPP CLI 双路径 ECMP FIB 安装/核查/撤销
- [x] 修复 VPP 26.10 binary-API socket framing/handshake，完成 API 驱动 ECMP 路由增删验收
- [x] VPP packet-level 双 namespace/af_packet/ICMP 转发验收

## 功能
- [x] v0.16 ECMP northbound route representation（gNMI `gateways[]` + NHGroup；真实 dataplane 多路径仍待）
- [x] FRR trixie BGP EVPN 邻居与 OSPF 邻居特权拓扑基线验收
- [~] v0.16 FRR BGP/OSPF → ZAPI → DPA → backend route install/withdraw 全链路验收
      （FRR 注册、真实 route notification 与 DPA 入口已验收；static 链路已在
      VPP 26.10 + FRR 10.3 上端到端通过：loop0/loop1 fixture、FRR static
      type=4、`10.99.0.0/24` API FIB 引用与撤路消失；BGP=10、OSPF=7 的默认
      映射已修正，但未重新跑 live BGP/OSPF 拓扑）
- [x] 真实 FRR zebra ZAPI socket 可达性验收（V5）
- [x] 增加可运行的 danos-fib daemon wiring，完成 live ZAPI → DPA → VPP route lifecycle 入口（`fib_live_bridge`；需特权拓扑验收）
- [x] 3 节点 BGP/ECMP、OSPF 收敛与 ping 验收（F1/F2）
- [x] WAL/DPA 重启恢复与 1000-object 事务规模验收
- [x] 特权容器软件转发基线记录（0.4363 Mpps；VPP+DPDK 性能仍需专用 dataplane lane）
- [ ] VPP+DPDK 专用 lane（已固化 preflight；RTL8168 经 VFIO/UIO 探测均无可用 DPDK PMD，需 VMXNET3/受支持 PCI NIC）

### 下一阶段执行顺序
- [x] 在 Debian trixie 特权 FRR+VPP 拓扑运行 `fib_live_bridge`，验收 route add/ECMP/withdraw/traffic/replay
- [x] 使用 `run_frr_zapi_vpp.sh` 固化真实 socket 验收日志与 VPP FIB/traffic 证据
- [x] 增加 FRR ZAPI HELLO client registration（真实 10-byte v6 header；bridge 已支持 `--reconnect`、信号退出）
- [x] 完成 FRR v6 live session 的 route subscription/notification 注册（router-id/interface replay、connected/static/OSPF/BGP redistribute）
- [x] 在带可用 VPP interface/path 的特权拓扑验收真实 route add/withdraw、ECMP、VPP FIB 和报文转发
      （adapter 已支持 `DANOS_VPP_IFINDEX_MAP=2:1`，单一路由 add 已实测成功）
      （bridge 已接入 programmed-ledger sweep，删除事件可调用 VPP route_del）
      （双 static next-hop 已实测进入 VPP 两个 resolved bucket；`--reconnect`
       场景下 add/delete 已实测完成；VPP ping 已 5/5、0% 丢包）
- [x] 固化 FRR+VPP 共享 socket 拓扑（readiness、健康检查、失败保留日志、bridge 生命周期）
      （bridge 已实现 VPP reconnect + desired replay；验收脚本已固化
       api.sock readiness 与 `0666` 权限 hook）
      （restart 后 programmed ledger 清理与 desired replay 已实测）
- [x] 对照 FRR 官方 zclient registration 核对 HELLO/REDISTRIBUTE_ADD、client identity、bitmap 和错误响应
- [x] 完成 Route/NH/NHGroup 依赖删除、tombstone、retry、rollback 的 Linux/VPP 双 backend 验收
- [ ] 在具备 PCI/VFIO、hugepages 和 DPDK plugin 的专用 runner 执行 VPP+DPDK 64B 单核/多核性能验收
      （已增加 trixie 容器 preflight；当前 PCI `0000:04:00.0` 仍绑定 r8169、runtime 未加载 DPDK）
- [ ] 在 VMware VMXNET3 guest runner 验证 `15ad:07b0` PCI NIC、VFIO 绑定、VPP DPDK plugin 和 64B 流量性能
      （QEMU/KVM 已验证 DANOS live guest 可见 `0000:00:02.0 [15ad:07b0]`；仍需注入含 VPP/DPDK 的 guest runtime）
- [x] 使用 QEMU `e1000` 验证当前 live ISO 虚拟网卡路径（`eth0`、DHCP、链路和默认路由）
      （仅为 ISO 网络 smoke test，不替代 VPP/DPDK 验收）
- [x] 为 live ISO 增加 `virtio-net` 及 failover 依赖并完成 QEMU virtio 网络 smoke test
- [ ] v0.16 收敛前冻结 OVS/P4、BFD 及大范围模型扩展，避免主链路分散
- [x] gNMI Subscribe STREAM 单连接多流状态机：增量 HEADERS/CONTINUATION 与
      DATA/gRPC 解析、per-stream 分派/取消、同连接 unary RPC 及多 STREAM 回归均已实现；
      剩余订阅语义限制（`use_models`、异构路径合同等）另行跟踪
- [ ] bfdd 翻译层(ADR-0006):DPA BFD 对象 ↔ frr bfdd 配置
- [x] 墓碑覆盖扩展：NHGroup 或成员 NH 从 desired 清除时，即使路由对象仍在 desired，也会先撤回依赖路由（route_del），再处理其他墓碑；reconciler 与 sweep 幂等重放、依赖恢复后重新编程均有回归
- [x] netlink IPv4 route presence OPER readback：检测外部撤路并触发同轮 desired replay
- [x] danos-netlink IPv6 地址编程与 OPER 读回：RTM_NEWADDR/RTM_DELADDR 携带 16 字节地址；重复添加幂等、缺失删除幂等、非法前缀拒绝；读取 IFA_LOCAL/IFA_ADDRESS 并按 scope 选择全球地址，mock 与真实 lo 回归覆盖
- [x] OPER 完整性扩展：netlink IPv4 路由 metric/ECMP 读回；VPP `sw_interface_dump`/`ip_table_dump`/`ip_route_dump` 的接口、VRF、路由 OPER 读回与差异检测；VPP dump 无 metric，非零 metric 显式返回 `DANOS_ERR_NOT_SUPPORTED`；均有 mock 解码/适配器回归，真实 VPP V6 lane 覆盖
- [ ] 模型表直接从 YANG 生成(当前经 JSON 规范中转)
- [x] NETCONF over SSH(RFC 6242 传输层，`danos-netconf-subsystem` + OpenSSH `Subsystem netconf`)
- [x] NETCONF candidate/startup 数据存储、commit/discard/lock/unlock/copy-config/delete-config
- [x] NETCONF validate（具名 datastore 与 inline config；对 modeled interface leaves 做语义校验）
- [x] NETCONF confirmed commit：共享 daemon 支持跨 SSH 会话 persist/persist-id、超时/会话关闭回滚；pending baseline 持久化到 `<startup>.confirmed`，启动时自动回滚未确认提交（RFC 6241 8.4）并有 fork/exec 重启回归
- [x] NETCONF committed running 持久化：plain/confirmed commit、running copy-config、直接 edit-config 与回滚都写 `<startup>.running`，重启加载；写失败使提交失败并回滚，fork/exec 重启回归覆盖
- [x] NETCONF 损坏 running 快照 fail-closed：拒绝启动并给出操作员可见错误；`DANOS_NETCONF_ALLOW_CORRUPT_RUNNING=1` 为显式 override，提交可修复坏文件
- [x] NETCONF 单守护进程托管：`danos-netconfd` 托管 candidate、锁、running 与 confirmed-commit 定时器，SSH subsystem 通过 Unix socket 代理；角色由 uid/group 策略决定并失败关闭，非 root socket 组权限与 systemd 配方已文档化，真实 sshd 双会话验收通过
- [x] NETCONF idle-session 超时：hello 后默认 300s 无流量即关闭，`--idle-timeout` / `DANOS_NETCONF_IDLE_TIMEOUT` 可配（0 关闭，上限 7 天）；关闭路径释放锁并回滚非 persist 的 confirmed commit，persist token 保留；daemon 与 in-process 回归覆盖

## 质量
- [x] libFuzzer 集成 target/CI（本地未安装 clang）
- [x] TSAN 门禁（32 并发连接用例）
- [x] 真实磁盘(非 tmpfs)上的 WAL fsync 基线（CI `/var/tmp`）
