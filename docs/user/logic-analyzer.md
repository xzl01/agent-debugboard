[中文](logic-analyzer.zh-CN.md)

# Logic Analyzer

RP2350 PIO2+DMA high-speed single-shot GPIO capture for short-burst diagnostics
at PIO rates. Not intended for sustained streaming at high sample rates.

The logic analyzer is available in the Web UI under the **Terminal workspace**,
alongside the serial terminal.

## Safe GPIO Pins

Only the following pins are available for logic analyzer capture:

| Pin | x1.2 ownership / access |
| --- | --- |
| GP7–GP9 | J13 recovery controls (`CON_MAS`, `CON_REST`, `CON_USER`) |
| GP10–GP11 | J4 target UART; capture temporarily takes ownership from UART |
| GP12–GP16 | J8 pins 1–5 |
| GP17–GP18 | No confirmed J8/J13 mapping; select by firmware GPIO name only |
| GP19 | PD CFG1 strap; capture-only internal signal, never drive it |
| GP20 | J8 pin 9, PIO I2C SCL; read-only tap, never drive or pull it low |
| GP29 | J8 pin 6, ADC3-owned and input-only |

No other GPIO pins can be used. In particular, GP21 is J8 pin 10 / PIO I2C
SDA and is not a WIDE11 channel. This list is an analyzer capability list, not
a physical connector pinout; use firmware GPIO metadata and the x1.2 board
drawing before attaching probes.

## Configuration

| Parameter | Range | Notes |
| --- | --- | --- |
| Channels | 1–16 | Selected from the safe pin allowlist above |
| Sample rate | 100,000–125,000,000 Hz | Actual rate may differ due to PIO clock divider quantization |
| post_samples | 1–512 | Samples captured after the trigger |
| pre_samples | 0–512 | Requires edge trigger + rate ≤25 MHz; total (pre+post) capped at 512 |

50 MHz and 125 MHz are very short single-shot bursts. For longer captures, use
a lower rate.

## Trigger Modes

| Mode | Behavior |
| --- | --- |
| `none` | Free-running capture, no trigger edge |
| `rising` | Capture starts on a low-to-high transition |
| `falling` | Capture starts on a high-to-low transition |
| `either` | Capture starts on any edge transition |

### Pre-Trigger Sampling

Set `pre_samples > 0` with an edge trigger (`rising`, `falling`, or `either`) to
capture samples both before and after the trigger edge. The trigger index in the
returned data is at position `pre_samples`.

- Supported at sample rates ≤25 MHz only.
- `pre_samples > 0` with `trigger: "none"` is rejected (HTTP 400).
- `pre_samples > 0` with rate >25 MHz is rejected (HTTP 400).
- Total capture (`pre_samples + post_samples`) is capped at 512 samples.

## Capture States

The analyzer progresses through these states:

```
idle → armed → capturing → done
                         → error
```

## HTTP API

### Arm a Capture

```
POST /api/v1/logic-analyzer
```

Request body example:

```json
{
  "selected_pins": [13, 15],
  "sample_rate_hz": 1000000,
  "pre_samples": 0,
  "post_samples": 512,
  "trigger": "rising"
}
```

Response (on success):

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": true,
  "command": "logic-analyzer",
  "action": "armed",
  "requestedSampleRateHz": 1000000,
  "actualSampleRateHz": 977600,
  "samplePeriodPs": 1024000,
  "backend": "rp2350-pio2-dma"
}
```

### Get Status / Retrieve Capture

```
GET /api/v1/logic-analyzer
```

Returns current state (`idle`, `armed`, `capturing`, `done`, `error`).

```
GET /api/v1/logic-analyzer/capture
```

Returns completed capture with samples:

```json
{
  "state": "done",
  "sampleCount": 512,
  "triggerIndex": 127,
  "requestedSampleRateHz": 1000000,
  "actualSampleRateHz": 977600,
  "samplePeriodPs": 1024000,
  "backend": "rp2350-pio2-dma",
  "config": { ... },
  "samples": [
    {"timestampUs": 0, "values": 0},
    {"timestampUs": 1, "values": 3}
  ]
}
```

### Cancel Capture

```
DELETE /api/v1/logic-analyzer
```

### Response Metadata

| Field | Description |
| --- | --- |
| `requestedSampleRateHz` | Rate requested in configuration |
| `actualSampleRateHz` | Actual rate achieved by PIO clock divider |
| `samplePeriodPs` | Sample period in picoseconds |
| `backend` | Capture backend (`rp2350-pio2-dma`) |
| `sampleCount` | Number of samples in capture |
| `triggerIndex` | Index of the trigger sample |

### Error Codes

| Code | HTTP Status | Cause |
| --- | --- | --- |
| `already_armed` | 409 | POST while analyzer is `armed` or `capturing` |
| `invalid_config` | 400 | Invalid JSON, out-of-range params, pre_samples with no edge trigger, or pre-trigger capture above 25 MHz |
| `arm_failed` | 500 | Internal firmware failure |

## Web UI

The Logic Analyzer card is in the **Terminal workspace** of the Web UI.

### Waveform Preview

Captured samples display as an SVG waveform visualization directly in the
browser. Each configured pin is shown as a separate channel.

### Protocol Decoding

A browser-based Rust/WASM decoder can decode captured waveforms into protocol
annotations. Supported protocols:

- UART
- I2C
- SPI

The decoder is served from these stable URLs:

- `/assets/decoder/logic-decoder.js`
- `/assets/decoder/logic-decoder_bg.wasm` (`application/wasm`, gzip-compressed)

To decode: arm a capture, wait for `done`, then use the in-browser decoder.
Annotations are rendered directly on the waveform view.

### Export Formats

| Format | Description |
| --- | --- |
| CSV | Raw sample data with timestamps |
| PulseView `.sr` | Sigrok-compatible file, openable in PulseView with the configured sample rate |

## Transport And Port Allocation

| Endpoint | Purpose |
| --- | --- |
| raw TCP 5556 | Native Sigrok/BeagleLogic logic-analyzer protocol |
| raw TCP 5555 | J4 raw UART only; never logic-analyzer traffic |
| TCP 80, `rigol-ds` | Separate Rigol SCPI compatibility personality |
| `/api/v1/scpi` WebSocket | Browser SCPI compatibility transport |

## Rigol SCPI Compatibility

Separately from native raw TCP 5556, firmware emulates a Rigol DS1102D
mixed-signal oscilloscope on the existing SCPI compatibility path, so PulseView
and sigrok-cli can connect through the `rigol-ds` driver.

### Connection

```
tcp-raw/<board-ip>/80
```

Port 80 multiplexes by first byte: HTTP traffic goes to the web server, SCPI
traffic goes to the emulation. The same SCPI engine is also reachable over
WebSocket at `ws://<board>/api/v1/scpi` for browser clients.

### Channel Map

| PulseView Channel | Firmware GPIO mapping |
| --- | --- |
| D0–D11 | Fixed protocol order: GP10, GP16, GP11, GP17, GP12, GP18, GP13, GP19, GP14, GP20, GP15, GP29 |
| D12–D14 | GP7, GP8, GP9 (J13 pins 1, 3, 2 respectively) |
| CH1 (analog) | GP29 / ADC3 (J8 pin 6) |

The D0–D11 sequence is a compatibility channel order, not a connector order or
a complete physical pinout. GP10/GP11 are on J4, GP12–GP16 and GP20/GP29 have
the J8 placements listed above, GP17/GP18 have no confirmed J8/J13 placement,
and GP19 remains the non-drivable PD CFG1 strap.

### Behavior

- **≤25 MHz**: real hardware pre-trigger capture (300 pre + 212 post samples).
- **>25 MHz**: single-shot burst of 512 samples, trigger edge aligned to sample
  300 via software.
- **AUTO fallback**: if no edge arrives within the timeout, an untriggered frame
  is returned.

### sigrok-cli Examples

```sh
# Scan for the board
sigrok-cli -d rigol-ds:conn=tcp-raw/172.29.203.1/80 --scan

# Digital capture with hardware pre-trigger
sigrok-cli -d rigol-ds:conn=tcp-raw/172.29.203.1/80 \
  --config timebase='20 us' --config triggersource=D0 \
  --config triggerslope=f --frames 1 --channels D0 -o capture.sr

# Analog GP29 channel
sigrok-cli -d rigol-ds:conn=tcp-raw/172.29.203.1/80 \
  --config timebase='1 ms' --frames 1 --channels CH1 -o analog.sr
```

Use the `rigol-ds` driver in PulseView with connection string
`tcp-raw/<board-ip>/80`.

## Deep Capture

Deep captures record into the 2 MB SPI-flash storage instead of RAM, enabling
up to 1 million samples.

### Limits

| Parameter | Value |
| --- | --- |
| Digital rate | ≤25 kHz |
| Analog rate (GP29) | ≤10 kHz |
| Max samples | 1,000,000 |
| Storage | 2 MB SPI-flash partition |

### Vendor SCPI Commands

| Command | Description |
| --- | --- |
| `:LINKR:DEEP:START <rate_hz> [duration_s]` | Erase window and begin capture (duration default 2 s, max 30 s) |
| `:LINKR:DEEP:STATUS?` | Query status: `IDLE`, `PREPARING`, `CAPTURING`, or `DONE` |
| `:LINKR:DEEP:DATA? <offset> <count>` | Download captured samples (2 bytes each digital, 1 byte analog) |
| `:LINKR:DEEP:STOP` | Abort current capture |

### Web UI

Click the **Deep** button on the Logic Analyzer card (defaults: 25 kHz, 2 s
window). The UI shows PREPARING/CAPTURING progress, downloads samples into the
waveform view, and exports CSV or PulseView `.sr` of up to 1 million samples.

## BeagleLogic Emulation

The native Sigrok/BeagleLogic protocol listens on raw TCP port **5556** and
emulates the BeagleLogic kernel driver, providing continuous acquisition in
PulseView. The listener binds `0.0.0.0`, so use the board's current NCM address.

Port **5555 is not a logic-analyzer port**. It is the binary raw-UART transport
for the MCU-owned J4 target serial console. Connecting a Sigrok/BeagleLogic
client to 5555 cannot negotiate or produce capture frames.

### Connection

```
beaglelogic:conn=tcp-raw/<board-ip>/5556
```

### Channels

14 digital channels in fixed protocol order (not connector order):

- ch0–ch11: GP10, GP16, GP11, GP17, GP12, GP18, GP13, GP19, GP14, GP20,
  GP15, GP29
- ch12–ch13: GP7, GP8 (J13 pins 1 and 3)

These channel numbers preserve the wire protocol and do not imply a shared
header. Resolve probe locations from current firmware metadata and the x1.2
board drawing.

### Rate and Sample Format

| Format | Sustained Rate |
| --- | --- |
| 16-bit | Up to ~150 kHz |
| 8-bit | Up to ~100 kHz |

Rates at or above 100 kHz use the hardware PIO+DMA path. Lower rates use a
paced GPIO-register loop.

### Connection troubleshooting

- Use raw TCP **5556** for native Sigrok/BeagleLogic and raw TCP **5555** only
  for J4 UART.
- If 5556 refuses a connection, verify the current board NCM address and that no
  WebSocket or TCP Sigrok session already owns the analyzer.
- If the client receives target-console bytes instead of protocol frames, it is
  connected to J4 on 5555; disconnect and use 5556.

## Streaming Mode (1–25 MHz)

For real-time waveform monitoring at lower sample rates, use continuous
streaming via the same SCPI scope protocol carried over WebSocket.

### Transport

```
ws://<board>/api/v1/scpi
```

The Web UI streams 600-sample live frames in a loop. Each frame is a gap-free
island delivered at tens-of-milliseconds cadence. This is not a contiguous
multi-MHz record.

### Browser UI

Click **Stream** on the Logic Analyzer card to start (disabled above 25 MHz).
A live status line shows streaming state, actual rate, and total received
samples. A rolling live waveform renders the buffered sample history per
selected pin.

The stream keeps up to one million buffered samples with window-size selection
(1K–256K), a follow-latest toggle, and a pan slider for browsing older history.

The decoder operates on single-shot captures for full post-hoc analysis, not on
stream data.

## Quick Start

1. Open Web UI at `http://172.29.203.1/`
2. Select the **Logic Analyzer** tab in the Terminal workspace
3. Configure pins, sample rate, and trigger mode
4. Click **Arm capture**
5. View the waveform, or export as CSV / PulseView `.sr`
6. For live monitoring, click **Stream** (1–25 MHz)
7. For Rigol SCPI compatibility, connect with the `rigol-ds` driver via
   `tcp-raw/172.29.203.1/80`
8. For native Sigrok/BeagleLogic raw TCP, connect to
   `beaglelogic:conn=tcp-raw/172.29.203.1/5556`
9. Keep raw TCP 5555 reserved for the J4 UART console

## Further Reading

- [Implementation details](../reference/logic-analyzer.md) — firmware architecture,
  API reference, rate guidance, and file-level source map
- [PulseView](https://sigrok.org/wiki/PulseView)
- [Sigrok file format](https://sigrok.org/wiki/File_format:Sigrok/v2)
