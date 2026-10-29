[中文](logic-analyzer.zh-CN.md)

# Logic Analyzer

RP2350 PIO2+DMA high-speed single-shot GPIO capture for short-burst diagnostics
at PIO rates. Not intended for sustained streaming at high sample rates.

The logic analyzer has its own **Logic Analyzer workspace** in the Web UI,
separate from the serial terminal.

## Capture GPIO Pins

The target capture set is **eight digital logic inputs: GP12–GP16 and
GP27–GP29**. ADC1–ADC3 name the latter three contacts; here they mean digital
HIGH/LOW capture, not analog voltage sampling.

The Web UI is **temporarily limited to GP12–GP16** (five channels), with GP13
selected by default. GP27–GP29 capture support is **not implemented** and awaits
review of firmware channel mapping, pin ownership and the protocol contract.
Their current read-only display is a temporary safety state, not completion of
the eight-channel requirement. The J8/J13 diagram remains the physical layout
reference:

| Pin / contact | Web UI behavior |
| --- | --- |
| GP12–GP16 | Selectable digital capture pins, J8 pins 1–5 |
| GP7–GP9 | Disabled J13 recovery controls (`CON_MAS`, `CON_REST`, `CON_USER`) |
| ADC1–ADC3 (GP27/GP28/GP29) | Retained contacts, J8 pins 8/7/6; capture support pending, currently read-only and not sampled by Web capture |
| GP5, GP6, GP20 and other non-capture contacts | Inert layout context; no capture or trigger gestures |

The underlying native sigrok/CLI hardware allowlist remains **GP10–GP20**.
Its former supplemental capture entries GP10/GP11/GP17–GP20 are no longer
offered by the Web picker. This UI limit does not change the sigrok protocol or
its WIDE11 channel mapping. GP10/GP11 belong to J4 UART; GP19 is the internal,
non-drivable PD CFG1 strap; GP20 is J8 pin 9 / PIO I2C SCL and must never be
driven or pulled low. GP21 (J8 pin 10 / SDA) is not a WIDE11 channel. Use firmware
GPIO metadata and the x1.2 board drawing before attaching probes.

**J4 is still affected:** the current FAST8 backend takes the GP10–GP17 physical
span, including GP10/GP11, and pauses J4 UART even when the Web selection contains
only GP12–GP16. Hiding UART pins in the picker does not isolate them from capture.

## Configuration

| Parameter | Range | Notes |
| --- | --- | --- |
| Web UI channels | 1–5 for now | GP12–GP16 only; ADC1–ADC3 capture remains pending |
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

Open the **Logic Analyzer workspace** in the Web UI.
Sampling settings and pin selection stay side by side on wider screens (stacked
on smaller screens). A separate full-width waveform section sits below both
controls, followed by protocol settings and results on the same page. There is
no Capture/Protocol Decode mode switch.

### Waveform Preview

Captured samples display as an SVG waveform visualization directly in the
browser. Each configured pin is shown as a separate channel. Bounded capture and
live stream use the same waveform area, with protocol annotations aligned to the
same time axis rather than a second waveform preview.
The settings preview shows the requested rate. The actual rate beside the
waveform comes from the capture acknowledgement; editing settings for the next
capture does not change the rate or time axis of an existing capture.

During a stream, clear **Follow latest** to freeze the current absolute sample
window while acquisition continues. The slider selects another retained window;
enabling Follow latest returns to the newest samples. The browser retains the
latest 1,000,000 samples. If the frozen window is evicted, a warning appears and
the plot moves to the earliest available window instead of silently relabeling
newer samples as the old history.

The stream preview groups samples into display-width buckets and keeps their
first and last edges, so rapid activity and single-sample pulses do not appear
as a false constant level. Dense activity is compressed: reduce the window size
for closer inspection; the overview does not show every individual edge.

### Protocol Decoding

A browser-based Rust/WASM decoder can decode captured waveforms into protocol
annotations. Supported protocols:

- UART
- I2C
- SPI

The decoder is served from these stable URLs:

- `/assets/decoder/logic-decoder.js`
- `/assets/decoder/logic-decoder_bg.wasm` (`application/wasm`, gzip-compressed)

Protocol settings can be prepared before capture. For a bounded capture, wait
for `done`, then run the in-browser decoder on the same page. Changing protocol
settings makes the previous result stale until decoding is run again. During a
live stream, annotations update automatically using the visible protocol
settings. Invalid channel mappings and decoder failures appear in the protocol
panel; correct the settings to resume decoding. Annotations are drawn directly
on the existing waveform; export,
capture cancellation and stream stop remain separate capture actions.

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
J8 pins 1–5, 9 and 6 respectively, GP17/GP18 have no confirmed J8/J13 placement,
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
sigrok-cli -d rigol-ds:conn=tcp-raw/<board-ip>/80 --scan

# Digital capture with hardware pre-trigger
sigrok-cli -d rigol-ds:conn=tcp-raw/<board-ip>/80 \
  --config timebase='20 us' --config triggersource=D0 \
  --config triggerslope=f --frames 1 --channels D0 -o capture.sr

# Analog GP29 channel
sigrok-cli -d rigol-ds:conn=tcp-raw/<board-ip>/80 \
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

## Stream Mode

The Web UI uses a sigrok live session and its binary WebSocket protocol for
Stream. This is separate from the optional Rigol SCPI compatibility interface
described above.

### Transport

The UI creates `/api/v1/live-sessions`, connects to the returned
`/api/v1/ws/<slot>` URL, and sends a sigrok `CONFIG` with `post=0`. Up to 25 MHz
this is the rolling Stream path. The browser retains at most one million recent
samples; this bounded browser history is not a promise of an indefinitely
lossless recording.

At supported high-rate combinations, 100/125 MHz Stream is a single lossless
packed burst rather than indefinite monitoring. It requires negotiated
`GENERIC_PACKED_BURST`; the `post=0` sentinel remains a CONFIG v1 request and
does not require `CONFIG_V2`. The stream stops when the device's 100000-sample
buffer fills and remains visible as a read-only final Stream snapshot. WIDE11
at 125 MHz remains unsupported. Bounded captures whose post-trigger depth
exceeds 65535 still require `CONFIG_V2`.

### Browser UI

Click **Stream** in the Logic Analyzer workspace. The status line shows the
requested and actual rate, total received samples, and whether a retained
snapshot stopped automatically, manually, or incompletely. A rolling waveform
renders the buffered history per selected pin. High-rate capability and
automatic-stop notices are shown before and after the operation.

The stream keeps up to one million buffered samples with window-size selection
(1K–256K), a follow-latest toggle, and a pan slider for browsing older history.

The decoder supports full post-hoc analysis of single-shot captures and updates
live annotations from a bounded window of stream data.

## Quick Start

1. Open Web UI at `http://<board-ip>/`
2. Open the **Logic Analyzer** workspace
3. Configure pins, sample rate, and trigger mode
4. Click **Arm capture**
5. View the waveform, or export as CSV / PulseView `.sr`
6. For rolling monitoring up to 25 MHz, click **Stream**; supported 100/125 MHz
   combinations run one 100000-sample packed burst and retain a read-only result
7. For Rigol SCPI compatibility, connect with the `rigol-ds` driver via
   `tcp-raw/<board-ip>/80`
8. For native Sigrok/BeagleLogic raw TCP, connect to
   `beaglelogic:conn=tcp-raw/<board-ip>/5556`
9. Keep raw TCP 5555 reserved for the J4 UART console

## Further Reading

- [Implementation details](../reference/logic-analyzer.md) — firmware architecture,
  API reference, rate guidance, and file-level source map
- [PulseView](https://sigrok.org/wiki/PulseView)
- [Sigrok file format](https://sigrok.org/wiki/File_format:Sigrok/v2)
