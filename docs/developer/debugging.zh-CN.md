# 开发调试

[English](debugging.md)

本文用于诊断和修改 Radxa Linkr Debugger 本身，不是连接目标板后的操作流程。
目标操作、电源控制、恢复和串口使用应遵循 Agent skill 与
[用户指南](../user/README.zh-CN.md)。

## 从首个失败边界开始

按最先失效的边界分类。不要从浏览器现象推断固件故障，也不要把本地测试当成
板级行为的证据。

| 首个失败边界 | 权威下一步 |
| --- | --- |
| 构建、生成资源或产物选择 | [构建指南](build.zh-CN.md) |
| ROM BOOTSEL 或 MCUboot 镜像恢复 | [刷写](flashing.zh-CN.md) |
| USB NCM、HTTP、captive portal 或托管 Web UI | [Web UI](../user/webui.zh-CN.md) 与 [故障排除](../user/troubleshooting.zh-CN.md) |
| 原始 HTTP 响应结构或状态端点 | [HTTP API](../user/api.zh-CN.md) |
| MCP 可用性、工具结果或进度协议 | [MCP Server](../reference/mcp-server.md) |
| 共享 UART 所有权、游标或 RX 归档 | [Serial Broker](../reference/serial-broker.md) |
| ADC、GP29 或遥测 wire shape | [ADC Telemetry](../reference/adc-telemetry.md) |
| 逻辑分析仪传输或捕获语义 | [Logic Analyzer](../reference/logic-analyzer.md) |
| OpenOCD/JTAG adapter 或目标复位 | [OpenOCD](../user/openocd.zh-CN.md) |
| 必需的板级验收 | [HIL 功能测试规范](../testing/hil-functional-test-spec.md) |

## 浏览器与桥接修改

对于板载页面，导航或资源加载超时首先指向 USB NCM、HTTP 服务或浏览器设置；
页面已渲染后的断言失败则指向 Web UI 回归。测试报告和问题分流必须保留这一区别。

板载页面由 HTTP 提供。直连 Web Serial 因此需要文档规定的安全来源覆盖、用户
手势和浏览器选择器。浏览器自动化可以覆盖设置 UI 和 bridge 行为，但不能替代
手动接受选择器或实际串口 I/O。支持的 bridge 与权限路径见
[Web UI](../user/webui.zh-CN.md)。

当托管 Web 页面需要访问板子时，先通过该页面排查 bridge 进程及其 REST、OTA 和
WebSocket 转发，再修改固件或协议代码。不要在操作 skill 中记录开发 bridge 命令。

## 控制面隔离

使用固件响应 envelope 区分传输故障和操作故障。端点可达但 `ok: false` 表示固件
拒绝操作；超时、连接被拒绝或无路由表示传输或 USB NCM 状态问题。字段和错误含义
以 [HTTP API](../user/api.zh-CN.md) 为准。

MCP 和 Web UI 共用串口基础设施。游标过期、共享订阅、独占写和 RX 归档完整性应
按照 [Serial Broker](../reference/serial-broker.md) 排查，不要通过添加主机端重试或
回放来掩盖问题。变更硬件状态的操作出错后必须重新由操作员决定。

## 固件与恢复修改

镜像构建、构建目录策略、release 产物名称和源码构建说明属于
[构建指南](build.zh-CN.md)。刷写步骤及合成 UF2 与 OTA 二进制的区别属于
[刷写](flashing.zh-CN.md)。正常控制面不可用时，物理 ROM BOOTSEL 恢复路径仍是
后备手段。

对目标进行 JTAG/SWD 工作时，Linkr 负责电源和恢复线，但不是 JTAG probe。请遵循
面向目标的 [OpenOCD](../user/openocd.zh-CN.md) 工作流及其 CH347 adapter 要求。

## 验证边界

单元、契约和浏览器测试只能识别本地回归。涉及固件行为或硬件交互主机控制的修改
必须按 [HIL 功能测试规范](../testing/hil-functional-test-spec.md) 完成板级验收。
带日期的测量和历史报告保留在 `docs/testing/`，不要复制到 skills 或开发调试说明。

## 相关文档

- [参与贡献](contributing.zh-CN.md) 负责本地验证、CI 和 HIL 策略。
- [版本与发布门禁](versioning.zh-CN.md) 负责版本和发布流程。
- [持久配置](../reference/persistent-configuration.md) 负责快照格式与恢复语义。
- [Power Analyzer](../reference/power-analyzer.md) 和
  [Sigrok Linkr v2](../reference/sigrok-linkr-v2.md) 负责各自的协议细节。
