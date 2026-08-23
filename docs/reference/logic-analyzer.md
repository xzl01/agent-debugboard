# Logic Analyzer

## Architecture Overview

The firmware logic analyzer uses RP2350 PIO2+DMA for high-speed GPIO capture.
It is not a polling-based diagnostic tool and does not support sustained streaming
at high rates.

### Control Paths

The logic analyzer has two distinct control paths:

**Web UI**: Creates a live session through `/api/v1/live-sessions` and speaks
the sigrok binary protocol on the returned `/api/v1/ws/<slot>` WebSocket URL.
Bounded pre-trigger is available for `rising`, `falling`, and `either` under the
contract in the Capture Parameters section.

**Native sigrok/PulseView**: Connects directly via raw-TCP on port 5556.
Use `sigrok-cli -d linkr-debugger:conn=tcp-raw/172.29.203.1/5556` or the
PulseView GUI with the same connection string.

### Supported Modes

| Mode | Pins | Description |
|------|------|-------------|
| FAST8 | GP10-GP17 | 8-channel capture |
| WIDE11 | GP10-GP20 | 11-channel capture; GP29 excluded from LA (ADC3 voltage monitor; ADC3-owned and input-only) |

GP7-GP9 and GP29 are not available in Sigrok modes. The Web UI sigrok pin
selection is limited to GP10-GP20; GP29 stays in the persisted/safe catalog but is ADC3-owned and input-only on this firmware (ADC3 voltage monitor).

### Capture Parameters

**CONFIG v1 pre/post are uint16.** Ordinary bounded captures use 1..65535 and
stream uses post=0. The negotiated CONFIG_V2 uses u32LE pre/post for the supported
configurations documented below.

**HELLO capability flags**:
- **bit 0 (CONFIG_V2)**: Advertises CONFIG_V2_REQ support for captures with post > 65535
- **bit 1 (GENERIC_PACKED_BURST)**: Advertises unified generic packed-burst architecture. When set, post=0 at supported high rates captures exactly 100000 samples losslessly, then auto-STOP/drain (not indefinite streaming)

With CONFIG_V2 and GENERIC_PACKED_BURST, bounded `pre=0`,
`post=65536..100000` uses the common packed pipeline at every otherwise
supported rate and pin plan. WIDE11 at requested 125 MHz remains invalid.

**High-rate `post=0` capacity-burst matrix**:

| Mode | Rate | pre | post | Notes |
|------|------|-----|------|-------|
| SINGLE | 100 MHz or 125 MHz | 0 | 0 | Captures exactly 100000 samples losslessly then auto-STOP/drain |
| FAST8 | 100 MHz or 125 MHz | 0 | 0 | Captures exactly 100000 samples losslessly then auto-STOP/drain |
| WIDE11 | 100 MHz only | 0 | 0 | Captures exactly 100000 samples losslessly then auto-STOP/drain. 125 MHz rejected by START (INVALID_CONFIG) |

Bounded **Arm** captures use requested sample rates from 100,000 through
125,000,000 Hz (100kHz-125MHz) and post-trigger sample counts from 1 through
65535. Web UI bounded pre-trigger supports rising, falling, and either only:
`pre_samples >= 1`, `post_samples >= 1`, and `pre_samples + post_samples <= 512`.
Requested rates are 1-25 MHz, and the selected physical plan must retain at
least `2 * ceil(actual_rate / 1000)` samples. Current Web discrete rates are:
SINGLE through 25 MHz, FAST8 through 10 MHz and not 25 MHz, WIDE11 through 5 MHz
and not 10 or 25 MHz. The UI may edit locally before its first connection when
generic constraints pass, then uses real per-mode CAPS and rejects or disables
old firmware or a mode without CAPS mode flag bit 5 (`PRE_TRIGGER`, `1 << 5`).
HELLO server flags bit 0 (`CONFIG_V2`) and bit 1 (`GENERIC_PACKED_BURST`) are
separate. Stream, trigger NONE, unsupported or high-rate generic packed burst,
and ordinary deep capture remain pre=0. Stream sends both pre and post as zero.

Completion is `pre_samples + post_samples`, and `triggerIndex` equals pre. The
firmware reuses the prepared common packed ring/sink lifecycle, treats packed
samples after prefill as the sole trigger authority, scans edges in software,
and freezes and drains the exact `[T-pre,T+post)` window. No invented IRQ pairing
or new buffer is used. Existing deep post behavior remains when pre=0. See the
[2026-07-28 HIL report](../testing/results/2026-07-28-logic-analyzer-pre-trigger-uart-hil.md).

### Hardware Buffer

The capture engine uses two engines: packed finite DMA and packed DMA ring.
SINGLE/FAST8 uses one 32768B packed wrap ring; WIDE11 uses lane A 16384B plus lane B 8192B
inside the same 32768B slice, with a common capacity of 16384 samples and a safety margin of 2048.
Usable capacities are SINGLE 260096, FAST8 30720, and WIDE11 14336 samples. The WIDE11 writer
enforces minimum lane sequence and stops when greater than 20 sample skew is detected.
DATA metadata is 8 bytes. Sample indices are modulo 24 bits; wrap is not a terminal condition.

On overrun, possible overrun, transport backpressure, or bounded-capture completion,
the firmware emits a terminal event and stops rather than silently continuing (lossless-or-stop).
The packed ring reuses the 149048 B total backing allocation (sized to max(normal, burst)=149048 B); the WIDE11 144184 B hardware slice and the 30720 B WS telemetry ring share that allocation.

Arena pause/resume is a lossless, generation-scoped handshake. The ADC sampler
atomically consumes preposted CONFIG/RESUME events, and an old generation's
RESUME cannot satisfy a newer pause request. Arena ownership also remains held
until the resume callback completes. Clients must not need an artificial delay
between a completed capture and an immediate restart.

#### Terminal selection freezes the writer (freeze-before-drain)

When the firmware selects a terminal condition while a bounded or high-rate capture
is still active (OVERRUN, possible overrun, transport backpressure, bounded-capture
completion, or any other non-OK terminal cause), it freezes the writer before draining
the ring so the consumer can never read samples that the producer is still allowed to
overwrite. Concretely:

1. Disable the trigger SM (existing).
2. Disable sampler SM A; drain its RX FIFO to a clean state. In dual-lane modes
   (WIDE11), disable sampler SM B and drain its RX FIFO as well.
3. Abort ring DMA channel A (single-lane) or channels A and B (dual-lane):
   disable the DMA IRQ, clear the EN bit, call `dma_channel_abort`, then clear the
   DMA IRQ. Channel ownership is **retained** — neither DMA channel is released
   back to the pool, and `la_packed_burst_release_dma_locked()` is **not** called.
4. Drain only the already-committed ring data through the normal consumer path.

This is idempotent: the freeze guard is checked first so a second terminal that
arrives while the writer is already frozen is a no-op. The dual-lane helper
`linkr_debugger_logic_analyzer_ring_freeze_policy(lane_count)` is a pure policy
function that returns the bitmask of hardware to stop, so it is independently
testable in host unit tests against `test_ring_freeze_policy_stops_required_stream_hardware()`.

The frozen writer prevents the failure mode where overrun or error servicing kept
hardware alive long enough for it to overwrite ring samples that the consumer was
still draining. Sample indices remain contiguous in this regime, so a future sample-
index-gap check cannot by itself detect the corruption — the freeze-before-drain
contract is what eliminates the hidden corruption, not the gap check.

#### Lossless-or-stop capacity outcomes

The matrix `pass` rule has three accepted shapes:

1. **Exact bounded/server completion**: the consumer received the expected exact
   sample count (`post_samples`, or the negotiated 100000-sample high-rate cap)
   with zero gaps and decode errors, followed by server EVENT STOPPED or a
   successful client STOP_REQ/STOP_RESP cleanup.
2. **Client duration completion**: a continuous run reached its requested
   duration cleanly and the client received STOP_RESP for its STOP_REQ.
3. **Explicit lossless capacity stop**: a continuous capture received server
   EVENT OVERRUN with zero gaps, decode errors, or disconnects and passed the
   restart and HTTP-health checks. The delivered DATA count may be zero or
   non-zero; a client STOP_REQ/STOP_RESP is not required.

A zero-DATA capacity stop (no DATA frames, only a STOPPED/OVERRUN event) is **not**
labeled a passing sustained-throughput claim. It is labeled `capacity_stop_before_data`
and is treated as a capacity-stop diagnostic, not a continuous-streaming pass: it
proves the firmware stopped cleanly before the consumer saw any data, which is what
the lossless-or-stop contract requires, but it does not show a measured throughput
envelope. Tests or matrix cells that observe an immediate `sample_index=0` OVERRUN
with `data_frames=0` must label the row this way.

### Measured No-Gap Continuous Ceilings

On the representative HIL setup:

| Scenario | Rate | Result |
|----------|------|--------|
| WS bounded SINGLE 100 kHz, post=65535 | 100 kHz | Exactly 65535 samples, 0 gaps, restart true, HTTP health true |
| WS bounded FAST8 100 kHz, post=65535 | 100 kHz | Exactly 65535 samples, 0 gaps, restart true, HTTP health true |
| WS bounded WIDE11 100 kHz, post=65535 | 100 kHz | Exactly 65535 samples, 0 gaps, restart true, HTTP health true |
| TCP bounded SINGLE 100 kHz, post=65535 | 100 kHz | Exactly 65535 samples, 0 gaps, restart true, HTTP health true |
| TCP bounded FAST8 100 kHz, post=65535 | 100 kHz | Exactly 65535 samples, 0 gaps, restart true, HTTP health true |
| TCP bounded WIDE11 100 kHz, post=65535 | 100 kHz | Exactly 65535 samples, 0 gaps, restart true, HTTP health true |
| WS continuous SINGLE | 1 MHz | 10 consecutive 5-second runs, ~4.991M-4.997M samples each, 998.16-998.70 ksps effective, zero sample-index gaps, zero disconnects, STOP response, immediate restart and HTTP health |
| WS continuous FAST8 | 240 kHz | 5-second no-gap ceiling |
| WS continuous FAST8 | 241 kHz | Adjacent failure, explicit terminal, restart and HTTP health retained |
| WS continuous WIDE11 | 149 kHz | 5-second no-gap ceiling |
| WS continuous WIDE11 | 150 kHz | Adjacent failure, explicit terminal, restart and HTTP health retained |
| TCP continuous SINGLE | 443 kHz | 5-second no-gap ceiling |
| TCP continuous SINGLE | 444 kHz | Adjacent failure, explicit terminal, restart and HTTP health retained |
| TCP continuous FAST8 | 241 kHz | 5-second no-gap ceiling |
| TCP continuous FAST8 | 242 kHz | Adjacent failure, explicit terminal, restart and HTTP health retained |
| TCP continuous WIDE11 | 147 kHz | 5-second no-gap ceiling |
| TCP continuous WIDE11 | 148 kHz | Adjacent failure, explicit terminal, restart and HTTP health retained |
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

### Generic Packed Burst (CONFIG_V2)

The generic packed-burst architecture uses a common packed arena for deep captures.
SINGLE and FAST8 use one capture SM; WIDE11 uses two capture SMs.
HELLO server_flags bit 0 advertises CONFIG_V2 and bit 1 advertises GENERIC_PACKED_BURST;
clients must use frame0x0b with 16-byte payload (u32LE pre/post) only for
bounded post > 65535. The v1 frame0x05 (12B) remains for bounded captures with
post <= 65535 and the post=0 stream sentinel; bit1 determines whether supported
high-rate post=0 captures exactly 100000 samples and then auto-STOP/drains.
Bounded `post=65536..100000` is rate-neutral within the otherwise supported
physical plan limits when both capabilities are negotiated.

The three physical capture plans:
- **SINGLE**: one 1-bit lane on FAST8 SM (GP10 default), autopush32, 32 samples per 32-bit word,
  12500 B source at 100 MHz (100000 samples × 1 bit). A FAST8 physical plan, not a separate Sigrok wire mode.
- **FAST8**: one 8-bit lane (GP10-GP17), autopush32, 4 samples per 32-bit word,
  100000 B source at 100 MHz (100000 samples × 8 bits)
- **WIDE11**: two capture SMs: SM-A (GP10-GP17, 8-bit autopush32, 100000 B) and SM-B (GP18-GP20, 3-bit autopush30, 40000 B); two DMA channels; 144184 B burst slice (overlays the 149048 B total backing allocation); triggered deep burst adds a third trigger-only SM

GP29 is excluded from WIDE11 LA (ADC3 voltage monitor; ADC3-owned and input-only). WIDE11 verification is
covered by the historical 2026-07-27 freeze-final HIL matrices below.

**Historical WIDE12 baseline (not current WIDE11; retained for context only)**:
SM-A (GP10-GP20, 11-bit autopush22, 200000 B) plus SM-B (GP29, 1-bit autopush32,
12500 B); two DMA channels, 212500 B total source; 216684 B shared arena.
See `docs/testing/results/2026-07-26-logic-analyzer-wide12-100k-hil.md` for the
WIDE12 historical evidence.

#### Historical 2026-07-27 WIDE11 freeze-final HIL evidence

The freeze-final HIL matrices are historical `pre=0` evidence for the WIDE11
deep-burst path on the representative HIL setup. They are not evidence for the
2026-07-28 pre-trigger implementation:

- **Authoritative ring matrix** (`logic-analyzer-wide11-packed-all-freeze-final.json`):
  **54/54 cases pass**, `overall_pass=true`: 36 bounded NONE cases across
  SINGLE/FAST8/WIDE11, 1/5/25 MHz, post=1024/4096, and TCP/WS, plus 18
  five-second continuous NONE cases across the same mode/rate/transport combinations.
- **High-rate matrix** (`logic-analyzer-high-rate-packed-burst-freeze-final.json`):
  **62/62 cases pass**, `overall_pass=true`. WS SINGLE/FAST8/WIDE11 bounded deep
  bursts plus stream sentinel cases across 100 MHz (WIDE11 native) and the quantized
  125 MHz (125081 kHz actual) rate for SINGLE/FAST8, with strict zero-gap checks.
- **WIDE11 exact 100000/98 DATA / zero gaps** (`logic-analyzer-high-rate-packed-burst-freeze-final.json`,
  WIDE11 rows at `requested_samplerate_khz=100000`, `post_samples=100000`):
  `received_sample_count=100000`, `data_frames=98`, `sample_index_gaps=0`,
  `disconnects=0`, `overrun_events=0`, `error_events=0`, `payload_over_budget_frames=0`,
  `data_decode_error_frames=0`, `invalid_sample_count_frames=0`, `stopped_events=1`,
  `last_sample_index=99999`, and the terminal is `server_stopped`; this server
  auto-completion does not send a client STOP_REQ or receive STOP_RESP. Each successful WIDE11 deep burst in the high-rate matrix drives 98
  DATA frames (100000 samples × 11 bits / 8 ≈ 1024 samples per frame at the project
  ceiling of 1024 samples per DATA frame).
- **Mapping** (`logic-analyzer-wide11-mapping-freeze-final-sequential.json`): the WIDE11
  mapping HIL drives GP10 with a UART stimulus and holds GP11–GP20 low. The full
  pass was `tcp` WIDE11 at `channel_mask=0x07FF` (11 bits), 100 MHz, post=100000,
  `received_sample_count=100000`, `data_frames=98`, `sample_index_gaps=0`,
  `disconnects=0`, `overrun_events=0`, `error_events=0`, with a clean STOP_RESP
  and post-capture board-health recovery. This is the only physical-stimulus mapping
  pattern covered by the freeze-final HIL; **it does not demonstrate high-state
  mapping on GP18–GP20 or simultaneous dual-lane transitions**. See
  `docs/testing/hil-functional-test-spec.md` and the SKILL before claiming full
  WIDE11 high-state mapping.
- **Telemetry isolation** (`logic-analyzer-wide11-telemetry-isolation-freeze-final-sequential.json`):
  pass. The arena lease fully quiesces the board's ADC HTTP/telemetry
  (`baseline.connected=true`, `overlap.expected_pause_observed=true`,
  `pre_pause_delivery_grace_records` non-empty during the lease window,
  `pause_record_count=0`, `post_pause_delivery_grace_records` resumes cleanly),
  and the post-lease HTTP `/api/v1/status` plus `/api/v1/adc/read` health checks
  pass within the freeze-final envelope.

### Bandwidth Guard During Capture

Every sigrok capture mode (bounded, stream, and packed burst, over both the
WebSocket and raw-TCP transports) holds the capture arbiter for the whole
capture lifetime. While the arbiter is owned by the logic analyzer, the ADC
telemetry sampler stops producing samples, so no telemetry frames compete with
logic samples for USB-NCM bandwidth; the sampler resumes automatically as soon
as the capture releases the arbiter, so no explicit resume command exists.
This applies in addition to the arena-level WIDE11 quiesce: the arena quiesce
protects the shared ring memory, while the bandwidth guard frees the link for
all capture modes.

At the requested 125 MHz rate the actual rate returned in the CONFIG_RESP ACK is
the RP2350 quantized 125081 kHz even when the caller asked for 125000 kHz. The
freeze-final HIL still accepts that quantized actual rate for SINGLE/FAST8 and
treats a quantized 125 MHz ACK at WIDE11 as a matrix-correct START rejection
(INVALID_CONFIG, error code 7). The matrix-eligibility rule uses the **requested**
rate, not the quantized actual: WIDE11 is capped at requested 100 MHz, while
SINGLE/FAST8 stay at requested up to 125 MHz.

Historical WIDE11 generic packed-burst HIL report:
`docs/testing/results/2026-07-27-logic-analyzer-generic-packed-burst-hil.md`.

### Bounded Capture Engine (pre-trigger and post-trigger)

Bounded Web captures with pre-trigger use the exact contract described above.
The existing prepared packed ring and sink are prefetched before trigger authority
moves to packed samples. Software scans the selected edge, then the firmware
freezes and drains `[T-pre,T+post)`, so the reported trigger index equals `pre`.
The old pre=0 finite path remains for NONE and other paths that cannot negotiate
bounded pre-trigger. Post>512 bounded uses packed ring streaming.


### Architecture

The LA backend uses two capture engines: packed finite DMA and packed DMA ring.
SINGLE/FAST8 uses one 32768B packed wrap ring; WIDE11 uses lane A 16384B plus lane B 8192B
inside the same 32768B slice, with a common capacity of 16384 samples and a safety margin of 2048.
Usable capacities are SINGLE 260096, FAST8 30720, and WIDE11 14336 samples. The WIDE11 writer
enforces minimum lane sequence and stops when greater than 20 sample skew is detected. A
protocol-neutral LA sink feeds consumer work items through a pool of 8 DATA slots
plus 1 terminal slot. Each WS SINGLE DATA slot carries up to 2048 packed samples;
the sink consumer runs at priority 7 and blocks naturally on full chunks. Legacy
callback paths, TCP transport, and non-SINGLE sample paths run at priority 8 with
yield. The sender applies one-byte RLE with BIT_PACK fallback. A 6144-byte
coalescing buffer aggregates complete frames before transmission. The terminal
condition fires on buffer pressure or explicit stop; the implementation does not
silently drop samples (lossless-or-stop). When the writer is frozen on a terminal
selection, the freeze policy (described under "Terminal selection freezes the
writer" above) disables the trigger SM and the sampler SM(s) and aborts the ring
DMA channel(s) without releasing the channels back to the pool, so the consumer
can only drain already-committed ring data. The packed ring reuses the 149048 B total backing
allocation (sized to max(normal, burst)=149048 B); the WIDE11 144184 B hardware slice and the
30720 B WS telemetry ring share that backing allocation.

The generic packed burst path uses a dual-SM packed arena: SM-A captures
GP10-GP17 (8-bit autopush32, 100000 B source) and SM-B captures GP18-GP20 (3-bit
autopush30, 40000 B source), with 140000 B total from two DMA channels and a
144184 B burst slice (overlays the 149048 B total backing allocation). This is the WIDE11 architecture; GP29 is excluded from
WIDE11 LA. NONE deep burst uses two capture SMs; triggered deep burst adds a third SM
running the 3-instruction trigger program. The three physical plans: SINGLE (one 1-bit lane, autopush32,
32 samples/word, 12500 B at 100 MHz), FAST8 (one 8-bit lane, autopush32, 4 samples/word,
100000 B at 100 MHz), and WIDE11 (two capture SMs: SM-A GP10-GP17 8-bit autopush32
100000 B + SM-B GP18-GP20 3-bit autopush30 40000 B).

### Final freeze-final footprint

The freeze-final build that produced the 54/54 + 62/62 + WIDE11 100000/98 + mapping
+ telemetry isolation evidence above was canonical sysbuild at:

- Application FLASH: 701900 bytes (82.79% of 847832)
- Application RAM: 475896 bytes (89.37% of 532480)
- Combined UF2 (MCUboot + application): 1455616 bytes

Only the combined UF2 is flashed for initial install or recovery; the application
binary alone is reserved for OTA updates through MCUboot. These current sizes
are from the 2026-07-28 pre-trigger HIL build. Earlier freeze-build sizes remain
historical evidence in their dated reports.

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

The protocol is the sigrok binary protocol (see `docs/reference/sigrok-linkr-v1.md`).
This is separate from the Web UI WebSocket path.

## File Reference

- `apps/radxa_linkr_debugger/src/linkr_debugger_sigrok_linkr.h`
- `apps/radxa_linkr_debugger/src/linkr_debugger_sigrok_linkr.c`
- `web/src/components/LogicAnalyzerCard.tsx`

## Resources

- [PulseView](https://sigrok.org/wiki/PulseView)
- [Sigrok file format](https://sigrok.org/wiki/File_format:Sigrok/v2)
- [RP2350 datasheet](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf)
