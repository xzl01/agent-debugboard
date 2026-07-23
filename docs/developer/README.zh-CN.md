# 开发者指南

[English](README.md)

欢迎来到 Radxa Linkr Debugger 开发者指南。本章节涵盖构建、烧录和参与项目开发所需的全部内容。

## 目录

- [快速入门](getting-started.zh-CN.md) — 环境搭建和首次构建
- [构建固件](build.zh-CN.md) — 详细构建说明（Nix 和手动方式）
- [烧录](flashing.zh-CN.md) — BOOTSEL 和 OTA 烧录流程
- [参与贡献](contributing.zh-CN.md) — 测试、HIL、CI 和编码规范
- [硬件映射](hardware-mapping.zh-CN.md) — RP2350A 引脚分配和原理图参考

## 仓库结构

```text
apps/radxa_linkr_debugger/        Zephyr 应用程序
apps/radxa_linkr_debugger/src/    固件源码和共享板级模型
apps/radxa_linkr_debugger/tests/  单元测试
cmd-ng/                          主 Rust 主机 CLI/TUI
web/                             Web UI 和设备桥接
doc/                          硬件文档、原理图和营销素材
skills/radxa-linkr-debugger/      Agent 技能和操作指南
west.yml                      Zephyr 工作区清单
```
