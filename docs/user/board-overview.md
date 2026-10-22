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
- **USB NCM** — network interface; the control-plane URL is the board address printed by `ip`
- **USB CDC ACM** — console serial fallback for the Zephyr shell and BOOTSEL recovery
- **USB CDC ACM (J4)** — one sink/source of the MCU J4 serial runtime; observed
  as ACM4 in the final x1.2 HIL, but host tty numbering is not stable

No driver installation is needed on Linux or macOS. Windows needs the standard
CDC ACM driver.

### Target Debug (CH347F)

The onboard CH347F is a standalone USB debug device on the board's USB hub. Its
valid product role documented here is target JTAG/SWD; RP2354A firmware is not
in that debug path. x1.2 has no VIO voltage selection, so use 3.3 V signaling.
Any still-shipped host CH347 UART tooling is a legacy non-J4 path. The J4 UART
below is owned by the RP2354A runtime and never traverses CH347.

### Power Outputs

Six firmware-advertised power outputs are controllable:

| Output | Voltage | Use case |
|--------|---------|----------|
| `5v_out` | 5V | SBCs, dev boards |
| `12v_out` | 12V | Higher-power targets |
| `20v_out` | 20V | USB-PD-class targets |
| `vdd_5v` | 5V | internal auxiliary/parent 5 V rail for the USB hub |
| `5v_usb_hub` | 5V | switched child branch for the USB hub |
| `5v_otg` | 5V | bidirectional J3 VBUS switch, child of `vdd_5v` |

`vdd_5v` is the internal auxiliary 5 V parent rail. `5v_usb_hub` is its switched
child on GPIO24 and boots off. Enabling the hub enables `vdd_5v` first when
needed; disabling the hub leaves the parent alone; disabling `vdd_5v` turns the
hub off first; enabling `vdd_5v` does not enable the hub. `5v_otg` is the
GPIO23/Q10 switch for external J3 VBUS; it is also a `vdd_5v` child, and
external VBUS can backfeed the parent through Q10's body diode. x1.2 has no USB
route switch and no `/switch/usb` endpoint.

The four INA226 monitors cover `5v_out`, `12v_out`, `20v_out`, and `vdd_5v`;
they are readable via `adc read`. `5v_usb_hub` and `5v_otg` have no INA226 and no ADC
descriptor, so the `vdd_5v` reading is the parent total including the hub
branch; a released `5v_otg` is not a measured 0 V. ADC0 is the PD-input source-side DC20V channel (`role="pd_input"`,
Device Tree divider reconstruction); `raw`/`mv` remain pad diagnostics. ADC1-3
are native pad-voltage inputs (`role="native"`), not rail-current sensors.
ADC3 is GP29 and input-only in the safe GPIO catalog. No host applies `115/15`.
ADC0 accuracy is uncalibrated and about 2%.

### TF/SD Card Slot

Micro-SD slot with a hardware mux that routes the card between two paths:

| Route | Description |
|-------|-------------|
| `target` (default) | SD card appears on the target board |
| `usb-reader` | SD card appears as a USB mass storage on the host |

Switch with `radxa-linkr-debuggerctl switch route sd usb-reader`.

### x1.2 switch controls

The firmware switch catalog contains only `sd` and `tf_wp`. `switch/sd` is
driven by MCU GPIO4 on schematic net `GP06_TF_SW`; `switch/tf_wp` uses GPIO22.
J8 USB D+/D− contacts are inert connector signals, not route controls.
`GET /api/v1/switch/usb` returns HTTP 404, and `vdd_5v` remains independent of
switch routing; `5v_usb_hub` is its power child.

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
| 4 | Pin 6: GP29 / ADC3 (safe, input-only) | Pin 16: GP5 / `GPIO5_CON` (safe) |
| 5 | Pin 5: GP16 (safe) | Pin 15: GP6 / `GPIO6_CON` (safe) |
| 6 | Pin 4: GP15 (safe) | Pin 14: GND |
| 7 | Pin 3: GP14 (safe) | Pin 13: USB DM |
| 8 | Pin 2: GP13 (safe) | Pin 12: USB DP |
| 9 (bottom) | Pin 1: GP12 (safe) | Pin 11: GND |

The safe-GPIO control set is GP7–GP9 on J13 plus GP5, GP6, GP12–GP16 and
input-only GP29/ADC3 on J8. GP5/GP6 are ordinary direct RP2354 GPIOs, default
input with no pulls; they are not USB/TF switches. GP17 and GP18 are absent.
The UI has 11 controls across 24 physical cells (13 inert); 10 controls are
output-capable. It is not a
connector pinout or mating-face diagram. Use the complete component-side tables
above and the schematic when wiring the headers.

The logic analyzer can capture GP10–GP20. SINGLE and FAST8 reach 125 MHz;
WIDE11 is limited to 100 MHz. GP29 is excluded from the logic analyzer because
ADC3 owns it. GP20 is sampled as a read-only SCL tap and must not be driven or
pulled low while the I2C bus is active.

### Target UART (J4)

J4 is one logical 3-pin target serial port on UART1. The runtime accepts any
integer baud from 4800 through 115200; the UI also offers five recommended
presets, with 115200 as the default. Rates above 115200 are rejected; earlier
high-rate samples are diagnostic only, not product support. The pinout is:

| Pin | Signal |
|-----|--------|
| 1 | UART1 TX (GP10) |
| 2 | UART1 RX (GP11) |
| 3 | GND |

A USB default line coding while DTR is false is staged only and does not change
the active baud or generation; HTTP/WS settings applied in that low state take
effect immediately. Opening the terminal applies staged settings before it
publishes DTR and opens ingress. A failed apply/attach/detach leaves J4 blocked
until a successful LOW/reset.

The repo-local `linkr_debugger_serial` runtime owns UART1 and a 4096-byte shared
RX ring. It fans out independently to J4 CDC ACM, four `serial.j4` WebSocket
sinks, and two raw-TCP clients on port 5555. The board-hosted xterm.js terminal
uses the MCU WebSocket path; it does not use host Web Serial or CH347.

Explicit input remains multiwriter. Only automatic terminal replies have one
selected responder: CDC while DTR is asserted, otherwise the lowest-numbered
attached WebSocket. Other terminal pages still send explicit keyboard, IME,
paste, and Ctrl-C input and are not read-only; the page labels them
**Auto-reply observer** / **Manual input allowed**. Raw TCP is never the
automatic responder. See
[the J4 serial transport contract](../reference/serial-broker.md) for the wire
fields and responder matrix.

GP10/GP11 are shared with the logic analyzer. Before PIO2 takes either pin,
firmware pauses J4 and handles active TX wire completion; capture traffic is
then unavailable. Cleanup restores `GPIO_FUNC_UART_AUX`, resumes fresh sink
epochs, and the same J4 sessions can continue without reboot.

### Status LED

Blue LED on GPIO25, active-high. Acts as a watchdog heartbeat — blinks at ~1 Hz
when firmware is healthy. Stops blinking if the watchdog trips. GPIO24 is the
`5v_usb_hub` enable, not the LED.

## Quick reference

```
radxa-linkr-debuggerctl power set 5v_out on       # power the target
radxa-linkr-debuggerctl switch route sd usb-reader  # read SD from host
radxa-linkr-debuggerctl gpio set GP13 1             # drive a GPIO
radxa-linkr-debuggerctl adc read                    # check current draw
radxa-linkr-debuggerctl adc read vdd_5v             # check the parent USB-hub rail
radxa-linkr-debuggerctl power set 5v_usb_hub on     # enable the hub branch (also enables vdd_5v)
radxa-linkr-debuggerctl doctor                      # full connection check
```
