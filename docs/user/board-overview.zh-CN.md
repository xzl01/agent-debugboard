[English](board-overview.md)

# 调试板概览

Radxa Linkr Debugger 是一块 USB 控制的硬件桥接板，用于自动化 bring-up、恢复和调试。一根 USB 线连到 PC，就能控制目标板供电、启动模式、SD 路由、电流监测和 GPIO——全部可通过 CLI、Web UI 或 AI Agent 脚本化操作。

`RA051_MB_X1.2` 的连接器位置和位号见
[X1.2 位号图](../hardware/RA051_V12_20260920位号图.pdf)，电气定义见
[X1.2 原理图](../hardware/RA051_MB_X1.2.pdf)。信号定义以下方表格为准；
市场宣传用的俯视图不是 pinout。

## 接口和端口

| 位号 | 接口 | 说明 |
|---|---|---|
| J15 | PC / Debug USB-C | USB NCM、console CDC ACM、J4 CDC ACM 和 CH347F hub 的主机连接口 |
| J1 | USB-C 电源输入 | 板级 `POWER IN` 输入 |
| J3 | 目标 USB-C | 经板载 hub 路由的目标板 / OTG USB 接口 |
| J2 | TF / microSD 卡槽 | 可在目标板与主机 USB 读卡器路径间切换 |
| J5 | 12 V 输出 | 可控 `12v_out` 接线端子 |
| J6 | 20 V 输出 | 可控 `20v_out` 接线端子 |
| J7 | 5 V 输出 | 可控 `5v_out` 接线端子 |
| J8 | GPIO / ADC 排针 | 安全 GPIO 子集、ADC 输入、PIO I2C、USB 信号、GND 和 hub 5 V |
| J13 | 恢复控制 GPIO | `CON_MAS`、`CON_REST`、`CON_USER` 和 GND |
| J4 | 目标串口 | 3 针 UART1 TX/RX/GND 排针 |

连接排线或探针前，请先确认板上丝印的 1 脚标记。

### Host USB

单根 USB-C 连接到 J15 所接 PC。板子枚举为复合设备：
- **USB NCM** — 网络接口，控制面地址 `http://172.29.203.1`
- **USB CDC ACM** — console 串口 fallback，用于 Zephyr shell 和 BOOTSEL 恢复
- **USB CDC ACM（J4）** — 专用目标串口桥

Linux 和 macOS 无需安装驱动。Windows 需要标准 CDC ACM 驱动。

### 目标板调试口（CH347F）

板载 CH347F 是挂在板载 USB hub 上的独立 USB 调试桥，提供目标 UART 和
JTAG/SWD 通道，并作为独立 USB 设备出现。RP2354A 固件不在其 JTAG/SWD 数据链路
中。x1.2 没有 VIO 电压选择，使用 3.3 V 信号电平。下方 J4 是 RP2354A 的 UART1
桥，不是 CH347F 通道。

### 电源输出

三条可控目标电源轨：

| 输出 | 电压 | 典型用途 |
|------|------|---------|
| `5v_out` | 5V | SBC、开发板 |
| `12v_out` | 12V | 较高功耗目标 |
| `20v_out` | 20V | USB-PD 级目标 |

`vdd_5v` 是 USB hub 的内部辅助 5 V 电源轨，同时在 CLI/API 中作为可控输出暴露；
固件让它随 `switch usb` 路由联动。

每条电源轨都有 INA226 电流/电压/功率监测，可通过 `adc read` 读取。ADC3
（`GP29`）是独立的纯监测电压输入，不是电源轨电流传感器。

### TF/SD 卡槽

Micro-SD 卡槽，带硬件 mux 在两条路径间切换：

| 路由 | 说明 |
|------|------|
| `target`（默认） | SD 卡出现在目标板上 |
| `usb-reader` | SD 卡作为 USB 大容量存储出现在主机上 |

切换命令：`radxa-linkr-debuggerctl switch route sd usb-reader`。

### USB 设备切换

`switch usb` 选择共享 USB 设备路径呈现给 PC 还是目标板。固件还会用该路由控制
`vdd_5v` hub 域的开关：

```sh
radxa-linkr-debuggerctl switch route usb target --confirm
radxa-linkr-debuggerctl switch route usb pc --confirm
```

切换前应先卸载存储设备并停止正在进行的 USB 传输。可使用
`radxa-linkr-debuggerctl switch get usb` 确认当前路径。

### GPIO 排针

J8 和 J13 都是从板右边缘侧出的排针。下列方向和坐标均为**元件面俯视**，不是
连接器配合面视角。J8 在上、J13 在下。J8 左列从上到下为 pin 10 到 pin 1，
右列从上到下为 pin 20 到 pin 11；固件以顶部为 `row=0`、左列为 `column=0`。

**J13**（2×2）— 恢复控制信号：

| UI 行 | 左侧触点（`column=0`） | 右侧触点（`column=1`） |
|---|---|---|
| 0（顶部） | Pin 2：GP9 / `CON_USER` / USER | Pin 3：GP8 / `CON_REST` / RSET |
| 1（底部） | Pin 1：GP7 / `CON_MAS` / MASKROM | Pin 4：GND（不可交互） |

**J8**（2×10）— 通用 GPIO、模拟输入和 I2C：

| UI 行 | 左侧触点（`column=0`） | 右侧触点（`column=1`） |
|---|---|---|
| 0（顶部） | Pin 10：GP21 / PIO I2C SDA | Pin 20：`5V_USB_HUB` |
| 1 | Pin 9：GP20 / PIO I2C SCL | Pin 19：USB DM |
| 2 | Pin 8：GP27 / ADC1 | Pin 18：USB DP |
| 3 | Pin 7：GP28 / ADC2 | Pin 17：GND |
| 4 | Pin 6：GP29 / ADC3（安全、仅输入） | Pin 16：GP5 |
| 5 | Pin 5：GP16（安全） | Pin 15：GP6 |
| 6 | Pin 4：GP15（安全） | Pin 14：GND |
| 7 | Pin 3：GP14（安全） | Pin 13：USB DM |
| 8 | Pin 2：GP13（安全） | Pin 12：USB DP |
| 9（底部） | Pin 1：GP12（安全） | Pin 11：GND |

映射到连接器的 safe-GPIO 控制子集是 J13 上的 GP7–GP9，以及 J8 上的
GP12–GP16 和仅输入的 GP29/ADC3。GP17、GP18 仍是可输出的 safe GPIO，但尚无
已确认的 J8/J13 位置；固件因此不输出它们的 `layoutGroup`、`layoutRow`、
`layoutColumn`，客户端通过通用 fallback 显示。UI 只是允许列表内的控制子集，
不是完整 pinout，也不是连接器配合面视图。接线时应以本节完整元件面表格和原理图
为准。

逻辑分析仪可捕获 GP10–GP20。SINGLE 和 FAST8 最高 125 MHz；WIDE11 限制为
100 MHz。GP29 因由 ADC3 拥有而不在逻辑分析仪范围内。GP20 作为只读 SCL tap
采样，I2C 总线工作时不得外部驱动或拉低。

### 目标串口（J4）

J4 是 UART1 上的 3 针串口，桥接到专用 USB CDC ACM：

| 引脚 | 信号 |
|-----|------|
| 1 | UART1 TX（GP10） |
| 2 | UART1 RX（GP11） |
| 3 | GND |

GP10/GP11 与逻辑分析仪共享。捕获选中其中任一引脚时，PIO2 拥有该引脚，J4
串口在捕获期间不可用；不要在捕获期间期待串口收发。捕获结束后，固件会把
GP10/GP11 恢复为 `GPIO_FUNC_UART_AUX` mux，J4 桥无需重启即可继续工作。

### 状态 LED

GPIO24 上的蓝色 LED，高电平有效。作为 watchdog 心跳指示——固件健康时约 1 Hz
闪烁。watchdog 触发后停止闪烁。

## 快速参考

```
radxa-linkr-debuggerctl power set 5v_out on       # 给目标板上电
radxa-linkr-debuggerctl switch route sd usb-reader  # 从主机读取 SD
radxa-linkr-debuggerctl gpio set GP13 1             # 驱动 GPIO
radxa-linkr-debuggerctl adc read                    # 查看电流
radxa-linkr-debuggerctl adc read vdd_5v             # 查看 USB hub 辅助 5V 轨
radxa-linkr-debuggerctl doctor                      # 完整连接检查
```
