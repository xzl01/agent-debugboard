# TUI 指南

[English](tui.md)

## 启动

不带子命令运行 CLI 即可启动交互式 TUI：

```sh
radxa-linkr-debuggerctl
```

## 布局

控制面分三个区域：

- **Power** — `12v_out`、`5v_out`、`20v_out` 开关
- **Switch** — SD（`target` / `usb-reader`）、USB（`pc` / `target`）、VIN（`1.8v` / `3.3v`）
- **GPIO** — 安全引脚，支持输出/输入模式

状态区同时显示 switch 的 `desired`（本地目标）和 `actual`（后端回读），方便判断路由是否真正生效。

## 导航

| 按键 | 操作 |
| --- | --- |
| 方向键 / Tab | 移动选择 |
| Space / Enter | 切换当前项 |
| `i` | 将当前 GPIO 切回输入模式 |
| `t` | SD switch → `target` |
| `u` | SD switch → `usb-reader` |

VIN 切换会弹确认提示——电压变更对硬件有副作用。

## 多实例

TUI 通过 HTTP 轮询状态，刷新频率 60 Hz。可以同时开多个实例，互不干扰。

## 高频采集

高频 ADC 采集用 CLI 的 `adc record`——它走独立的 websocket，不经过 TUI。详见 [CLI 参考](cli.zh-CN.md#录制)。
