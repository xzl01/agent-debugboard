# Hardware Mapping

[中文](hardware-mapping.zh-CN.md)

## Hardware support policy

The active target is the `RA051_MB_X1.2` board with an RP2354A (QFN-60,
RP2350A-compatible core and pinout). G2/RP2040 is retired and is not built,
released, maintained, or included in HIL coverage.

The build keeps the upstream `rpi_pico2/rp2350a/m33/mcuboot` board definition.
`U23` (external QSPI) is not populated on x1.2, so the board uses the 2 MB
internal flash. The 4 MB sysbuild partition layout only over-declares the
storage partition and does not move used data past 2 MB; moving to a 2 MB
layout would require a repo-local MCUboot board. See the
[x1.2 hardware and BSP reference](../reference/hardware-x1.2.md) for the
canonical board description.

## RP2354A Pin Table

| Function | Firmware name | Schematic signal | GPIO |
|---|---|---|---|
| 5 V output enable | `5v_out` | `GP0_5V_EN` | 0 |
| Auxiliary 5 V (USB hub) | `vdd_5v` | `GP01_5V_WS_EN` | 1 |
| 12 V output enable | `12v_out` | `GP02_12V_EN` | 2 |
| 20 V output enable | `20v_out` | `GP03_20V_EN` | 3 |
| TF/SD route switch | `switch sd` | `GP06_TF_SW` | 4 |
| USB device route switch | `switch usb` | `GPIO5` / USB mux | 5 |
| J4 UART TX | UART1 TX | `GPIO10_CON` | 10 |
| J4 UART RX | UART1 RX | `GPIO11_CON` | 11 |
| CH224A PD strap | — (firmware keeps high-Z input) | `RP2354_PD_CFG1` | 19 |
| PIO I2C SCL / WIDE11 lane 10 | I2C | `RP2354_SCL` | 20 |
| PIO I2C SDA | I2C | `RP2354_SDA` | 21 |
| TF write-protect | `switch tf_wp` | `TF_WP` | 22 |
| USB OTG enable | — | `OTG_USB_EN` | 23 |
| Status LED | — | `LED_BLUE` | 24 |
| ADC0 | — | `GPIO26_ADC0` | 26 |
| ADC1 | — | `GPIO27_ADC1` | 27 |
| ADC2 | — | `GPIO28_ADC2` | 28 |
| ADC3 / GP29 | `adc3` | `GPIO29_ADC3` | 29 |
| Recovery GPIO | `CON_MAS` | `GP07_MAS` | 7 |
| Recovery GPIO | `CON_REST` | `GP08_REST` | 8 |
| Recovery GPIO | `CON_USER` | `GP09_USER` | 9 |
| J8 safe GPIO | `GP12`-`GP18` | `GPIO12`-`GPIO18` | 12-18 |

`GP10`/`GP11` belong to the J4 UART and are not in the safe-GPIO catalog. They
are also logic-analyzer lanes (SINGLE on GP10; FAST8/WIDE11 on GP10/GP11).
While a capture owns them, PIO2 drives the capture function and J4 is
unavailable; cleanup restores `GPIO_FUNC_UART_AUX` on the configured pins.
`GP19` is the CH224A PD strap, and `GP20`/`GP21` are the PIO I2C bus; none of
these are exposed as GPIO controls. `GP29` is cataloged but input-only because
`adc3` owns it.

## Status LED (GPIO24)

GPIO24 is the blue status LED, active-high in the Device Tree. It operates as a
watchdog heartbeat driven through Device Tree chosen properties rather than a
Zephyr built-in heartbeat driver or `CONFIG_LED`. The LED blinks at
approximately 1 Hz (full on/off cycle) and advances only after the hardware
watchdog feed succeeds. Skipped or failed feeds reset the LED to the inactive
state while firmware owns the GPIO.

## Power rails

Four rails are controllable: `5v_out`, `12v_out`, `20v_out`, and `vdd_5v`
(the USB-hub auxiliary 5 V domain). All enables are active-high through a
P-FET stage. Board 3.3 V is generated from USB5V by `U14`; x1.2 has no
selectable VIO/1.8 V regulator and no `switch/vin` item. `vdd_5v` follows the
`switch usb` route in firmware. Boot state is defined by Device Tree and
firmware initialization; read the live status instead of imposing a host-side
default.

## Current and voltage sensing

Four INA226 devices share the PIO I2C bus at 200 kHz. Each reports current,
bus voltage, and power for one rail; there is no on-board ADC current path.

| Rail | Device | 7-bit address | Shunt |
| --- | --- | --- | --- |
| `20v_out` | U9 | `0x40` | 10 mOhm |
| `12v_out` | U15 | `0x41` | 10 mOhm |
| `5v_out` | U11 | `0x44` | 10 mOhm |
| `vdd_5v` | U12 | `0x45` | 10 mOhm |

ADC0-ADC2 are exposed on J8 and are not rail-sense inputs. ADC3 (`GP29`) is a
monitor-only voltage channel owned by `adc3`; it has no power state. The
firmware reports all five descriptors and the host presents them without a
host-side calibration table or zero-point correction. See the
[ADC telemetry contract](../reference/adc-telemetry.md).

## Schematic reference

- [Schematic (rev X1.2)](../hardware/RA051_MB_X1.2.pdf)
- [Reference Designators (X1.2)](../hardware/RA051_V12_20260920位号图.pdf)
