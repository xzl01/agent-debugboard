# TUI 指南

[English](tui.md)

## 启动

不带子命令运行 CLI 即可启动交互式 TUI：

```sh
radxa-linkr-debuggerctl
```

## 布局

TUI 控制面分为三个区域：

- **Power** — `12v_out`、`5v_out`、`20v_out` 开/关切换
- **Switch** — SD switch（`target` / `usb-reader`）、USB switch（`pc` / `target`）、VIN（`1.8v` / `3.3v`）
- **GPIO** — 安全 GPIO 引脚，支持输出/输入模式

状态区同时显示 switch 的 `desired`（本地目标状态）与 `actual`（后端回读状态），方便诊断瞬态或单向路由故障。

## 导航

| 按键 | 操作 |
| --- | --- |
| 方向键 / Tab | 移动选择 |
| Space / Enter | 切换当前项 |
| `i` | 将当前 GPIO 切回输入模式 |
| `t` | SD switch → `target` |
| `u` | SD switch → `usb-reader` |

## VIN 确认

VIN 切换需要确认，因为电压变更具有硬件副作用。在 TUI 中对 VIN 项按 Space/Enter 并在提示时确认。

## 多实例稳定性

TUI 维持较温和的 60 Hz 重绘节奏，通过 HTTP 轮询状态和 ADC 数据。因此多个 TUI 实例可以同时稳定运行。

## 高频采集

高频 ADC 采集请在 CLI 中使用 `adc record` — 它创建独立于 TUI 的 live websocket session。详见 [CLI 参考](cli.zh-CN.md#录制)。
