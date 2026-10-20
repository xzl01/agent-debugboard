[中文](board-overview.zh-CN.md)

# Board Overview

Radxa Linkr Debugger is a USB-controlled hardware bridge for automated board bring-up, recovery, and debug. One USB cable to your PC gives you control over target power, boot mode, SD routing, current monitoring, and GPIO — all scriptable from the CLI, Web UI, or an AI agent.

The connector locations and reference designators for `RA051_MB_X1.2` are in the
[X1.2 reference designator drawing](../hardware/RA051_V12_20260920位号图.pdf).
Electrical definitions are in the
[X1.2 schematic](../hardware/RA051_MB_X1.2.pdf). Use the tables below for signal
definitions; the marketing top-view artwork is not a pinout.

## Connectors and Ports

| Ref. | Interface | Description |
|---|---|---|
| J15 | PC / debug USB-C | Primary PC connection for USB NCM, the console CDC ACM, the J4 CDC ACM, and the CH347F hub |
| J1 | USB-C power input | Board `POWER IN` input |
| J3 | Target USB-C | Target/OTG USB connection routed through the onboard hub |
| J2 | TF / microSD slot | Card path can be routed between the target and host USB reader |
| J5 | 12 V output | Controllable `12v_out` screw terminal |
| J6 | 20 V output | Controllable `20v_out` screw terminal |
| J7 | 5 V output | Controllable `5v_out` screw terminal |
| J8 | GPIO / ADC header | Safe GPIO subset, ADC inputs, PIO I2C, USB signals, grounds, and hub 5 V |
| J13 | Recovery GPIO header | `CON_MAS`, `CON_REST`, `CON_USER`, and GND |
| J4 | Target UART | 3-pin UART1 TX/RX/GND header |

Before attaching a cable or probe, confirm the pin-1 marker on the board
silkscreen.

### Host USB

Single USB-C connection at J15 to the PC. The board enumerates as a composite device:
- **USB NCM** — network interface, control plane at `http://172.29.203.1`
- **USB CDC ACM** — console serial fallback for the Zephyr shell and BOOTSEL recovery
- **USB CDC ACM (J4)** — dedicated target UART bridge

No driver installation is needed on Linux or macOS. Windows needs the standard
CDC ACM driver.

### Target Debug (CH347F)

The onboard CH347F is a standalone USB debug bridge on the board's USB hub. It
provides target UART and JTAG/SWD channels and appears as its own USB device.
The RP2354A firmware is not in its JTAG/SWD data path. x1.2 has no VIO voltage
selection; use 3.3 V signaling. The J4 UART below is the RP2354A UART1 bridge,
not a CH347F channel.

### Power Outputs

Three controllable power rails for target boards:

| Output | Voltage | Use case |
|--------|---------|----------|
| `5v_out` | 5V | SBCs, dev boards |
| `12v_out` | 12V | Higher-power targets |
| `20v_out` | 20V | USB-PD-class targets |

`vdd_5v` is the internal auxiliary 5 V rail for the USB hub. It is also exposed
as a controllable output in the CLI/API, and firmware gates it with the
`switch usb` route.

Each rail has an INA226 current/voltage/power monitor readable via `adc read`.
ADC3 (`GP29`) is a separate monitor-only voltage input and is not a rail-current
sensor.

### TF/SD Card Slot

Micro-SD slot with a hardware mux that routes the card between two paths:

| Route | Description |
|-------|-------------|
| `target` (default) | SD card appears on the target board |
| `usb-reader` | SD card appears as a USB mass storage on the host |

Switch with `radxa-linkr-debuggerctl switch route sd usb-reader`.

### USB Device Switching

`switch usb` selects whether the shared USB device path is routed to the PC or
to the target. The firmware also gates the `vdd_5v` hub domain with this route:

```sh
radxa-linkr-debuggerctl switch route usb target --confirm
radxa-linkr-debuggerctl switch route usb pc --confirm
```

Unmount storage devices and stop active USB transfers before changing the
route. Use `radxa-linkr-debuggerctl switch get usb` to verify the active path.

### GPIO Headers

J8 and J13 are side-facing headers on the board's right edge. All orientations
and coordinates below are a **component-side top view**, not the mating-face
view. J8 is above J13. For J8, the left column runs from pin 10 at the top to
pin 1 at the bottom; the right column runs from pin 20 at the top to pin 11 at
the bottom. Firmware uses `row=0` for the top and `column=0` for the left.

**J13** (2×2) — recovery control signals:

| UI row | Left contact (`column=0`) | Right contact (`column=1`) |
|---|---|---|
| 0 (top) | Pin 2: GP9 / `CON_USER` / USER | Pin 3: GP8 / `CON_REST` / RSET |
| 1 (bottom) | Pin 1: GP7 / `CON_MAS` / MASKROM | Pin 4: GND (not interactive) |

**J8** (2×10) — general-purpose GPIO, analog inputs, and I2C:

| UI row | Left contact (`column=0`) | Right contact (`column=1`) |
|---|---|---|
| 0 (top) | Pin 10: GP21 / PIO I2C SDA | Pin 20: `5V_USB_HUB` |
| 1 | Pin 9: GP20 / PIO I2C SCL | Pin 19: USB DM |
| 2 | Pin 8: GP27 / ADC1 | Pin 18: USB DP |
| 3 | Pin 7: GP28 / ADC2 | Pin 17: GND |
| 4 | Pin 6: GP29 / ADC3 (safe, input-only) | Pin 16: GP5 |
| 5 | Pin 5: GP16 (safe) | Pin 15: GP6 |
| 6 | Pin 4: GP15 (safe) | Pin 14: GND |
| 7 | Pin 3: GP14 (safe) | Pin 13: USB DM |
| 8 | Pin 2: GP13 (safe) | Pin 12: USB DP |
| 9 (bottom) | Pin 1: GP12 (safe) | Pin 11: GND |

The mapped safe-GPIO control subset is GP7–GP9 on J13 plus GP12–GP16 and
input-only GP29/ADC3 on J8. GP17 and GP18 remain output-capable safe GPIOs, but
they have no confirmed J8/J13 placement; firmware therefore omits their
`layoutGroup`, `layoutRow`, and `layoutColumn`, and clients display them through
the generic fallback. The UI is only this allowlisted control subset, not a
connector pinout or mating-face diagram. Use the complete component-side tables
above and the schematic when wiring the headers.

The logic analyzer can capture GP10–GP20. SINGLE and FAST8 reach 125 MHz;
WIDE11 is limited to 100 MHz. GP29 is excluded from the logic analyzer because
ADC3 owns it. GP20 is sampled as a read-only SCL tap and must not be driven or
pulled low while the I2C bus is active.

### Target UART (J4)

J4 is a 3-pin UART on UART1, bridged to a dedicated USB CDC ACM:

| Pin | Signal |
|-----|--------|
| 1 | UART1 TX (GP10) |
| 2 | UART1 RX (GP11) |
| 3 | GND |

GP10/GP11 are shared with the logic analyzer. While a capture selects either
pin, PIO2 owns it and the J4 UART is unavailable; do not expect serial traffic
during the capture. After the capture ends, firmware restores the
`GPIO_FUNC_UART_AUX` mux on GP10/GP11 and the J4 bridge works again without a
reboot.

### Status LED

Blue LED on GPIO24, active-high. Acts as a watchdog heartbeat — blinks at ~1 Hz
when firmware is healthy. Stops blinking if the watchdog trips.

## Quick reference

```
radxa-linkr-debuggerctl power set 5v_out on       # power the target
radxa-linkr-debuggerctl switch route sd usb-reader  # read SD from host
radxa-linkr-debuggerctl gpio set GP13 1             # drive a GPIO
radxa-linkr-debuggerctl adc read                    # check current draw
radxa-linkr-debuggerctl adc read vdd_5v             # check the USB-hub rail
radxa-linkr-debuggerctl doctor                      # full connection check
```
