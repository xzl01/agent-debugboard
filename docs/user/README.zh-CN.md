# 用户指南

[English](README.md)

欢迎使用 Radxa Linkr Debugger 用户指南。

## 目录

- [调试板概览](board-overview.zh-CN.md) — 接口、端口和物理连接
- [安装 CLI](install.md) — 下载并安装主机侧 CLI
- [CLI 参考](cli.md) — 所有 CLI 子命令和用法
- [HTTP API 参考](api.md) — REST 和 WebSocket API 端点
- [TUI 指南](tui.md) — 交互式终端界面
- [Web UI 指南](webui.md) — 浏览器端控制面板
- [OTA 固件更新](ota.md) — MCUboot 空中更新
- [OpenOCD / JTAG](openocd.md) — 配合 OpenOCD 使用
- [逻辑分析仪](logic-analyzer.md) — 高速 GPIO 捕获
- [功耗分析仪](power-analyzer.md) — 触发式电流捕获

## 快速开始

1. 安装 CLI：参见 [安装 CLI](install.md)
2. 通过 USB 连接调试板
3. 运行 `radxa-linkr-debuggerctl doctor` 验证连接
4. 打开 http://172.29.203.1/ 访问 Web UI
