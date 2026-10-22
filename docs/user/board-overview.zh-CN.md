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
- **USB NCM** — 网络接口；控制面地址为 `ip` 命令输出的板卡 URL
- **USB CDC ACM** — console 串口 fallback，用于 Zephyr shell 和 BOOTSEL 恢复
- **USB CDC ACM（J4）** — MCU J4 serial runtime 的一个 sink/source；最终 x1.2
  HIL 中枚举为 ACM4，但主机 tty 编号不稳定

Linux 和 macOS 无需安装驱动。Windows 需要标准 CDC ACM 驱动。

### 目标板调试口（CH347F）

板载 CH347F 是挂在 USB hub 上的独立调试设备；这里仍有效的产品用途是目标
JTAG/SWD，RP2354A 固件不在该数据链路中。x1.2 没有 VIO 电压选择，使用 3.3 V
信号电平。仍随 host tools 提供的 CH347 UART 功能属于 legacy 非 J4 路径。下方
J4 由 RP2354A runtime 拥有，绝不经过 CH347。

### 电源输出

固件公布六路可控电源输出：

| 输出 | 电压 | 典型用途 |
|------|------|---------|
| `5v_out` | 5V | SBC、开发板 |
| `12v_out` | 12V | 较高功耗目标 |
| `20v_out` | 20V | USB-PD 级目标 |
| `vdd_5v` | 5V | USB hub 的内部辅助 5 V 父轨 |
| `5v_usb_hub` | 5V | USB hub 的开关子轨 |
| `5v_otg` | 5V | J3 双向 VBUS 开关，`vdd_5v` 子轨 |

`vdd_5v` 是 USB hub 的内部辅助 5 V 父轨。`5v_usb_hub` 是 GPIO24 上的开关子轨，
启动时关闭。打开 hub 子轨时按需先打开 `vdd_5v`；关闭 hub 子轨不改变父轨；关闭
`vdd_5v` 会先关闭 hub 子轨；仅打开 `vdd_5v` 不会打开 hub 子轨。`5v_otg` 是
GPIO23/Q10 控制的外部 J3 VBUS，也是 `vdd_5v` 子轨；外部 VBUS 可经 Q10 体二极管
回灌父轨。x1.2 没有 USB route switch，也没有 `/switch/usb` 端点。

四颗 INA226 监测 `5v_out`、`12v_out`、`20v_out` 和 `vdd_5v`，可通过
`adc read` 读取。`5v_usb_hub` 与 `5v_otg` 没有 INA226，也没有 ADC descriptor，
因此 `vdd_5v` 读数是包含 hub 支路的父轨总量；released 的 `5v_otg` 不是测得 0 V。ADC0 是 PD 输入源侧 DC20V 通道
（`role="pd_input"`，由 Device Tree 分压还原）；`raw`/`mv` 仍是 pad 诊断值。
ADC1-3 是 native pad 电压输入（`role="native"`），不是电源轨电流传感器；
ADC3 为 GP29，在 safe GPIO 目录中仅允许输入。主机不做 `115/15` 换算。ADC0
未校准精度约为 2%。

### TF/SD 卡槽

Micro-SD 卡槽，带硬件 mux 在两条路径间切换：

| 路由 | 说明 |
|------|------|
| `target`（默认） | SD 卡出现在目标板上 |
| `usb-reader` | SD 卡作为 USB 大容量存储出现在主机上 |

切换命令：`radxa-linkr-debuggerctl switch route sd usb-reader`。

### x1.2 switch 控制

固件 switch 目录只有 `sd` 和 `tf_wp`。`switch/sd` 使用 MCU GPIO4、原理图 net
`GP06_TF_SW`；`switch/tf_wp` 使用 GPIO22。J8 USB D+/D− 是 inert 连接器信号，
不是 route 控件。`GET /api/v1/switch/usb` 返回 HTTP 404，`vdd_5v` 与 switch
路由无关，`5v_usb_hub` 是它的电源子轨。

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
| 4 | Pin 6：GP29 / ADC3（安全、仅输入） | Pin 16：GP5 / `GPIO5_CON`（安全） |
| 5 | Pin 5：GP16（安全） | Pin 15：GP6 / `GPIO6_CON`（安全） |
| 6 | Pin 4：GP15（安全） | Pin 14：GND |
| 7 | Pin 3：GP14（安全） | Pin 13：USB DM |
| 8 | Pin 2：GP13（安全） | Pin 12：USB DP |
| 9（底部） | Pin 1：GP12（安全） | Pin 11：GND |

safe-GPIO 控制集是 J13 上的 GP7–GP9，以及 J8 上的 GP5、GP6、GP12–GP16 和
仅输入的 GP29/ADC3。GP5/GP6 是普通 RP2354 直连 GPIO，默认 input、无 pull，
不是 USB/TF switch。GP17、GP18 不存在。24 个物理 cell 中有 11 个控件、13 个
inert，10 个控件可输出。UI 不是完整 pinout，也不是连接器配合面视图。接线时应
以本节完整元件面表格和原理图为准。

逻辑分析仪可捕获 GP10–GP20。SINGLE 和 FAST8 最高 125 MHz；WIDE11 限制为
100 MHz。GP29 因由 ADC3 拥有而不在逻辑分析仪范围内。GP20 作为只读 SCL tap
采样，I2C 总线工作时不得外部驱动或拉低。

### 目标串口（J4）

J4 是 UART1 上的单一逻辑 3 针目标串口。runtime 接受 4800 到 115200 的任意
整数 baud；UI 另提供五个推荐 preset，默认 115200。高于 115200 的 rate 会被
拒绝；早期 high-rate 样本只是诊断证据，不是产品支持范围。J4 引脚定义如下：

| 引脚 | 信号 |
|-----|------|
| 1 | UART1 TX（GP10） |
| 2 | UART1 RX（GP11） |
| 3 | GND |

DTR=false 时的 USB default line coding 只会 stage，不会改变 active baud 或
generation；此低态下通过 HTTP/WS 应用的 settings 立即生效。打开终端时，板子先
应用 staged settings，之后才 publish DTR 并开放 ingress。apply/attach/detach
失败会让 J4 保持 blocked，直到成功的 LOW/reset。

repo-local `linkr_debugger_serial` runtime 拥有 UART1 和共享 4096 字节 RX ring，
独立 fan-out 到 J4 CDC ACM、四个 `serial.j4` WebSocket sink 和 raw TCP 5555
上的两个 client。板载 xterm.js 终端使用 MCU WebSocket，不使用 host Web Serial
或 CH347。

显式输入仍然是 multiwriter。只有自动终端回复有一个被选中的 responder：CDC
在 DTR 有效时优先，否则选择编号最小的已连接 WebSocket。其它终端页面仍会发送
显式键盘、IME、粘贴和 Ctrl-C 输入，不是只读；页面标记为
**Auto-reply observer** / **Manual input allowed**。raw TCP 永远不会成为自动
responder。wire 字段和 responder 矩阵见
[J4 串口传输契约](../reference/serial-broker.md)。

GP10/GP11 与逻辑分析仪共享。PIO2 接管前固件暂停 J4，并处理 active TX 的物理
wire completion；捕获期间串口不可用。cleanup 恢复 `GPIO_FUNC_UART_AUX` 和 fresh
sink epoch，同一 J4 session 无需重启即可继续。

### 状态 LED

GPIO25 上的蓝色 LED，高电平有效。作为 watchdog 心跳指示——固件健康时约 1 Hz
闪烁。watchdog 触发后停止闪烁。GPIO24 是 `5v_usb_hub` 使能，不是 LED。

## 快速参考

```
radxa-linkr-debuggerctl power set 5v_out on       # 给目标板上电
radxa-linkr-debuggerctl switch route sd usb-reader  # 从主机读取 SD
radxa-linkr-debuggerctl gpio set GP13 1             # 驱动 GPIO
radxa-linkr-debuggerctl adc read                    # 查看电流
radxa-linkr-debuggerctl adc read vdd_5v             # 查看 USB hub 父轨
radxa-linkr-debuggerctl power set 5v_usb_hub on     # 打开 hub 子轨（会先打开 vdd_5v）
radxa-linkr-debuggerctl doctor                      # 完整连接检查
```
