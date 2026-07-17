# Logic Analyzer

## Implementation Overview

The firmware logic analyzer uses RP2350 PIO2+DMA for high-speed single-shot
GPIO capture. It is not a polling-based diagnostic tool and does not support
sustained streaming at high rates.

The Web UI includes a project-owned Rust/WASM decoder that runs entirely in the
browser to decode captured samples. The stable decoder URLs are:
- `/assets/decoder/logic-decoder.js`
- `/assets/decoder/logic-decoder_bg.wasm`

Both assets are gzip-compressed and served from flash with appropriate MIME types
(JS for the glue, `application/wasm` for the WASM binary).

The decoder is inspired by sigrok decoder semantics but is a project-owned Rust
implementation that supports UART, I2C, and SPI protocols only. It does not
execute Python libsigrokdecode plugins and does not provide full plugin
compatibility.

## Technical Architecture

### Firmware Side

- **Implementation**: RP2350 PIO2 state machines with DMA transfer
- **Capture buffer**: 512 samples maximum per single-shot burst
- **Sample rate**: Requested rates from 1,000,000 through 125,000,000 Hz (1-125MHz);
  actual rate is derived from PIO clock divider and reported in response metadata
- **Trigger modes**: `none`, `rising`, `falling`, `either` via PIO edge detection
- **Pre-trigger**: Supported for edge triggers at ≤25 MHz; uses DMA ping-pong
  stream with software edge detection and rolling ring buffer
- **State machine**: `idle`, `armed`, `capturing`, `done`, `error`
- **HTTP fragment handling**: The logic analyzer POST endpoint uses 4x1024-byte slots
  and accumulates MORE/FINAL fragments for complete capture data

### Web Side

- **Decoder**: Project-owned Rust/WASM implementation under `web/decoder/`
- **Component**: `LogicAnalyzerCard.tsx`
- **Export formats**: CSV and PulseView .sr
- **Visualization**: SVG waveform display
- **Protocol support**: UART, I2C, SPI only

## Feature Status

| Feature | Status |
| --- | --- |
| RP2350 PIO2+DMA capture | Implemented |
| Up to 16 GPIO channels from safe allowlist | Implemented |
| Configurable sample rate 1-125MHz | Implemented |
| 512-sample single-shot bursts | Implemented |
| Edge trigger (rising/falling/either) | Implemented |
| Pre-trigger (edge triggers, ≤25 MHz) | Implemented |
| CSV export | Implemented |
| PulseView .sr export | Implemented |
| Web UI waveform display | Implemented |
| HTTP API control | Implemented |
| Browser WASM decode (UART/I2C/SPI) | Implemented |
| HTTP fragment accumulation (4x1024-byte slots) | Implemented |

## API Endpoints

### GET /api/v1/logic-analyzer

Get logic analyzer status.

**Response example**:

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": true,
  "command": "logic-analyzer",
  "action": "status",
  "state": "idle"
}
```

**State values**: `idle`, `armed`, `capturing`, `done`, `error`

### POST /api/v1/logic-analyzer

Arm capture.

**Request body example**:

```json
{
  "selected_pins": [13, 15],
  "sample_rate_hz": 1000000,
  "pre_samples": 0,
  "post_samples": 512,
  "trigger": "rising"
}
```

**Response example** (top-level fields):

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

### GET /api/v1/logic-analyzer/capture

Retrieve completed capture.

**Response example** (top-level and config):

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": true,
  "command": "logic-analyzer",
  "action": "capture",
  "state": "done",
  "sampleCount": 512,
  "triggerIndex": 127,
  "requestedSampleRateHz": 1000000,
  "actualSampleRateHz": 977600,
  "samplePeriodPs": 1024000,
  "backend": "rp2350-pio2-dma",
  "config": {
    "pinCount": 2,
    "sampleRateHz": 977600,
    "requestedSampleRateHz": 1000000,
    "actualSampleRateHz": 977600,
    "samplePeriodPs": 1024000,
    "backend": "rp2350-pio2-dma",
    "pinBase": 13,
    "triggerType": "rising"
  },
  "samples": [
    {"timestampUs": 0, "values": 0},
    {"timestampUs": 1, "values": 3}
  ]
}
```

### DELETE /api/v1/logic-analyzer

Cancel or release capture.

**Response example**:

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": true,
  "command": "logic-analyzer",
  "action": "released"
}
```

## Configuration Parameters

- `selected_pins`: Array of GPIO pin numbers from the safe allowlist (GP7, GP8,
  GP9, GP10-GP20, GP29), maximum 16 channels
- `sample_rate_hz`: Requested sample rate from 1,000,000 through 125,000,000 Hz
- `pre_samples`: Pre-trigger sample count (0-512 total); requires an edge trigger
  and rate ≤25 MHz. Ignored for `trigger: "none"`.
- `post_samples`: Number of samples after trigger (0-512 total); the total
  capture (`pre_samples + post_samples`) is capped at 512 samples
- `trigger`: `none`, `rising`, `falling`, or `either`

Safe GPIO pins for logic analyzer:

- `GP7` (`CON_MAS`)
- `GP8` (`CON_REST`)
- `GP9` (`CON_USER`)
- `GP10`-`GP20` (J16 header)
- `GP29` (ADC3)

## Response Metadata

The arm response includes:

- `requestedSampleRateHz`: The rate requested in the configuration
- `actualSampleRateHz`: The actual rate achieved by PIO clock divider
- `samplePeriodPs`: Sample period in picoseconds
- `backend`: The capture backend in use (`rp2350-pio2-dma`)

The capture response additionally includes:

- `sampleCount`: Number of samples in the capture
- `triggerIndex`: Index of the trigger sample within the capture
- `config`: Full configuration object with rate, pin, and trigger settings

## Error Codes

- `already_armed`: Returned on repeated POST while `armed` or `capturing`
- `invalid_config`: Invalid JSON, out-of-range parameters, `pre_samples > 0`
  with `trigger: "none"`, or `pre_samples > 0` with rate >25 MHz
- `arm_failed`: Internal arm failure

## Rate Guidance

50MHz and 125MHz are very short single-shot bursts; the firmware does not
claim sustained streaming at those rates. For longer captures at high rates,
use a lower requested rate. The actual sample period may differ slightly from
the requested rate due to PIO clock divider quantization.

Pre-trigger captures use the DMA ping-pong stream path and are limited to
rates ≤25 MHz. Requesting `pre_samples > 0` with `sample_rate_hz > 25000000`
returns `invalid_config`.

## Single-Shot Capture Limits

The single-shot capture buffer is 512 samples (2,048 bytes raw, ~16 KB
compressed JSON). This limit is hard-coded in firmware and cannot be increased
without expanding the static RAM allocation.

| Sample Rate | Channels | Capture Duration |
|-------------|----------|------------------|
| 1 MHz       | 1-16     | 512 µs           |
| 8 MHz       | 1-16     | 64 µs            |
| 25 MHz      | 1-16     | 20.5 µs          |
| 50 MHz      | 1-16     | 10.2 µs          |
| 100 MHz     | 1-16     | 5.1 µs           |
| 125 MHz     | 1-16     | 4.1 µs           |

## Continuous Streaming Mode

For real-time waveform monitoring at lower sample rates, use the continuous
streaming mode. This mode uses DMA ping-pong buffers and pushes chunks over
the existing WebSocket session.

**Supported rates**: 1 MHz through 25 MHz (single channel recommended for
rates above 8 MHz). Higher requested rates are rejected with HTTP 400
("stream rate exceeds 25 MHz maximum") because the per-block DMA interrupt
path cannot sustain more; single-shot captures support up to 125 MHz.
WebSocket delivery is decoupled from the sample rate: chunks are compressed
and formatted only for a bounded share of DMA blocks (~200 per second), so
multi-MHz streaming cannot saturate the system workqueue.

**Endpoint**: `POST /api/v1/logic-analyzer/stream`

**Request body**: Same as single-shot arm, but `trigger` must be `none`.

**Response**: Returns `action: "stream_started"` with `actualSampleRateHz`.

**Stop**: `DELETE /api/v1/logic-analyzer/stream`. The stream also stops
automatically when the last subscribed WebSocket client disconnects (for
example when the browser tab closes), so an abandoned stream never blocks a
later start with `already_armed`.

**WebSocket**: Subscribe to the live session with `type: "subscribe"` to
receive `logic-chunk` messages:

```json
{"type":"logic-chunk","seq":0,"count":1024,"values":[0,1,3,2,...]}
```

Each chunk contains 1024 compressed 16-bit sample words. The `seq` field
increments with each chunk. Chunks are produced at sample rate and delivered
best-effort: when the firmware cannot keep up (for example a slow client),
chunks are dropped and the loss is visible as `seq` gaps. Clients should not
expect a contiguous sample stream at multi-MHz rates.

**Browser UI**: Click the **Stream** button in the Logic Analyzer card to
start continuous capture (disabled above 25 MHz with a hint). A live status
line (streaming badge, actual rate, total received sample count) appears
while streaming, and a rolling **live waveform** renders a window of the
stream history per selected pin, refreshing about seven times per second.
The stream keeps up to one million buffered samples; the toolbar offers
window-size selection (1K-256K samples), a follow-latest toggle, and a pan
slider for browsing older history while streaming continues. When the
decoder is configured (UART/I2C/SPI pins), live protocol annotations are
decoded from the visible window and overlaid on the waveform. The decoder
still operates on single-shot captures for full post-hoc analysis. Click
**Stop stream** to end the session; the WebSocket close handshake completes
without errors.

## Pre-Trigger Captures

Pre-trigger sampling captures a window of samples around a trigger edge.
Set `pre_samples > 0` with an edge trigger (`rising`, `falling`, or `either`)
to enable it. The firmware uses a rolling ring buffer (512 entries) and
software edge detection over the DMA ping-pong stream.

**Rate limit**: ≤25 MHz (same as continuous streaming).

**Behavior**: The ring buffer continuously records stream samples. When the
configured edge is detected, the firmware collects the requested number of
post-trigger samples, then finalizes. The returned capture contains the
`pre_samples` most recent samples before the edge, followed by `post_samples`
samples after it, with `triggerIndex = pre_samples`.

**Limitations**:

- Pre-trigger requires an edge trigger; `trigger: "none"` with `pre_samples > 0`
  is rejected.
- The PIO `in pins` instruction requires the pad function to be set to a PIO
  peripheral. If another subsystem (e.g., the HTTP GPIO API) switches the pad
  function to SIO while the stream is running, the PIO input path is
  disconnected and samples freeze. For target-board signals this is not an
  issue — only GPIO API loopback testing is affected.
- Trigger detection scans samples chunk-by-chunk (1024 samples per chunk at
  ≤25 MHz). The edge must appear within the stream's sample window; at1 MHz
  the ring holds ~500 µs of history.

## Usage

 1. Open Web UI: `http://172.29.203.1/`
 2. Select the **Terminal workspace** tab
 3. Find **Logic Analyzer** card
 4. Configure parameters (optional)
 5. Click **Arm capture** to start acquisition
 6. Wait for capture to complete
 7. Click **CSV** or **PulseView (.sr)** to export data
 8. Open the .sr file in PulseView to view waveforms

## File Reference

- `apps/radxa_linkr_debugger/src/linkr_debugger_logic_analyzer.h`
- `apps/radxa_linkr_debugger/src/linkr_debugger_logic_analyzer.c`
- `web/src/components/LogicAnalyzerCard.tsx`

## Resources

- [PulseView](https://sigrok.org/wiki/PulseView)
- [Sigrok file format](https://sigrok.org/wiki/File_format:Sigrok/v2)
- [RP2350 datasheet](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf)
