# 后端真实读回与漂移恢复（2026-11-14）

权威源码：`/home/aikon/src/danos-open`。本轮证据采集时工作区未提交，必须同时绑定补丁和新增文件；后续分组提交保留这一历史边界。

## 实现与边界

- 真实 VPP 启动必须有完整、合法的 `DANOS_VPP_IFINDEX_MAP`；缺失、坏尾缀、重复源、
  溢出、零端口均拒绝。允许多个 Linux 别名显式映射同一 VPP 端口；安装后不随 getenv 改变。
  只有不配置 map 的 mock 夹具保留 identity 映射。未知接口不会回退到 index 1。
- VPP 真实读回使用 `ip_route_lookup(exact=1)` 和 `sw_interface_dump` + control_ping barrier。
  请求与回复先按部署版本的 API CRC 校验，CRC 解析及 IO 在同一连接锁内完成。
  当前支持本轮验证的 VPP 26.10 schema；不认识的 schema 返回 NOT_SUPPORTED，不产生 OPER。
- IPv4 路由检查精确前缀、table、完整无序等权 ECMP 集合、gateway、出接口和支持的 path 语义。
  VPP 会将显式接口、零 gateway 的 attached 路径 flags 规范化为0，检查接受该等价表示。
  缺成员、错 gateway、错端口及外部删路都触发修复；仅成员重排不触发。
- Linux 采用 strict、指定表的 multipart route dump，避免普通 GETROUTE 的策略查询和覆盖路由
  被误当成指定表精确路由。只有完整 DONE 且无 DUMP_INTR/解析错误才发布观察结果。
  RTA_TABLE 支持32位表编号；表257已在隔离命名空间验证，不污染 main。
- 接口实际 admin/carrier、MTU等字段来自后端；当前自动修复只覆盖 admin 漂移。
  单纯 carrier down 是物理观察，不自动把它当成 admin 配置漂移。
- 只有已确认的 NOT_FOUND/VERIFY_FAIL 触发重新下发。IO/DOWN/不完整结果清除旧 OPER，
  保留 programming ledger、记录错误并退避。只读探测不会因编程重试次数用完而永久停止。
- VPP 连接/握手、单事务及 dump barrier 默认5秒绝对期限；包括部分帧、写阻塞和无关 context。
  Netlink send/receive 各有1秒上限；超时关闭 socket，后续请求用新连接及新 sequence。
  VPP stat socket 连接和收取 fd 同样有期限。TCP relay 使用数字 IP:port，不在锁内查询 DNS。
- gNMI 接口 `/state` 无观察时，叶子返回 NOT_FOUND，子树省略未观察接口；CLI OPER 为 unknown。
  配置路径仍可读取。PROGRAMMED 仍只表示后端接受应用，不等于转发验收。

仍不支持/未验收：VRF 的实际读回、VPP blackhole 编码（现在明确拒绝，避免伪成功）、
完整 IPv6 下一跳验证、接口地址/MTU自动纠偏、规模路由性能与物理 NIC 持续转发。
Linux 当前逐路由 dump 所选 table，需要后续批量快照优化；本轮不宣称大 FIB 性能资格。
默认 Netlink mock 的选择方式也仍待显式化。本轮没有升级或重打部署 ISO。

## 重跑

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure -j2
VPP_TEST_IMAGE=<local-vpp-26.10-dependency-image> \
  bash danos-test/integration/run_backend_readback_live.sh
```

dependency image 需要 vpp/vppctl、iproute2、sh、stdbuf 和可执行当前二进制的系统库。
脚本只使用新建容器的 network namespace，禁用 DPDK，创建两个 loopback 与 dummy 接口，
不挂宿主网卡、不使用宿主网络或 VM 磁盘；退出清理自己的容器。它是读回/修复功能验证，
不等于报文转发、物理硬件或整版发布资格。产物保存在 build/backend-readback.*，应复制到证据目录。

## Schema 依据

编码尺寸与 CRC 从本轮 dependency image 的 `/usr/share/vpp/api/core/{ip,interface,fib_types}.api.json`
核对。路由查找和错误返回行为参照上游：
[FDio ip_api.c](https://github.com/FDio/vpp/blob/master/src/vnet/ip/ip_api.c)、
[FDio error.h](https://github.com/FDio/vpp/blob/master/src/vnet/error.h)。
Linux 普通 GETROUTE 的策略查询边界参照
[Linux route.c](https://github.com/torvalds/linux/blob/master/net/ipv4/route.c)。
实际验收以记录的 image ID、API JSON 和当前补丁为准，不以 master 的未来状态替代。

## 本轮验证结果

- Release CTest 89/89；相关 ASan+UBSan 8/8（单独重跑后通过）。
- 独立 gnmic 互通、metrics、mgrd kill/restart/WAL 回放软件 gate 全 PASS。
- 隔离 VPP 26.10 与真实 Linux netns：错 gateway、缺 ECMP 成员、外部删路、admin 漂移修复通过；
  VPP 成员重排不误判；Linux table257 不污染 main。
- Linux admin-down 场景中，路由可能先于接口恢复而短暂得到 ENETUNREACH；测试要求在有界重试内
  恢复，而不是假设对象迭代顺序能保证同一 pass 全部成功。
- 曾同时启动普通与 sanitizer 两套测试，固定回环端口冲突导致一次 gnmi_grpc 启动失败；
  该失败日志保留，等待另一套退出后单独运行的8项全部通过。

完整证据在 `/home/aikon/src/danos-open-evidence/20261114-backend-readback/`。
证据采集时没有提交或推送；后续源码提交不等于正式发布或物理部署。
