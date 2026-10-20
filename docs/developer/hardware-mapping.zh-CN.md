# 硬件映射

[English](hardware-mapping.md)

## 硬件支持策略

当前目标板是 `RA051_MB_X1.2`，MCU 为 RP2354A（QFN-60，内核与引脚兼容
RP2350A）。G2 / RP2040 已退役，不再参与构建、发布、维护和 HIL 覆盖。

构建仍复用上游 `rpi_pico2/rp2350a/m33/mcuboot` board 定义。x1.2 不贴装外部
QSPI `U23`，使用 2 MB 内部 Flash。4 MB sysbuild 分区布局只是把 storage 分区
声明得更大，不会把实际数据放到 2 MB 之后；若要切换到 2 MB 布局，需要仓库内的
MCUboot board。权威板级说明见
[x1.2 硬件与 BSP 参考](../reference/hardware-x1.2.md)。

## RP2354A 引脚表

| 功能 | 固件名称 | 原理图信号 | GPIO |
|---|---|---|---|
| 5 V 输出使能 | `5v_out` | `GP0_5V_EN` | 0 |
| 辅助 5 V（USB hub） | `vdd_5v` | `GP01_5V_WS_EN` | 1 |
| 12 V 输出使能 | `12v_out` | `GP02_12V_EN` | 2 |
| 20 V 输出使能 | `20v_out` | `GP03_20V_EN` | 3 |
| TF/SD 路由切换 | `switch sd` | `GP06_TF_SW` | 4 |
| USB 设备路由切换 | `switch usb` | `GPIO5` / USB mux | 5 |
| J4 UART TX | UART1 TX | `GPIO10_CON` | 10 |
| J4 UART RX | UART1 RX | `GPIO11_CON` | 11 |
| CH224A PD strap | —（固件保持高阻输入） | `RP2354_PD_CFG1` | 19 |
| PIO I2C SCL / WIDE11 lane 10 | I2C | `RP2354_SCL` | 20 |
| PIO I2C SDA | I2C | `RP2354_SDA` | 21 |
| TF 写保护 | `switch tf_wp` | `TF_WP` | 22 |
| USB OTG 使能 | — | `OTG_USB_EN` | 23 |
| 状态 LED | — | `LED_BLUE` | 24 |
| ADC0 | — | `GPIO26_ADC0` | 26 |
| ADC1 | — | `GPIO27_ADC1` | 27 |
| ADC2 | — | `GPIO28_ADC2` | 28 |
| ADC3 / GP29 | `adc3` | `GPIO29_ADC3` | 29 |
| 恢复控制 GPIO / J13 pin 1 | `CON_MAS` | `GP07_MAS` | 7 |
| 恢复控制 GPIO / J13 pin 3 | `CON_REST` | `GP08_REST` | 8 |
| 恢复控制 GPIO / J13 pin 2 | `CON_USER` | `GP09_USER` | 9 |
| J8 pin 1-5 安全 GPIO | `GP12`-`GP16` | `GPIO12`-`GPIO16` | 12-16 |
| 无 J8/J13 映射的安全 GPIO | `GP17`、`GP18` | `GPIO17`、`GPIO18` | 17、18 |

`GP10`/`GP11` 属于 J4 UART，不在安全 GPIO 目录中。它们同时也是逻辑分析仪
lane（SINGLE 用 GP10，FAST8/WIDE11 用 GP10/GP11）：捕获拥有这些引脚时 PIO2
接管，J4 不可用；cleanup 会把已配置引脚恢复为 `GPIO_FUNC_UART_AUX`。`GP19`
是 CH224A PD strap，`GP20`/`GP21` 是 PIO I2C 总线，三者都不作为 GPIO 控制
暴露。`GP29` 保留在目录中，但由 `adc3` 拥有，只能作为输入。

## 连接器方向与已确认映射

以下为用户结合原始x1.2 PDF确认的**元件面俯视图**。J8和J13都位于板卡
**右侧边缘**。这里只记录与控制/监测有关的已确认子集，不是完整连接器pinout；
空位或未列位置绝不能推断、点击或路由到其它GPIO。

J8为2x10连接器。自顶部向下，左列编号为`10`到`1`，右列为`20`到`11`。
已确认的相关J8端子全部位于左列：

| J8 pin | 信号 | 固件用途 |
|---|---|---|
| 10 | GP21 | PIO I2C SDA，不属于safe-GPIO控制 |
| 9 | GP20 | PIO I2C SCL，不属于safe-GPIO控制 |
| 8 | GP27 / ADC1 | ADC1端子，不是电源轨采样 |
| 7 | GP28 / ADC2 | ADC2端子，不是电源轨采样 |
| 6 | GP29 / ADC3 | `adc3`，safe目录中仅输入 |
| 5 | GP16 | safe GPIO |
| 4 | GP15 | safe GPIO |
| 3 | GP14 | safe GPIO |
| 2 | GP13 | safe GPIO |
| 1 | GP12 | safe GPIO |

ADC0/GP26在该确认映射中没有J8端子。GP17和GP18仍属于safe-GPIO允许列表，
但没有J8或J13连接器映射。

J13为2x2连接器，同样按元件面俯视：

| | 左 | 右 |
|---|---|---|
| 上 | pin 2：GP9 / USER | pin 3：GP8 / RSET |
| 下 | pin 1：GP7 / MASKROM | pin 4：GND |

J13 pin 4是**GND**，不是3.3 V。safe-GPIO允许列表独立保持为`GP7`-`GP9`、
`GP12`-`GP18`及仅输入`GP29`；不得从允许列表反推连接器位置。

## 状态 LED（GPIO24）

GPIO24 是蓝色状态 LED，Device Tree 中按高电平有效配置。它通过 Device Tree
chosen 属性作为 watchdog 心跳指示灯驱动，不使用 Zephyr 内置心跳驱动或
`CONFIG_LED`。LED 以约 1 Hz 频率闪烁（完整开/关周期），仅在硬件 watchdog 喂狗
成功后推进。固件拥有该 GPIO 时，跳过或失败的喂狗会把 LED 重置为非活动状态。

## 电源轨

可控制电源轨有四路：`5v_out`、`12v_out`、`20v_out` 和 `vdd_5v`（USB hub 的
辅助 5 V 域）。所有使能都经 P-FET 级，高电平有效。板级 3.3 V 由 `U14` 从
USB5V 生成；x1.2 没有可切换的 VIO/1.8 V regulator，也没有 `switch/vin` 项。
`vdd_5v` 在固件中随 `switch usb` 路由联动。启动默认状态由 Device Tree 和固件
初始化决定；应读取实时状态，而不是在主机侧施加默认值。

## 电流与电压监测

四颗 INA226 共享生产默认400 kHz的PIO I2C总线。合格基线使用生产BOM中的
4.7 kΩ上拉到常供电VCC_3V3，并保持J8空载。每颗INA226为一个电源轨上报电流、
总线电压和功率；板上没有ADC电流采样路径。任何J8器件、线缆、上拉或异域电源
都必须重新验证总电容、上升时间、VOL和电源域边界。固件启动时probe CH224A，
状态请求继续轮询；无PD源/VHV时失败后进入5秒退避，插入源后恢复。该期间
VCC_3V3供电的INA226遥测仍可工作。

| 电源轨 | 器件 | 7 位地址 | 采样电阻 |
| --- | --- | --- | --- |
| `20v_out` | U9 | `0x40` | 10 mOhm |
| `12v_out` | U15 | `0x41` | 10 mOhm |
| `5v_out` | U11 | `0x44` | 10 mOhm |
| `vdd_5v` | U12 | `0x45` | 10 mOhm |

J8只引出ADC1/GP27、ADC2/GP28和ADC3/GP29；ADC0/GP26不是J8端子。
ADC1/ADC2不是电源轨采样输入。ADC3（`GP29`）是由`adc3`拥有的纯监测电压通道，
没有电源状态。固件上报五个descriptor，主机直接展示固件值，不做主机侧校准表
或零点修正。合约见
[ADC telemetry 合约](../reference/adc-telemetry.md)。

## 原理图参考

- [原理图（rev X1.2）](../hardware/RA051_MB_X1.2.pdf)
- [位号图（X1.2）](../hardware/RA051_V12_20260920位号图.pdf)
