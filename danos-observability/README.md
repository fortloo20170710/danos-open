# danos-observability — 可观测性与 Telemetry

> 设计依据：`docs/archive/v1.1/observability/observability_telemetry.md`（第 22 章）。

## 已实现（v0.16）

- **Prometheus exporter**：`src/prometheus.c`，128 项指标注册表、外部 stat provider
- **HTTP `/metrics` 服务**：`src/prom_server.c`，带连接上限、读写超时；
  默认仅监听 `127.0.0.1`，用 `DANOS_METRICS_BIND` 显式开放远程抓取
- **结构化日志**：`src/log.c`
- **gNMI Subscribe**：由 `danos-mgmt` 的 gRPC 栈提供（ONCE/STREAM 子集）
- **审计**：由 `danos-security/src/audit.c` 提供 JSON-lines 审计日志

## 尚未完成

- OpenTelemetry 分布式追踪、IPFIX/NetFlow 导出
- Alertmanager 规则与 Grafana dashboard 模板
- 指标基数与标签治理（当前为固定名称的扁平指标）

## 接口边界

- 与 `danos-core`：订阅事件总线、Transaction 生命周期
- 与 `danos-mgmt/gnmi`：gNMI Subscribe 服务端实现
- 与 `danos-security`：审计日志接入
