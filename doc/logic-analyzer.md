# Logic Analyzer

## Architecture Overview

The firmware logic analyzer uses RP2350 PIO2+DMA for high-speed GPIO capture.
It is not a polling-based diagnostic tool and does not support sustained streaming
at high rates.

### Control Paths

The logic analyzer has two distinct control paths:

**Web UI**: Creates a live session through `/api/v1/live-sessions` and speaks
the sigrok binary protocol on the returned `/api/v1/ws/<slot>` WebSocket URL.
This is not an HTTP logic-analyzer protocol. Pre-trigger capture is intentionally
not exposed in the Web UI.

**Native sigrok/PulseView**: Connects directly via raw-TCP on port 5556.
Use `sigrok-cli -d linkr-debugger:conn=tcp-raw/172.29.203.1/5556` or the
PulseView GUI with the same connection string.

### Supported Modes

| Mode | Pins | Description |
|------|------|-------------|
| FAST8 | GP10-GP17 | 8-channel capture |
| WIDE12 | GP10-GP20 + GP29 | 12-channel capture; GP29 is bit 11 |

GP7-GP9 are not available in Sigrok modes. The Web UI sigrok pin selection
is limited to GP10-GP20 plus GP29.

### Capture Parameters

**CONFIG pre/post are uint16, bounded 1..65535.**

Bounded **Arm** captures use requested sample rates from 100,000 through
125,000,000 Hz (100kHz-125MHz) and post-trigger sample counts from 1 through
65535. Pre-trigger is intentionally not exposed in the Web UI; requests always
send `pre_samples=0` explicitly.

Continuous **Stream** mode supports 1-25 MHz and sends `post_samples=0` until
stopped by the user.

### Hardware Buffer

The capture buffer is the 32 KiB RP2350 hardware write ring. DATA metadata is
8 bytes. Sample indices are modulo 24 bits; wrap is not a terminal condition.

On overrun, possible overrun, transport backpressure, or bounded-capture
completion, the firmware emits a terminal event and stops rather than silently
continuing.

Arena pause/resume is a lossless, generation-scoped handshake. The ADC sampler
atomically consumes preposted CONFIG/RESUME events, and an old generation's
RESUME cannot satisfy a newer pause request. Arena ownership also remains held
until the resume callback completes. Clients must not need an artificial delay
between a completed capture and an immediate restart.

### Measured No-Gap Continuous Ceilings

On the representative HIL setup:

| Scenario | Rate | Result |
|----------|------|--------|
| WS bounded SINGLE 100 kHz, post=65535 | 100 kHz | Exactly 65535 samples, 0 gaps, restart true, HTTP health true |
| WS bounded FAST8 100 kHz, post=65535 | 100 kHz | Exactly 65535 samples, 0 gaps, restart true, HTTP health true |
| WS bounded WIDE12 100 kHz, post=65535 | 100 kHz | Exactly 65535 samples, 0 gaps, restart true, HTTP health true |
| TCP bounded SINGLE 100 kHz, post=65535 | 100 kHz | Exactly 65535 samples, 0 gaps, restart true, HTTP health true |
| TCP bounded FAST8 100 kHz, post=65535 | 100 kHz | Exactly 65535 samples, 0 gaps, restart true, HTTP health true |
| TCP bounded WIDE12 100 kHz, post=65535 | 100 kHz | Exactly 65535 samples, 0 gaps, restart true, HTTP health true |
| WS continuous SINGLE | 1 MHz | 10 consecutive 5-second runs, ~4.991M-4.997M samples each, 998.16-998.70 ksps effective, zero sample-index gaps, zero disconnects, STOP response, immediate restart and HTTP health |
| WS continuous FAST8 | 240 kHz | 5-second no-gap ceiling |
| WS continuous FAST8 | 241 kHz | Adjacent failure, explicit terminal, restart and HTTP health retained |
| WS continuous WIDE12 | 149 kHz | 5-second no-gap ceiling |
| WS continuous WIDE12 | 150 kHz | Adjacent failure, explicit terminal, restart and HTTP health retained |
| TCP continuous SINGLE | 443 kHz | 5-second no-gap ceiling |
| TCP continuous SINGLE | 444 kHz | Adjacent failure, explicit terminal, restart and HTTP health retained |
| TCP continuous FAST8 | 241 kHz | 5-second no-gap ceiling |
| TCP continuous FAST8 | 242 kHz | Adjacent failure, explicit terminal, restart and HTTP health retained |
| TCP continuous WIDE12 | 147 kHz | 5-second no-gap ceiling |
| TCP continuous WIDE12 | 148 kHz | Adjacent failure, explicit terminal, restart and HTTP health retained |
| WS bounded SINGLE NONE post=1 | actual 125.081 MHz | Exactly 1 sample, 0 gaps, restart true, HTTP health true |
| TCP bounded SINGLE NONE post=1 | actual 125.081 MHz | Exactly 1 sample, 0 gaps, restart true, HTTP health true |
| WS bounded SINGLE rising post=512 | 5 MHz | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS bounded SINGLE falling post=512 | 5 MHz | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS bounded SINGLE either post=512 | 5 MHz | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS bounded SINGLE rising post=512 | 25 MHz | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS bounded SINGLE falling post=512 | 25 MHz | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS bounded SINGLE rising post=512 | 50 MHz | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS bounded SINGLE falling post=512 | 50 MHz | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS bounded SINGLE rising post=512 | 100 MHz | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS bounded SINGLE falling post=512 | 100 MHz | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS bounded SINGLE either post=512 | 100 MHz | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| TCP bounded SINGLE rising post=512 | 100 MHz | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS continuous SINGLE | 1 MHz | 4,997,120 samples, 999,340.8 samples/s, zero sample-index gaps, zero disconnects |

Adjacent WS SINGLE failure was not measured under the final architecture. The 1MHz
result is a verified operating point, not a claimed absolute ceiling. 50MHz and
125MHz are short single-shot bursts; the firmware does not claim sustained
streaming at those rates.

### Bounded Capture Engine (pre=0, post=1..512)

Bounded captures with pre=0 and post=1..512 use an exact finite PIO+DMA engine:
trigger NONE starts immediately ungated, rising and falling edges use hardware
IRQ-gated detection, and EITHER first snapshots the current pin level in
firmware then waits for the opposite edge using the same 3-instruction trigger
path. Because EITHER samples the pin level in firmware before arming the
hardware trigger, there is an arm-time race window. Larger bounded requests
(post>512) and continuous post=0 use ring streaming instead.

### Architecture

The LA backend uses a 32 KiB / 8192-entry RP2350 hardware DMA write ring with
7168 usable sample threshold before the safety margin is consumed. A
protocol-neutral LA sink feeds consumer work items through a pool of 8 DATA slots
plus 1 terminal slot. Each WS SINGLE DATA slot carries up to 2048 packed samples;
the sink consumer runs at priority 7 and blocks naturally on full chunks. Legacy
callback paths, TCP transport, and non-SINGLE sample paths run at priority 8 with
yield. The sender applies one-byte RLE with BIT_PACK fallback. A 6144-byte
coalescing buffer aggregates complete frames before transmission. The terminal
condition fires on buffer pressure or explicit stop; the implementation does not
silently drop samples.

## Web UI Capture

The Web UI creates a sigrok live session and communicates exclusively over the
WebSocket binary protocol. The workflow:

1. Create live session via `/api/v1/live-sessions`
2. Connect to returned `/api/v1/ws/<slot>` WebSocket URL
3. Speak sigrok binary protocol over the WebSocket

The logic analyzer is available in the Web UI under the **Terminal workspace**,
alongside the serial terminal. Captured samples can be decoded in the browser using
the project-owned Rust/WASM logic decoder: the stable decoder URLs are
`/assets/decoder/logic-decoder.js` and `/assets/decoder/logic-decoder_bg.wasm`,
served with `application/wasm` MIME for the WASM asset and gzip-compressed.

The decoder supports UART, I2C, and SPI protocols only; it is not a
libsigrokdecode Python plugin compatibility layer.

Completed captures can be previewed in the waveform view and exported as CSV or
PulseView `.sr` files. The browser decoder operates on completed bounded captures
only, not on stream data.

## Native Sigrok Integration

Native sigrok-cli and PulseView connect via raw-TCP on port 5556:

```sh
sigrok-cli -d linkr-debugger:conn=tcp-raw/172.29.203.1/5556 --scan
```

The protocol is the sigrok binary protocol (see `doc/sigrok-linkr-v1.md`).
This is separate from the Web UI WebSocket path.

## File Reference

- `apps/radxa_linkr_debugger/src/linkr_debugger_sigrok_linkr.h`
- `apps/radxa_linkr_debugger/src/linkr_debugger_sigrok_linkr.c`
- `web/src/components/LogicAnalyzerCard.tsx`

## Resources

- [PulseView](https://sigrok.org/wiki/PulseView)
- [Sigrok file format](https://sigrok.org/wiki/File_format:Sigrok/v2)
- [RP2350 datasheet](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf)
