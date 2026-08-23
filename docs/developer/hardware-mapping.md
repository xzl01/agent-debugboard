# Hardware Mapping

[中文](hardware-mapping.zh-CN.md)

## Hardware support policy

The active and validated target is G3 with RP2350A. G2/RP2040 is retired and is
not built, released, maintained, or included in HIL coverage. RP2354 remains a
future board port and must not reuse the current 4 MB Pico 2 board definition
without a dedicated flash layout and board-level validation.

## RP2350A Pin Table

| Function | Firmware name | Schematic signal | GPIO |
|---|---|---|---|
| 12 V output enable | `12v_out` | `GP02_12V_EN` | 2 |
| 5 V output enable | `5v_out` | `GP05_5V_EN` | 0 |
| 5 V WS / VDD_5V always-on rail | `5v_ws` | `GP09_5V_WS_EN` | 1 |
| 20 V output enable | `20v_out` | `GP10_20V_EN` | 3 |
| TF/SD route switch | `switch sd` | `GP06_TF_SW` | 4 |
| USB hub mux switch | `switch usb` | `GP03_USB3_HUB` | 5 |
| CH347 1.8 V VIN supply | internal to `switch vin` | `1V8_EN` | 6 |
| TF write-protect | — | `TF_WP` | 22 |
| CH347 VIO voltage select | `switch vin` | `VIO_SEL` | 23 |
| Test point | — | `TP15` | 24 |
| Status LED | — | `LED_BLUE` | 25 |
| GPIO alias | `CON_MAS` | `CON_MAS` | 7 |
| GPIO alias | `CON_REST` | `CON_REST` | 8 |
| GPIO alias | `CON_USER` | `CON_USER` | 9 |
| J16 GPIO range | `GP10`-`GP20` | — | 10-20 |
| J16 ADC3 / GPIO | `ADC3` / `GP29` | — | 29 (ADC3) |
| 5 V current monitor | `adc read 5v_out` | `S_C_5V` | 26 (ADC0) |
| 12 V current monitor | `adc read 12v_out` | `S_C_12V` | 27 (ADC1) |
| 20 V current monitor | `adc read 20v_out` | `S_C_20V` | 28 (ADC2) |

## Status LED (GPIO25)

GPIO25 is the blue status LED, active-low. It operates as a watchdog
heartbeat driven through Device Tree chosen properties rather than a Zephyr built-in
heartbeat driver or `CONFIG_LED`. The LED blinks at approximately 1 Hz (full on/off
cycle) and advances only after the hardware watchdog feed succeeds. Skipped or failed
feeds reset the LED to the inactive state while firmware owns the GPIO.

## Power rails and VIN

VIN defaults to 3.3V at boot. GPIO1 VDD_5V and its GPIO6 VDD_1V8 child rail are
always on in the Device Tree model. The selectable CH347 VIO level is modeled
as a standard `regulator-gpio` regulator with exact 1.8V and 3.3V states, and
firmware selects it through the Zephyr regulator API. Confirm your target
supports the selected voltage before applying it.

## Current monitor

The current monitor channels use INA139 with a 10 mOhm shunt and 50 kOhm
output load. The MCU reports raw ADC diagnostics plus
standard sensor current values from Zephyr's `current-sense-amplifier`
interface, and the host CLI now presents those values directly without any
host-side calibration table or zero-point correction.
See the public
[TI INA139 datasheet](https://www.ti.com/product/INA139) for the sensor
transfer function.

## Schematic reference

The current schematic copy is stored at:
- [docs/hardware/radxa-linkr-debugger-schematic-x1.1.pdf](../hardware/radxa-linkr-debugger-schematic-x1.1.pdf)
