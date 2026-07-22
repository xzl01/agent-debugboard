[中文](board-overview.zh-CN.md)

# Board Overview

Radxa Linkr Debugger is a USB-controlled hardware bridge for automated board bring-up, recovery, and debug. One USB cable to your PC gives you control over target power, boot mode, SD routing, current monitoring, and GPIO — all scriptable from the CLI, Web UI, or an AI agent.

<!-- TODO: add board photo/diagram here -->

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

### GPIO Headers

**J13** (2×2) — three dedicated GPIOs:

| Pin | GPIO | Label | Typical use |
|-----|------|-------|-------------|
| 1 | GP7 | CON_MAS | MASKROM entry signal |
| 2 | GP8 | CON_REST | Reset control |
| 3 | GP9 | CON_USER | User-defined |

**J16** (6×2) — general-purpose GPIO and analog input:

| Pins | GPIO | Notes |
|------|------|-------|
| 1–11 | GP10–GP20 | Digital I/O, also used by logic analyzer |
| 12 | GP29 | ADC3 analog input |

All J13 and J16 pins are in the safe GPIO allowlist. The logic analyzer can capture on any of these pins at up to 125 MHz.

### Status LED

Blue LED on GPIO25. Acts as a watchdog heartbeat — blinks at ~1 Hz when firmware is healthy. Stops blinking if the watchdog trips.

## Quick reference

```
radxa-linkr-debugctl power set 5v_out on       # power the target
radxa-linkr-debugctl switch route sd usb-reader  # read SD from host
radxa-linkr-debugctl gpio set GP13 1             # drive a GPIO
radxa-linkr-debugctl adc read                    # check current draw
radxa-linkr-debugctl doctor                      # full connection check
```
