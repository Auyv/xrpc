# 贡献指南

## 本地检查

公开仓库不含测试工程。提交前至少确认核心库能编译、工具能生成并编译仿真：

```bash
python tools/xrpc_port_gen.py generate tools/examples/port_custom.yaml --out out/check/port
```

```bash
cmake -S . -B out/check/lib -G Ninja -DXRPC_PORT_DIR=out/check/port/port_include
```

```bash
cmake --build out/check/lib
```

```bash
python tools/xrpc_net_gen.py sim tools/examples/net_demo.yaml --out out/check/sim
```

```mermaid
flowchart LR
    A["设计"] --> B["确认"]
    B --> C["实现和测试"]
    C --> D["看过测试结果"]
    D --> E["提交"]
```

## 规则

| 项 | 要求 |
| --- | --- |
| 改协议 | 先讨论设计。帧格式、消息类型、字段含义都是协议，由黄金字节测试守住 |
| 内存 | 核心库不用动态内存，缓冲区由调用方提供 |
| 依赖 | 核心库不引入新依赖，工具只用 Python 标准库；可选功能例外，缺少时要提示 |
| 风格 | C99，Allman 大括号，4 空格缩进，单一出口，静态函数用 `L_` 前缀，注释用中文 |
| 提交 | 一次提交只做一件事，写明线上协议影响和测试情况 |
| 文档 | 给人看的文档要有图 |

## 版本

版本号只写在 `common/include/xrpc/xrpc_common.h`。协议版本 `XRPC_PROTOCOL_VERSION` 单独维护，两者互不影响。

| 改动 | 1.0.0 之前 | 1.0.0 之后 |
| --- | --- | --- |
| 不兼容的接口或协议 | 升次版本 | 升主版本 |
| 新功能 | 升次版本 | 升次版本 |
| 修复 | 升修订版本 | 升修订版本 |

每个版本在 `CHANGELOG.md` 记一节，tag 写成 `v0.9.0`。
