# 2026-11-14 远程分支整合

权威目录 `/home/aikon/src/danos-open`。用户授权检查并合并 main 以外的三个远程分支。
本轮保留全部分支，不删除分支、不强制推送、不创建版本 tag 或 GitHub Release。

## 分支检查

| 分支 | 核对时末端 | 相对 main 的新增内容 | 处理 |
|---|---|---|---|
| main | `916491a` | 主线基准，已包含126个旧本地基线之后的提交 | 保留完整历史 |
| agent/advance-v016-tracks-34e6fd0a7534 | `6bb6bf3` | 无；已被 main 的 PR #28 合入 | 验证祖先关系，不重复应用 |
| docs/fix-gate-sha-916491a | `ca37022` | 修正克隆地址、把历史 gate PASS 精确绑定 `13fec36` | 快进到该文档提交 |
| dev/backend-readback-20261114 | `30ff960` | 发布链、后端强化、管理状态语义和文档四组提交 | 真实 merge，逐项解决重叠实现 |

## 冲突处理

- 统一为主线 `route_readback(resolved, out, user)`，不保留两个并列的路由验证 callback。
- VPP 保留主线 multipart dump/body callback、纯解码器、mock dump 测试及 VRF 表读回。
  实际接口/精确路由读取保留开发分支的 CRC 校验、连接锁和完整 ECMP 检查；传输层共用绝对期限。
- Linux 保留主线 IPv4/IPv6 接口地址、metric、完整 mock FIB、依赖撤销顺序及失败重试；
  合入32位 table、严格 multipart 精确读回、接口 admin 实况与超时后新 socket。
- 保留主线“重写后仍 VERIFY_FAIL 则不记成功”的检查；只有完成验证才清除重写失败预算。
  只读 IO 探测继续使用有界退避，保留 ledger，缺观察值时不伪造状态。
- 主线 NETCONF daemon、共享 session/锁、durable confirmed-commit、事务补偿和 gNMI 协议增强保留。
- 三项测试更新反映新的明确合同：默认路由现可通过 table dump 查询；attached 路径不编码零 gateway；
  状态转换单测显式关闭等待，退避行为由专门用例验证。未把原有失败改成跳过。
- 真实 VPP 老夹具显式声明 `1:1,2:2` 映射，不恢复生产 identity fallback。

## 本地验证

- Release 构建与 CTest **100/100 PASS**。
- ASan+UBSan 全量 CTest **100/100 PASS**，与普通 socket 测试分开执行。
- 隔离真实 VPP 26.10/Linux 夹具：严格映射、错 gateway、缺 ECMP 成员、成员重排、删路、admin 漂移、
  Linux table257/main 隔离通过；主线 VPP V1–V6（含 VRF 表读回、metric 不支持返回）通过。
- 软件互通、TLS、metrics、WAL 崩溃恢复结果见原始 `software.log`。

证据目录：`/home/aikon/src/danos-open-evidence/20261114-branch-integration/`。
测试记录绑定合并前候选的完整差异和二进制；最终 merge commit 映射在同目录补记。
本次没有重跑物理流量、大规模容量或整版 QEMU/VMware/PCI 发布资格。
历史发布 gate 仍只绑定 `13fec36`，不得因源码合并将其改记为本次提交通过。
