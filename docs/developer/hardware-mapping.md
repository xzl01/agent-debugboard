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
| J8 pin 16 safe GPIO | `GP5` | `GPIO5_CON` | 5 |
| J8 pin 15 safe GPIO | `GP6` | `GPIO6_CON` | 6 |
| J4 UART TX | UART1 TX | `GPIO10_CON` | 10 |
| J4 UART RX | UART1 RX | `GPIO11_CON` | 11 |
| CH224A PD strap | — (firmware keeps high-Z input) | `RP2354_PD_CFG1` | 19 |
| PIO I2C SCL / WIDE11 lane 10 | I2C | `RP2354_SCL` | 20 |
| PIO I2C SDA | I2C | `RP2354_SDA` | 21 |
| TF write-protect | `switch tf_wp` | `TF_WP` | 22 |
| USB OTG enable | — | `OTG_USB_EN` | 23 |
| USB-hub 5 V enable | `5v_usb_hub` | `GPIO24_USB_CRL` | 24 |
| Status LED | — | `LED_BLUE` | 25 |
| ADC0 / CH224A input gate | private `ch224a_input_voltage` | `GPIO26_ADC0`, DC20V divider | 26 |
| ADC1 | — | `GPIO27_ADC1` | 27 |
| ADC2 | — | `GPIO28_ADC2` | 28 |
| ADC3 / GP29 | `adc3` | `GPIO29_ADC3` | 29 |
| Recovery GPIO / J13 pin 1 | `CON_MAS` | `GP07_MAS` | 7 |
| Recovery GPIO / J13 pin 3 | `CON_REST` | `GP08_REST` | 8 |
| Recovery GPIO / J13 pin 2 | `CON_USER` | `GP09_USER` | 9 |
| J8 pins 1-5 safe GPIO | `GP12`-`GP16` | `GPIO12`-`GPIO16` | 12-16 |

`GP10`/`GP11` belong to the J4 UART and are not in the safe-GPIO catalog. They
are also logic-analyzer lanes (SINGLE on GP10; FAST8/WIDE11 on GP10/GP11).
While a capture owns them, PIO2 drives the capture function and J4 is
unavailable; cleanup restores `GPIO_FUNC_UART_AUX` on the configured pins.
`GP19` is the CH224A PD strap, and `GP20`/`GP21` are the PIO I2C bus; none of
these are exposed as GPIO controls. `GP29` is cataloged but input-only because
`adc3` owns it.

## Connector Orientation And Confirmed Mapping

The following is the component-side top view confirmed from the original x1.2
PDF. Both J8 and J13 are on the board's **right edge**. This is a confirmed
mapping of the relevant control, monitoring, and power subset, not a complete
connector pinout; an omitted or empty position must never be inferred as another
GPIO.

J8 is a 2x10 connector. From the top, its left column is pins `10` through `1`
and its right column is pins `20` through `11`:

| J8 pin | Signal | Firmware role |
|---|---|---|
| 10 | GP21 | PIO I2C SDA, reserved from safe-GPIO control |
| 20 | `5V_USB_HUB` | switched `5v_usb_hub` child-rail output; not a GPIO |
| 9 | GP20 | PIO I2C SCL, reserved from safe-GPIO control |
| 8 | GP27 / ADC1 | ADC1 header terminal; not rail sensing |
| 7 | GP28 / ADC2 | ADC2 header terminal; not rail sensing |
| 6 | GP29 / ADC3 | `adc3`, safe catalog input-only |
| 5 | GP16 | safe GPIO |
| 4 | GP15 | safe GPIO |
| 3 | GP14 | safe GPIO |
| 2 | GP13 | safe GPIO |
| 1 | GP12 | safe GPIO |
| 16 | GP5 / `GPIO5_CON` | direct safe GPIO; default input, no pulls |
| 15 | GP6 / `GPIO6_CON` | direct safe GPIO; default input, no pulls |

ADC0/GP26 has no J8 terminal in this confirmed mapping. GP17 and GP18 are not
in the current safe/control catalog.

J13 is a 2x2 connector. In the same component-side top view:

| | Left | Right |
|---|---|---|
| Top | pin 2: GP9 / USER | pin 3: GP8 / RSET |
| Bottom | pin 1: GP7 / MASKROM | pin 4: GND |

J13 pin 4 is **GND**, not 3.3 V. The safe-GPIO allowlist has eleven entries:
output-capable `GP5`, `GP6`, `GP7`-`GP9`, `GP12`-`GP16`, plus input-only
`GP29`. The 24 physical cells therefore contain 11 controls and 13 inert cells;
10 controls are output-capable.

## Status LED (GPIO25)

GPIO25 is the blue status LED, active-high in the Device Tree. It operates as a
watchdog heartbeat driven through Device Tree chosen properties rather than a
Zephyr built-in heartbeat driver or `CONFIG_LED`. The LED blinks at
approximately 1 Hz (full on/off cycle) and advances only after the hardware
watchdog feed succeeds. Skipped or failed feeds reset the LED to the inactive
state while firmware owns the GPIO. GPIO24 is not the LED; it enables
`5v_usb_hub`.

## Power rails

Five outputs are controllable: `5v_out`, `12v_out`, `20v_out`, `vdd_5v`, and
`5v_usb_hub`. `vdd_5v` is the USB-hub auxiliary 5 V parent domain, and
`5v_usb_hub` is its switched child branch on GPIO24. All enables are active-high
through a P-FET or load-switch stage. Board 3.3 V is generated from USB5V by
`U14`; x1.2 has no selectable VIO/1.8 V regulator and no `switch/vin` item.
`vdd_5v` is an independent firmware-owned power state; x1.2 has no
`switch/usb`. Boot state is defined by Device Tree, persistent replay, and
firmware initialization; read the live status instead of imposing a host-side
default.

Parent-child setter semantics are firmware-owned: enabling `5v_usb_hub` enables
`vdd_5v` first when needed; disabling `5v_usb_hub` leaves `vdd_5v` unchanged;
disabling `vdd_5v` disables `5v_usb_hub` first; enabling `vdd_5v` alone does not
enable `5v_usb_hub`. A child-only saved `5v_usb_hub=on` snapshot therefore
replays the parent on as well.

## Current and voltage sensing

Four INA226 devices share the PIO I2C bus at the 400 kHz production default.
The qualified baseline uses production 4.7 kOhm pull-ups to always-powered
VCC_3V3 with J8 unloaded. Each INA226 reports current, bus voltage, and power
for one rail; there is no on-board ADC current path. `5v_usb_hub` has no INA226
and no ADC descriptor, so its current is not separately measurable; the
`vdd_5v` reading is the parent total including the hub branch. Any J8 device,
cable, pull-up, or foreign power domain requires a new
capacitance/rise-time/VOL and power-domain validation. After deferred CH224A initialization from `main`, a
dedicated low-priority thread samples only the private input gate about every
100 ms. It performs no steady-state CH224A I2C traffic or capability reads. A
stable absent-to-present edge triggers one gated probe/status discovery without
waiting for the old source's five-second backoff; removal or ADC failure clears
PD state without I2C. This is polling, not an interrupt. INA226 telemetry remains
available from VCC_3V3 while PD is absent.

CH224A access is gated by a private standard `voltage-divider` sensor on ADC0.
R46 (100 kOhm) and R47 (15 kOhm) scale `DC20V` after Q28 and before U10; this is
the source-side input path, not a direct CH224A VHV measurement. Device Tree is
the sole owner of the 15 kOhm output / 115 kOhm full-divider facts and the
2.8 V present / 2.5 V absent hysteresis. From absent/unknown state the driver
requires three samples at least 5 ms apart before any CH224A I2C transaction;
while present, every transaction gets a fresh sample.

CH224A register `0x09` is considered readable whenever I2C acknowledges and
returns one byte. The driver decodes only named status masks; it does not reject
`0x00`, `0x80`, `0x88`, `0xff`, or future values because an unknown/reserved bit
is set. Unknown bits produce one boot-time warning containing the raw byte and
are otherwise ignored; they do not trigger retries, bus recovery, or presence
and PD-cache clearing.

Probe errors classified as bus busy/stuck or timeout may request one standard
`i2c_recover_bus()` attempt per CH224A probe sequence. The existing PIO driver
performs that bounded recovery under its bus lock using its fail-closed,
open-drain-only sequence. Ordinary address NAK (`-EIO`) never triggers bus
clear, and the generic Zephyr I2C shell remains disabled. Recovery itself is
ADC-gated both before its SCL pulses and before the following probe retry.

The voltage-divider instance remains private to the CH224A lock and is not a
separate public telemetry descriptor. The public `adc0` descriptor uses the same
Device Tree divider facts to report canonical source-side DC20V voltage; its
`raw` and `mv` remain ADC-pad diagnostics. ADC1-3 report native pad voltage.
The ADC driver's `adc_context` serializes ADC0 with the other channels. No host
may apply `115/15` or another divider ratio to any public ADC field. ADC0's
uncalibrated accuracy is approximately 2%.

| Rail | Device | 7-bit address | Shunt |
| --- | --- | --- | --- |
| `20v_out` | U9 | `0x40` | 10 mOhm |
| `12v_out` | U15 | `0x41` | 10 mOhm |
| `5v_out` | U11 | `0x44` | 10 mOhm |
| `vdd_5v` | U12 | `0x45` | 10 mOhm |

J8 exposes ADC1/GP27, ADC2/GP28, and ADC3/GP29; ADC0/GP26 is not a J8 terminal.
ADC1/ADC2 are not rail-sense inputs. ADC0 is the PD-input source-side channel;
ADC1-3 are native pad-voltage channels. None has a power state; ADC3 (`GP29`)
remains owned by `adc3` and input-only. The firmware reports four INA226 current
descriptors followed by these four ADC descriptors, and the host presents them
without a host-side calibration table, zero-point correction, or divider
conversion. See the [ADC telemetry contract](../reference/adc-telemetry.md).

## Schematic reference

- [Schematic (rev X1.2)](../hardware/RA051_MB_X1.2.pdf)
- [Reference Designators (X1.2)](../hardware/RA051_V12_20260920位号图.pdf)
