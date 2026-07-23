[中文](board-overview.zh-CN.md)

# Board Overview

Radxa Linkr Debugger is a USB-controlled hardware bridge for automated board bring-up, recovery, and debug. One USB cable to your PC gives you control over target power, boot mode, SD routing, current monitoring, and GPIO — all scriptable from the CLI, Web UI, or an AI agent.

![Radxa Linkr Debugger top-view board illustration](../../doc/marketing/radxa-linkr-debugger-board-top-view.svg)

*Top-view illustration for locating the major connectors. Use the connector
tables and schematic below for electrical definitions and pin numbering.*

### Interface Callouts

The blue numbers are documentation callouts, not PCB silkscreen identifiers.
The `Jxx` values are the schematic reference designators.

| No. | Ref. | Interface | Description |
|---:|---|---|---|
| 1 | J18 | SPI header | CH347F SPI breakout: SCS0, MISO, SCK, MOSI, 3.3 V, and ground |
| 2 | J11 | UART / I2C header | CH347F dual-UART and I2C-capable signal breakout |
| 3 | J8 | JTAG / SWD header | Target debug signals with selectable 1.8 V / 3.3 V VIO |
| 4 | J13 | Recovery GPIO header | `CON_MAS`, `CON_REST`, and `CON_USER`; pin 1 is NC |
| 5 | J3 | Target USB-C | USB 3 target/OTG connection routed through the onboard hub |
| 6 | J12 | USB device switch | Stacked USB 3 ports: upper to target, lower for the switchable USB device |
| 7 | J1 | DC input | Main 20 V input through the DC5525 barrel connector |
| 8 | J6 | 20 V output | Controllable `20v_out` screw terminal |
| 9 | J5 | 12 V output | Controllable `12v_out` screw terminal |
| 10 | J7 | 5 V output | Controllable `5v_out` screw terminal |
| 11 | J2 | TF / microSD slot | Card path can be routed between the target and host USB reader |
| 12 | J15 | PC + Debug USB-C | Primary PC connection for USB NCM, CDC ACM, and CH347F |

The simplified illustration does not separately show the J16 safe GPIO header;
use the J16 pin table below when connecting GPIO or logic-analyzer probes.

Connector numbering below follows the current G3 schematic
([`doc/radxa-linkr-debugger-schematic-x1.1.pdf`](../../doc/radxa-linkr-debugger-schematic-x1.1.pdf)).
Before attaching a cable or probe, confirm the pin-1 marker on the board
silkscreen.

## Connectors and Ports

### Host USB

Single USB-C connection to the PC. The board enumerates as a composite device:
- **USB NCM** — network interface, control plane at `http://172.29.203.1`
- **USB CDC ACM** — serial fallback for Zephyr shell and BOOTSEL recovery

No driver installation needed on Linux or macOS. Windows needs the standard CDC ACM driver.

### Target Debug (CH347F)

The onboard CH347F provides two UART channels and a JTAG/SWD port, wired directly to the target debug connector. The RP2350 firmware does not sit in the JTAG/SWD data path — CH347F acts as a standalone debug probe.

| Channel | Device suffix | Purpose |
|---------|--------------|---------|
| UART0 | `D1` | Primary target serial (U-Boot, kernel, login) |
| UART1 | `D3` | Secondary serial channel |
| JTAG/SWD | — | Target debug via OpenOCD |

VIO voltage is selectable between 3.3V (default) and 1.8V via `switch vin`. Both UART channels share the same VIO level.

### Power Outputs

Three controllable power rails for target boards:

| Output | Voltage | Use case |
|--------|---------|----------|
| `5v_out` | 5V | SBCs, dev boards |
| `12v_out` | 12V | Higher-power targets |
| `20v_out` | 20V | USB-PD-class targets |

Each rail has a current-sense monitor (INA139) readable via `adc read`.

`5V_FIN` is the board's power input — not a controllable output.

### TF/SD Card Slot

Micro-SD slot with a hardware mux that routes the card between two paths:

| Route | Description |
|-------|-------------|
| `target` (default) | SD card appears on the target board |
| `usb-reader` | SD card appears as a USB mass storage on the host |

Switch with `radxa-linkr-debuggerctl switch route sd usb-reader`.

### USB Device Switching (J12)

J12 is a stacked USB 3 connector with two distinct roles:

- **Upper port** — connects to the target board.
- **Lower port** — accepts the USB device to be shared.

The USB mux routes the device in the lower port to either the PC connected at
J15 or the target connected through the upper port:

```sh
radxa-linkr-debuggerctl switch route usb target --confirm
radxa-linkr-debuggerctl switch route usb pc --confirm
```

Unmount storage devices and stop active USB transfers before changing the
route. Use `radxa-linkr-debuggerctl switch get usb` to verify the active path.

### GPIO Headers

**J13** (2×2) — three dedicated GPIOs and one unconnected pin:

| Pin | GPIO | Label | Typical use |
|-----|------|-------|-------------|
| 1 | — | NC | Not connected; do not use as GPIO |
| 2 | GP9 | CON_USER | User-defined |
| 3 | GP7 | CON_MAS | MASKROM entry signal |
| 4 | GP8 | CON_REST | Reset control |

**J16** (6×2) — general-purpose GPIO and analog input:

| Pin | GPIO | Notes |
|------|------|-------|
| 1 | GP10 | Digital I/O, logic analyzer capable |
| 2 | GP16 | Digital I/O, logic analyzer capable |
| 3 | GP11 | Digital I/O, logic analyzer capable |
| 4 | GP17 | Digital I/O, logic analyzer capable |
| 5 | GP12 | Digital I/O, logic analyzer capable |
| 6 | GP18 | Digital I/O, logic analyzer capable |
| 7 | GP13 | Digital I/O, logic analyzer capable |
| 8 | GP19 | Digital I/O, logic analyzer capable |
| 9 | GP14 | Digital I/O, logic analyzer capable |
| 10 | GP20 | Digital I/O, logic analyzer capable |
| 11 | GP15 | Digital I/O, logic analyzer capable |
| 12 | GP29 | ADC3 analog input |

The three connected J13 GPIOs and all twelve J16 signal pins are in the safe
GPIO allowlist. The logic analyzer can capture on these GPIOs at up to 125 MHz.

### Status LED

Blue LED on GPIO25. Acts as a watchdog heartbeat — blinks at ~1 Hz when firmware is healthy. Stops blinking if the watchdog trips.

## Quick reference

```
radxa-linkr-debuggerctl power set 5v_out on       # power the target
radxa-linkr-debuggerctl switch route sd usb-reader  # read SD from host
radxa-linkr-debuggerctl gpio set GP13 1             # drive a GPIO
radxa-linkr-debuggerctl adc read                    # check current draw
radxa-linkr-debuggerctl doctor                      # full connection check
```
