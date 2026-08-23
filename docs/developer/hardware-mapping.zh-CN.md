# 硬件映射

[English](hardware-mapping.md)

## 硬件支持策略

当前正式支持并完成验证的目标是 G3 / RP2350A。G2 / RP2040 已退役，
不再参与构建、发布、维护和 HIL 覆盖。RP2354 仍属于后续 board 移植；
在完成专用 Flash 布局和板级验证之前，不得直接复用当前 Pico 2 的
4 MB board 定义。

## RP2350A 引脚表

| 功能 | 固件名称 | 原理图信号 | GPIO |
| --- | --- | --- | --- |
| 12 V 输出使能 | `12v_out` | `GP02_12V_EN` | 2 |
| 5 V 输出使能 | `5v_out` | `GP05_5V_EN` | 0 |
| 5 V WS / VDD_5V 常开电源轨 | `5v_ws` | `GP09_5V_WS_EN` | 1 |
| 20 V 输出使能 | `20v_out` | `GP10_20V_EN` | 3 |
| TF/SD 路由切换 | `switch sd` | `GP06_TF_SW` | 4 |
| USB hub mux 切换 | `switch usb` | `GP03_USB3_HUB` | 5 |
| CH347 1.8 V VIN 电源 | `switch vin` 内部控制 | `1V8_EN` | 6 |
| TF 写保护 | — | `TF_WP` | 22 |
| CH347 VIO 电压选择 | `switch vin` | `VIO_SEL` | 23 |
| 测试点 | — | `TP15` | 24 |
| 状态 LED | — | `LED_BLUE` | 25 |
| GPIO 别名 | `CON_MAS` | `CON_MAS` | 7 |
| GPIO 别名 | `CON_REST` | `CON_REST` | 8 |
| GPIO 别名 | `CON_USER` | `CON_USER` | 9 |
| J16 GPIO 范围 | `GP10`-`GP20` | — | 10-20 |
| J16 ADC3 / GPIO | `ADC3` / `GP29` | — | 29 (ADC3) |
| 5 V 电流监测 | `adc read 5v_out` | `S_C_5V` | 26 (ADC0) |
| 12 V 电流监测 | `adc read 12v_out` | `S_C_12V` | 27 (ADC1) |
| 20 V 电流监测 | `adc read 20v_out` | `S_C_20V` | 28 (ADC2) |

## 状态 LED（GPIO25）

GPIO25 是蓝色状态 LED，低电平有效。它通过 Device Tree chosen 属性驱动，
作为 watchdog 心跳指示灯，而非使用 Zephyr 内置心跳驱动或 `CONFIG_LED`。
LED 以约 1 Hz 频率闪烁（完整的开/关周期），仅在硬件 watchdog 喂狗成功后推进。
当固件拥有该 GPIO 时，跳过或失败的喂狗会将 LED 重置为非活动状态。

## 电源轨和 VIN

VIN 启动默认值为 3.3V。GPIO1 VDD_5V 及其 GPIO6 VDD_1V8 子电源轨在
Device Tree 模型中保持常开。可选 CH347 VIO 电平使用标准 `regulator-gpio`
regulator 建模，包含精确的 1.8V 和 3.3V states，固件通过 Zephyr regulator
API 选择电压。切换前请确认目标板支持所选电压。

## 电流监测

电流监测通道使用 INA139、10 mOhm 采样电阻和 50 kOhm 输出负载。
MCU 同时上报原始 ADC 调试值，以及通过 Zephyr
`current-sense-amplifier` 标准接口得到的电流值；主机侧现在直接展示这些值，
不再做 ADC 校准表或零点修正。
传感器传输函数参考公开的
[TI INA139 规格书](https://www.ti.com/product/INA139)。

## 原理图参考

当前原理图副本放在：
- [docs/hardware/radxa-linkr-debugger-schematic-x1.1.pdf](../hardware/radxa-linkr-debugger-schematic-x1.1.pdf)
