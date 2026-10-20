# DANOS-Open Dependency Version Matrix

> G5: 依赖版本锁定。当前验证基线如下，任何升级须通过全量 CI + conformance。

## v0.16 验证基线

| 依赖 | 版本 | 备注 |
|------|------|------|
| FRR | 10.3.x | 通过 ZAPI socket 通信，不链接库 |
| VPP | 26.10 | Primary dataplane（Debian trixie） |
| DPDK | 24.07 | 与 VPP 26.10 配套；独立性能测试 lane 使用 |
| OVS | - | 未开始（v0.16 冻结） |
| P4Runtime | - | 未开始（v0.16 冻结） |
| Linux | 6.12+ | Debian trixie 内核；VFIO/IOMMU |
| GCC | 13+ | C11 |
| CMake | 3.16+ | |

## 版本锁定文件

构建时使用 `danos-build/deps/versions.lock` 锁定精确版本：

```ini
FRR_VERSION=10.3
VPP_VERSION=26.10
DPDK_VERSION=24.07
LINUX_MIN_VERSION=6.12
GCC_MIN_VERSION=13
CMAKE_MIN_VERSION=3.16
```

## 兼容性规则

1. **major** 版本变更需要 DANOS-Open major 升级
2. **minor** 版本变更在 CI 矩阵测试通过后接受
3. **patch** 版本变更自动接受（bug fix）
4. 任何依赖升级必须通过全量 CI + conformance 测试
