[English](board-overview.md)

# 调试板概览

Radxa Linkr Debugger 是一块 USB 控制的硬件桥接板，用于自动化 bring-up、恢复和调试。一根 USB 线连到 PC，就能控制目标板供电、启动模式、SD 路由、电流监测和 GPIO——全部可通过 CLI、Web UI 或 AI Agent 脚本化操作。

<!-- TODO: 在此处添加调试板实物图/示意图 -->

## 接口和端口

### Host USB

单根 USB-C 连接到 PC。板子枚举为复合设备：
- **USB NCM** — 网络接口，控制面地址 `http://172.29.203.1`
- **USB CDC ACM** — 串口 fallback，用于 Zephyr shell 和 BOOTSEL 恢复

Linux 和 macOS 无需安装驱动。Windows 需要标准 CDC ACM 驱动。

### 目标板调试口（CH347F）

板载 CH347F 提供两路 UART 和一个 JTAG/SWD 口，直连目标板调试连接器。RP2350 固件不在 JTAG/SWD 数据链路中——CH347F 作为独立调试探针工作。

| 通道 | 设备后缀 | 用途 |
|------|---------|------|
| UART0 | `D1` | 主串口（U-Boot、内核、登录） |
| UART1 | `D3` | 第二串口通道 |
| JTAG/SWD | — | 通过 OpenOCD 调试目标板 |

VIO 电压可选 3.3V（默认）或 1.8V（`switch vin`）。两路 UART 共享同一 VIO 电平。

### 电源输出

三条可控电源轨，为目标板供电：

| 输出 | 电压 | 典型用途 |
|------|------|---------|
| `5v_out` | 5V | SBC、开发板 |
| `12v_out` | 12V | 较高功耗目标 |
| `20v_out` | 20V | USB-PD 级目标 |

每条电源轨都有电流传感监测（INA139），通过 `adc read` 读取。

`5V_FIN` 是板子的供电输入，不是可控输出。

### TF/SD 卡槽

Micro-SD 卡槽，带硬件 mux 在两条路径间切换：

| 路由 | 说明 |
|------|------|
| `target`（默认） | SD 卡出现在目标板上 |
| `usb-reader` | SD 卡作为 USB 大容量存储出现在主机上 |

切换命令：`radxa-linkr-debuggerctl switch route sd usb-reader`。

### GPIO 排针

**J13**（2×2）— 三个专用 GPIO：

| 引脚 | GPIO | 标签 | 典型用途 |
|------|------|------|---------|
| 1 | GP7 | CON_MAS | MASKROM 入口信号 |
| 2 | GP8 | CON_REST | 复位控制 |
| 3 | GP9 | CON_USER | 用户自定义 |

**J16**（6×2）— 通用 GPIO 和模拟输入：

| 引脚 | GPIO | 说明 |
|------|------|------|
| 1–11 | GP10–GP20 | 数字 I/O，逻辑分析仪也可用 |
| 12 | GP29 | ADC3 模拟输入 |

J13 和 J16 的所有引脚都在安全 GPIO 允许列表中。逻辑分析仪可以在这些引脚上以最高 125 MHz 采样。

### 状态 LED

GPIO25 上的蓝色 LED。作为 watchdog 心跳指示——固件健康时约 1 Hz 闪烁。watchdog 触发后停止闪烁。

## 快速参考

```
radxa-linkr-debuggerctl power set 5v_out on       # 给目标板上电
radxa-linkr-debuggerctl switch route sd usb-reader  # 从主机读取 SD
radxa-linkr-debuggerctl gpio set GP13 1             # 驱动 GPIO
radxa-linkr-debuggerctl adc read                    # 查看电流
radxa-linkr-debuggerctl doctor                      # 完整连接检查
```
