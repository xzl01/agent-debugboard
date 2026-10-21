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
| J8 pin 16 safe GPIO | `GP5` | `GPIO5_CON` | 5 |
| J8 pin 15 safe GPIO | `GP6` | `GPIO6_CON` | 6 |
| J4 UART TX | UART1 TX | `GPIO10_CON` | 10 |
| J4 UART RX | UART1 RX | `GPIO11_CON` | 11 |
| CH224A PD strap | —（固件保持高阻输入） | `RP2354_PD_CFG1` | 19 |
| PIO I2C SCL / WIDE11 lane 10 | I2C | `RP2354_SCL` | 20 |
| PIO I2C SDA | I2C | `RP2354_SDA` | 21 |
| TF 写保护 | `switch tf_wp` | `TF_WP` | 22 |
| USB OTG 使能 | — | `OTG_USB_EN` | 23 |
| USB hub 5 V 使能 | `5v_usb_hub` | `GPIO24_USB_CRL` | 24 |
| 状态 LED | — | `LED_BLUE` | 25 |
| ADC0 / CH224A输入门控 | 私有`ch224a_input_voltage` | `GPIO26_ADC0`、DC20V分压 | 26 |
| ADC1 | — | `GPIO27_ADC1` | 27 |
| ADC2 | — | `GPIO28_ADC2` | 28 |
| ADC3 / GP29 | `adc3` | `GPIO29_ADC3` | 29 |
| 恢复控制 GPIO / J13 pin 1 | `CON_MAS` | `GP07_MAS` | 7 |
| 恢复控制 GPIO / J13 pin 3 | `CON_REST` | `GP08_REST` | 8 |
| 恢复控制 GPIO / J13 pin 2 | `CON_USER` | `GP09_USER` | 9 |
| J8 pin 1-5 安全 GPIO | `GP12`-`GP16` | `GPIO12`-`GPIO16` | 12-16 |

`GP10`/`GP11` 属于 J4 UART，不在安全 GPIO 目录中。它们同时也是逻辑分析仪
lane（SINGLE 用 GP10，FAST8/WIDE11 用 GP10/GP11）：捕获拥有这些引脚时 PIO2
接管，J4 不可用；cleanup 会把已配置引脚恢复为 `GPIO_FUNC_UART_AUX`。`GP19`
是 CH224A PD strap，`GP20`/`GP21` 是 PIO I2C 总线，三者都不作为 GPIO 控制
暴露。`GP29` 保留在目录中，但由 `adc3` 拥有，只能作为输入。

## 连接器方向与已确认映射

以下为用户结合原始x1.2 PDF确认的**元件面俯视图**。J8和J13都位于板卡
**右侧边缘**。这里只记录与控制、监测和供电有关的已确认子集，不是完整连接器
pinout；
空位或未列位置绝不能推断、点击或路由到其它GPIO。

J8为2x10连接器。自顶部向下，左列编号为`10`到`1`，右列为`20`到`11`：

| J8 pin | 信号 | 固件用途 |
|---|---|---|
| 10 | GP21 | PIO I2C SDA，不属于safe-GPIO控制 |
| 20 | `5V_USB_HUB` | `5v_usb_hub` 子轨开关输出，不是 GPIO |
| 9 | GP20 | PIO I2C SCL，不属于safe-GPIO控制 |
| 8 | GP27 / ADC1 | ADC1端子，不是电源轨采样 |
| 7 | GP28 / ADC2 | ADC2端子，不是电源轨采样 |
| 6 | GP29 / ADC3 | `adc3`，safe目录中仅输入 |
| 5 | GP16 | safe GPIO |
| 4 | GP15 | safe GPIO |
| 3 | GP14 | safe GPIO |
| 2 | GP13 | safe GPIO |
| 1 | GP12 | safe GPIO |
| 16 | GP5 / `GPIO5_CON` | 直接 safe GPIO；默认 input、无 pull |
| 15 | GP6 / `GPIO6_CON` | 直接 safe GPIO；默认 input、无 pull |

ADC0/GP26在该确认映射中没有J8端子。GP17和GP18不属于当前safe/control目录。

J13为2x2连接器，同样按元件面俯视：

| | 左 | 右 |
|---|---|---|
| 上 | pin 2：GP9 / USER | pin 3：GP8 / RSET |
| 下 | pin 1：GP7 / MASKROM | pin 4：GND |

J13 pin 4是**GND**，不是3.3 V。safe-GPIO允许列表共11项：可输出的`GP5`、
`GP6`、`GP7`-`GP9`、`GP12`-`GP16`，以及仅输入`GP29`。24个物理cell中
11个可控制、13个inert，其中10个可输出。

## 状态 LED（GPIO25）

GPIO25 是蓝色状态 LED，Device Tree 中按高电平有效配置。它通过 Device Tree
chosen 属性作为 watchdog 心跳指示灯驱动，不使用 Zephyr 内置心跳驱动或
`CONFIG_LED`。LED 以约 1 Hz 频率闪烁（完整开/关周期），仅在硬件 watchdog 喂狗
成功后推进。固件拥有该 GPIO 时，跳过或失败的喂狗会把 LED 重置为非活动状态。
GPIO24 不是 LED，而是 `5v_usb_hub` 使能。

## 电源轨

可控制输出有五路：`5v_out`、`12v_out`、`20v_out`、`vdd_5v` 和
`5v_usb_hub`。`vdd_5v` 是 USB hub 的辅助 5 V 父级电源域，`5v_usb_hub` 是
GPIO24 上的开关子轨。所有使能都经 P-FET 或 load-switch 级，高电平有效。板级
3.3 V 由 `U14` 从 USB5V 生成；x1.2 没有可切换的 VIO/1.8 V regulator，也没有
`switch/vin` 或 `switch/usb`。`vdd_5v` 是独立的 firmware-owned 电源状态。
启动状态由 Device Tree、持久化重放和固件初始化决定；应读取实时状态，而不是在
主机侧施加默认值。

父子 setter 语义由固件拥有：打开 `5v_usb_hub` 时按需先打开 `vdd_5v`；关闭
`5v_usb_hub` 不改变 `vdd_5v`；关闭 `vdd_5v` 会先关闭 `5v_usb_hub`；仅打开
`vdd_5v` 不会打开 `5v_usb_hub`。因此只保存 `5v_usb_hub=on` 的 child-only
快照在重启重放时也会把父轨打开。

## 电流与电压监测

四颗 INA226 共享生产默认400 kHz的PIO I2C总线。合格基线使用生产BOM中的
4.7 kΩ上拉到常供电VCC_3V3，并保持J8空载。每颗INA226为一个电源轨上报电流、
总线电压和功率；板上没有ADC电流采样路径。`5v_usb_hub` 没有 INA226，也没有
ADC descriptor，因此不能单独测量其电流；`vdd_5v` 读数是包含 hub 支路的父轨
总量。任何J8器件、线缆、上拉或异域电源都必须重新验证总电容、上升时间、VOL和
电源域边界。`main`显式完成CH224A的
deferred init后，由独立低优先级线程约每100 ms只采样私有输入gate；present稳态
不产生CH224A I2C事务，也不重复读取capability。稳定的absent→present边沿会跳过
旧source的5秒probe退避并执行一次gate保护的probe/status discovery；移除或ADC错误
则不发I2C而立即清PD状态。这是轮询，不是IRQ。PD absent期间，VCC_3V3供电的
INA226遥测仍可工作。

CH224A访问由ADC0上的私有标准`voltage-divider` sensor门控。R46（100 kΩ）和
R47（15 kΩ）测量Q28之后、U10之前的`DC20V`分压；这是源侧输入路径，不是直接
测量CH224A VHV。15 kΩ输出电阻、115 kΩ全分压以及2.8 V present/2.5 V absent
迟滞均只由Device Tree描述。输入为absent/unknown时，驱动必须取得间隔至少5 ms
的三次稳定样本后才允许任何CH224A I2C事务；present后每笔事务仍重新采样。

CH224A寄存器`0x09`只要获得I2C ACK并成功读回一个字节，就视为有效读取。驱动只
解析已命名的status mask；不能因为unknown/reserved bit置位而拒绝`0x00`、`0x80`、
`0x88`、`0xff`或未来值。未知位每次启动最多记录一次包含原始字节的warning，随后
忽略；不得因此触发retry、bus recovery，也不得清presence或PD capability缓存。

CH224A probe遇到bus busy/stuck或timeout分类时，每个probe序列最多调用一次标准
`i2c_recover_bus()`。现有PIO driver在bus lock下执行有界、fail-closed且仅开漏的
恢复序列；普通地址NAK（`-EIO`）绝不触发bus clear，Zephyr通用I2C shell继续禁用。
恢复SCL脉冲之前和恢复后的probe retry之前都必须重新通过ADC gate。

该voltage-divider实例只在CH224A锁内使用，本身不是公共telemetry descriptor。
独立的公共`adc0` descriptor通过raw ADC driver读取同一个ADC pad，上报的是pad电压，
不是分压器还原后的源侧电压；driver的`adc_context`负责串行化ADC0与其它通道。
未来若上报源侧电压，必须新增由firmware按Device Tree换算的独立字段，不能由host对
公共`adc0`执行`115/15`倍乘。

| 电源轨 | 器件 | 7 位地址 | 采样电阻 |
| --- | --- | --- | --- |
| `20v_out` | U9 | `0x40` | 10 mOhm |
| `12v_out` | U15 | `0x41` | 10 mOhm |
| `5v_out` | U11 | `0x44` | 10 mOhm |
| `vdd_5v` | U12 | `0x45` | 10 mOhm |

J8只引出ADC1/GP27、ADC2/GP28和ADC3/GP29；ADC0/GP26不是J8端子。
ADC1/ADC2不是电源轨采样输入。ADC0至ADC3均为没有电源状态的纯监测pad电压通道；
ADC3（`GP29`）仍由`adc3`拥有且仅输入。固件先上报四个INA226电流descriptor，再
上报这四个ADC descriptor；主机直接展示固件值，不做主机侧校准表、零点修正或
分压换算。合约见
[ADC telemetry 合约](../reference/adc-telemetry.md)。

## 原理图参考

- [原理图（rev X1.2）](../hardware/RA051_MB_X1.2.pdf)
- [位号图（X1.2）](../hardware/RA051_V12_20260920位号图.pdf)
