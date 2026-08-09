# radxa-linkr-debugger

[English](README.md)

RP2350 系列固件，用于 **Radxa Linkr Debugger** —— 一块 USB 控制的硬件调试桥，
让 PC 侧 Agent/AI 可以直接操作目标板供电、刷机模式、TF/SD 路由、
电流监测 ADC 和一组安全 GPIO。

当前正式支持的硬件为 G3 / RP2350A。G2 / RP2040 已停止支持：
不再为其构建固件、发布产物、修复兼容性问题或执行硬件验证。
RP2354 硬件需要完成专用 board 定义和 HIL 验证后才能声明支持。

![Radxa Linkr Debugger 宣传图](doc/marketing/radxa-linkr-debugger-promo.png)

## 功能范围

| 模块 | 当前支持 |
| --- | --- |
| USB 控制 | 复合 USB：NCM HTTP/WS + CDC ACM fallback |
| 主机自动化 | Rust `linkr-host` 统一承载 Web/网关/Broker/MCP，另提供支持 JSON 输出的 Rust CLI/TUI |
| Web UI | 仪表盘 http://172.29.203.1/ |
| 逻辑分析仪 | PIO2+DMA，100 kHz–125 MHz，WebSocket/raw-TCP Sigrok，[共用 packed arena 与当前 WIDE11 捕获](doc/logic-analyzer.md) |
| 功率分析仪 | 触发式采集，环形缓冲，CSV/NDJSON 导出 |
| 电源输出 | `12v_out`、`5v_out`、`20v_out`、`vdd_5v` |
| ADC 监测 | `5v_out`、`12v_out`、`20v_out` 电流读数及 GP29/ADC3 电压遥测；详见 [doc/adc-telemetry.md](doc/adc-telemetry.md) |
| 路由切换 | 固件上报的 TF/SD、USB hub mux、TF 写保护（`writable`/`protected`）、VIN（1.8V/3.3V） |
| GPIO | `GP7`–`GP20`；`GP29` 仍保留在目录中，但在 `adc3` 拥有时仅可输入（见 [GP29 ownership](doc/adc-telemetry.md#gp29-ownership)） |
| OTA 更新 | MCUboot 无签名 OTA |
| 看门狗 | 自主恢复至 BOOTSEL |
| 持久化配置 | [一份由固件持有的显式快照](doc/persistent-configuration.md)；安全值在启动时恢复，危险值需要确认，清除快照不会改变实时硬件 |
| 强制门户 | DHCP option 114/HTTP 自动打开 Web UI |

WIDE11 使用 144184 B 硬件切片和 30720 B WS 遥测环，共享 149048 B 总后备分配。

ADC3 契约、wire shape 和 GP29 所有权规则见
[doc/adc-telemetry.md](doc/adc-telemetry.md)；[2026-07-31 ADC3 telemetry HIL 报告](doc/testing/results/2026-07-31-adc3-telemetry-hil.md)
是 GP29 直接所有权子项的硬件验证证据。

`5V_FIN` 会被当作独立的输入/来源电源处理，不作为可控输出暴露给主机。

## 持久化配置

持久化配置是固件拥有的单一显式快照，使用唯一的 v1 线格式；见[权威契约](doc/persistent-configuration.md)。快照头部为 12 字节；byte 4 version 只接受 1，byte 7 zero 是唯一接受的恢复填充。Save 捕获实时值、持久化 v1 快照并立即应用这些值，一次确认的 Save 即完成持久化与应用（save persists and applies）。任何结构有效的 v1 快照在每次正常启动时全量重放所有已保存条目，包括危险值（v1 full restore）；Save 时的固件确认是唯一的危险门。version 字节不是 1 的快照永远不会被重放、迁移或自动清除（never replayed, migrated, or auto-cleared），只有显式的新 Save 才能覆盖。重放在第一个硬件失败处停止，并报告已应用、失败和待处理行；failed retry via repeated save 是唯一的重试路径。清除快照不会改变实时硬件。

本地验证不等于真实硬件 HIL。2026-08-05 真实硬件 HIL 通过 v1 save-and-apply flow；见[日期 v1-save HIL 报告](doc/testing/results/2026-08-05-persistent-config-v1-save-hil.md)。历史 2026-07-30 真实硬件 HIL 六个 runner flow 全部通过；见[历史六 flow 报告](doc/testing/results/2026-07-30-persistent-config-hil.md)。未来本地测试仍不能替代板级 HIL。

### 固定契约摘要

| Contract ID | Frozen literal |
| --- | --- |
| `storage` | `storage_partition+Settings+NVS` |
| `snapshot` | `linkr/config/snapshot;v1;one` |
| `explicit-save` | `ordinary-setters-volatile;explicit-save-only` |
| `boot-restore` | `defaults-first;v1-full-restore` |
| `header` | `12B-header;byte4-version=1;byte7-zero;max-104B` |
| `firmware-confirmation` | `firmware-owned-confirmation;save-time-only` |
| `save` | `persists-and-applies;failed-retry-via-resave` |
| `clear` | `settings_delete;hardware-unchanged` |
| `busy` | `busy:capture\|ota` |
| `recovery` | `BOOTSEL:radxa-linkr-debugger-rp2350.uf2;OTA:radxa-linkr-debugger-rp2350-ota.bin;zephyr.uf2-invalid` |
| `security` | `no-profiles;no-encryption;no-authentication-or-authorization;no-config-rollback` |
| `hil-boundary` | `local-distinct;real-HIL-2026-08-05-v1-save-pass` |

## 快速开始

1. [安装 CLI](docs/user/install.zh-CN.md)
2. 通过 USB 连接调试板
3. 运行 `radxa-linkr-debuggerctl doctor`
4. 打开 http://172.29.203.1/ 访问 Web UI

```sh
radxa-linkr-debuggerctl --version
radxa-linkr-debuggerctl doctor
radxa-linkr-debuggerctl status
```

如需主机托管的 Web UI、共享串口会话和 MCP，先构建 Web，再启动单一 Rust Host 进程：

```sh
npm --prefix web ci
npm --prefix web run build
cargo run --manifest-path host-tools/Cargo.toml -- serve
```

然后打开 <http://127.0.0.1:8787/>。详见
[Host Tools](host-tools/README.md) 和 [MCP 服务](doc/mcp-server.md)。

如果 release CLI 尚未下载或安装，请先看 [安装 CLI](docs/user/install.zh-CN.md)；
该指南同时覆盖稳定 release 通道和独立的滚动 nightly 通道。Agent 可优先使用
[本地 MCP 服务](doc/mcp-server.md)访问共享串口和常用调试板能力；curl 保留为
跨平台兜底。通过 CLI 做自动化时，
优先使用 `--json`，解析 `schema`、`ok`、`command` 和 `error.code`，不要解析面向
人看的文本输出。

## 文档

- **[用户指南](docs/user/README.zh-CN.md)** — CLI、TUI、WebUI、OTA、OpenOCD、逻辑分析仪、功率分析仪
- **[开发者指南](docs/developer/README.zh-CN.md)** — 构建、刷写、贡献、硬件映射
- **[持久化配置](doc/persistent-configuration.md)** — 固件持有的快照、恢复与确认行为

## 给 AI Agent 的使用入口

AI Agent 在操作硬件前，应先读取
[skills/radxa-linkr-debugger/SKILL.md](skills/radxa-linkr-debugger/SKILL.md)。这份 skill
是仓库内面向 Agent 的权威操作规程。若本地 MCP 可用，skill 会优先使用强类型
MCP 工具，再回退到 curl 或 CLI；它同时覆盖连接诊断、JSON 命令使用和有副作用
操作的安全规则。

在修改仓库文件之前，AI Agent 还应先读取 [AGENTS.md](AGENTS.md)。仓库内的
默认规则如下：

- 任何代码改动都必须在同一个改动中同步更新对应的 skill 和文档。
- 修改固件行为或主机侧 CLI 逻辑时，必须同步更新相关说明并运行对应测试。
- 修改固件时，必须检查并保持 USB CDC ACM 串口的 BOOTSEL fallback 路径可用。
- 修改 skill 时，必须执行一次 subagent 验证/测试。
- 添加新功能时，只要实际可行，就应同步添加对应的功能测试。
- 修改固件或与真实硬件交互的主机侧逻辑时，结束前必须完成 HIL 功能测试；参见 `AGENTS.md` 和 `doc/testing/hil-functional-test-spec.md`。
- 只要 Zephyr 绑定和板级模型能自然表达，硬件描述就优先使用 Device Tree，而不是固件里的硬编码表。
- 软件实现应保持标准、统一、优雅，避免引入让维护、自动化或文档理解变得困难的临时性写法。
- MCU 侧输出应尽量贴近接口原值；只要不破坏原始固件契约，解释、校准和展示优先放在 host 侧完成。

推荐 Agent 最小流程：

```sh
# 推荐的本地 Agent 接入；客户端配置见 doc/mcp-server.md。
cargo run --manifest-path host-tools/Cargo.toml -- mcp

# CLI 兜底与诊断。
radxa-linkr-debuggerctl --version
radxa-linkr-debuggerctl --json doctor
radxa-linkr-debuggerctl --json status
```

如果 release CLI 尚未下载或安装，先按下方 release 安装路径处理。只有在执行
MCP 不可用时才回退到 skill 的 curl 工作流。通过 CLI 做自动化时，
优先使用 `--json`，解析 `schema`、`ok`、`command` 和 `error.code`，不要解析面向
人看的文本输出。

## 仓库结构

```text
apps/radxa_linkr_debugger/        Zephyr 应用
apps/radxa_linkr_debugger/src/    固件源码和共享板级模型
apps/radxa_linkr_debugger/tests/  单元测试
cmd-ng/                          面向高级用户和 Agent 的 Rust CLI/TUI
host-tools/                      Rust Web 托管、设备网关、串口 Broker 与 MCP
web/                             React UI 和迁移期 Node 兼容实现
doc/                          硬件文档、原理图和宣传素材
skills/radxa-linkr-debugger/      面向 Agent 的 skill 和操作规程
west.yml                      Zephyr workspace manifest
```
