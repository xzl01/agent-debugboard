# Linkr Sigrok Protocol v1

Binary framed protocol over TCP port 5556 for continuous logic analyzer streaming with hardware trigger and compression. The Web UI uses this same protocol over a WebSocket transport after creating a live session via `/api/v1/live-sessions`.

On the WebSocket transport, one WebSocket binary message may contain multiple
complete Sigrok frames concatenated in FIFO order. Clients must parse the
WebSocket payload as a byte stream, emit every complete inner Sigrok frame in
order, and keep only an incomplete trailing Sigrok frame for the next receive
call. This transport coalescing does not change the inner Sigrok frame header,
payload format, frame order, or request/response semantics.

## Wire Format

All multi-byte fields are little-endian.

### Header (9 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | magic | 0x72 ('r') |
| 1 | 1 | version | Protocol version (1) |
| 2 | 1 | type | Message type |
| 3 | 4 | id | Request/Sequence ID |
| 7 | 2 | payload_len | Payload length in bytes |

**ID semantics**:
- Request-Response: Client assigns ID, server echoes back
- Data/Event: Server assigns monotonically increasing sequence

### Message Types

| Type | Name | Direction | Payload |
|------|------|-----------|---------|
| 0x01 | HELLO_REQ | C→S | None |
| 0x02 | HELLO_RESP | S→C | Hello (5B) |
| 0x03 | CAPS_REQ | C→S | None |
| 0x04 | CAPS_RESP | S→C | Caps (1 + N×8B) |
| 0x05 | CONFIG_REQ | C→S | Config v1 (12B) — for post <= 65535 and post=0 stream sentinel |
| 0x06 | CONFIG_RESP | S→C | Ack (6B) |
| 0x07 | START_REQ | C→S | None |
| 0x08 | START_RESP | S→C | Ack (6B) — emitted only after capture ownership is acquired and the backend is successfully prepared; receiving this frame is the trigger-safe barrier. Ordinary finite/ring paths are already armed or running. For WIDE11 deep burst, NONE sends state 3 with no ARMED event, while triggered captures send state 2 followed by the ARMED event; GO then synchronously enables the sampler SM(s). A failed start returns a synchronous FRAME_ERROR and no false ARMED or RUNNING event. |
| 0x09 | STOP_REQ | C→S | None |
| 0x0a | STOP_RESP | S→C | Ack (6B) |
| 0x0b | CONFIG_V2_REQ | C→S | Config v2 (16B) — for large-depth captures with post > 65535 |
| 0x10 | EVENT | S→C | Event (6B) |
| 0x11 | DATA | S→C | Meta (8B) + Samples |
| 0x7f | ERROR | S→C | Error (3B) |

## Payload Formats

### HELLO_RESP (5 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | protocol_version | Protocol version (1) |
| 1 | 1 | server_flags | Capability flags (see below) |
| 2 | 1 | mode_count | Number of modes |
| 3 | 2 | max_payload_len | Max payload size |

**server_flags** advertises protocol capabilities:

| Bit | Flag | Description |
|-----|------|-------------|
| 0 | CONFIG_V2 | Supports CONFIG_V2_REQ frame0x0b for captures with post > 65535. When set, the client may use frame0x0b (16B, u32LE pre/post) for large-depth captures. When clear, only frame0x05 (CONFIG_REQ v1, 12B) is available for bounded captures with post <= 65535 and the post=0 stream sentinel. |
| 1 | GENERIC_PACKED_BURST | Advertises the unified generic packed-burst architecture. When set, post=0 at supported high rates captures exactly 100000 samples losslessly then auto-STOP/drain, not indefinite streaming. The client should check this flag to determine the post=0 behavior contract. |

Without GENERIC_PACKED_BURST, the post=0 behavior is implementation-defined. With
GENERIC_PACKED_BURST, the post=0 contract at supported high rates is: capture
exactly 100000 samples losslessly, then normal auto-STOP/drain.

The current maximum payload is 4104 bytes: 8 bytes of DATA metadata plus up to
4096 bytes of compressed sample data. Current firmware chunks contain at most
1024 points, so WIDE11 bit-pack data is at most 2048 bytes and remains within
that bound.

### CAPS_RESP (1 + N×8 bytes)

**Header (1 byte)**:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | mode_count | Number of modes |

**Mode Caps (8 bytes each)**:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | mode_id | Mode identifier |
| 1 | 1 | mode_flags | Mode capabilities |
| 2 | 1 | channel_count | Max channels |
| 3 | 1 | sample_bytes | Max bytes per sample |
| 4 | 3 | max_samplerate_khz | Max sample rate (kHz, little-endian, 125000 = 125 MHz) |
| 7 | 1 | compression | Supported compression |

**Mode Flags**:
| Bit | Flag | Description |
|-----|------|-------------|
| 0 | CONTINUOUS | Supports continuous streaming |
| 1 | TRIGGER_NONE | Supports no-trigger mode |
| 2 | TRIGGER_RISING | Supports rising edge trigger |
| 3 | TRIGGER_FALLING | Supports falling edge trigger |
| 4 | TRIGGER_EITHER | Supports either edge trigger |

Note: PRE_TRIGGER flag exists in the protocol but is not exposed in the Web UI.
The Web UI always sends `pre_samples=0`.

**Compression Flags**:
| Bit | Flag | Description |
|-----|------|-------------|
| 0 | BIT_PACK | Supports dynamic bit packing |
| 1 | RLE | Supports run-length encoding |

### CONFIG_REQ (12 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | mode_id | Selected mode |
| 1 | 1 | trigger_type | 0=none, 1=rising, 2=falling, 3=either |
| 2 | 1 | trigger_channel | Trigger channel index (0-based in channel_mask) |
| 3 | 2 | channel_mask | Enabled channels (bitmask) |
| 5 | 3 | samplerate_khz | Requested rate (kHz, little-endian) |
| 8 | 2 | pre_samples | Pre-trigger depth (0; non-zero is rejected in Web UI) |
| 10 | 2 | post_samples | Post-trigger depth (0=unlimited/stream) |

**pre_samples and post_samples are uint16.** The Web UI always sends
`pre_samples=0`. Bounded captures use `post_samples=1..65535`; stream mode uses
`post_samples=0` as the unlimited sentinel.

**Dynamic Channel Compression**:
- `channel_mask` selects which channels to capture
- Actual bytes per sample = `ceil(popcount(channel_mask) / 8)`
- Example: 3 channels → 1 byte, 12 channels → 2 bytes

**Trigger Channel**:
- `trigger_channel` is 0-based index within `channel_mask`
- Example: `channel_mask=0b00000101` (channels 0 and 2), `trigger_channel=1` triggers on channel 2

**Rate Negotiation**:
- Client sends `samplerate_khz` in CONFIG_REQ
- Server responds with actual rate in CONFIG_RESP
- Actual rate may differ due to clock divider constraints
- PIO only supports integer clock dividers: actual_rate = 125 MHz / clock_div

### CONFIG_V2_REQ (16 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | mode_id | Selected mode |
| 1 | 1 | trigger_type | 0=none, 1=rising, 2=falling, 3=either |
| 2 | 1 | trigger_channel | Trigger channel index (0-based in channel_mask) |
| 3 | 2 | channel_mask | Enabled channels (bitmask) |
| 5 | 3 | samplerate_khz | Requested rate (kHz, little-endian) |
| 8 | 4 | pre_samples | Pre-trigger depth (u32LE, must be 0) |
| 12 | 4 | post_samples | Post-trigger depth (u32LE) |

**Use CONFIG_V2_REQ only when a bounded capture's post_samples exceeds 65535.**
The v1 CONFIG_REQ post=0 stream sentinel remains valid at all rates. At supported
high rates, HELLO server_flags bit 1 (GENERIC_PACKED_BURST), not CONFIG_V2
encoding, determines whether that post=0 sentinel captures exactly 100000 samples
and then auto-STOP/drains.

With CONFIG_V2 and GENERIC_PACKED_BURST, bounded `pre=0`,
`post=65536..100000` uses the common packed pipeline at every otherwise
supported rate and pin plan. WIDE11 at requested 125 MHz remains invalid.

**High-rate `post=0` capacity-burst matrix**:

| Mode | Rate | pre | post | Notes |
|------|------|-----|------|-------|
| SINGLE | 100 MHz or 125 MHz | 0 | 0 | Captures exactly 100000 samples losslessly then auto-STOP/drain |
| FAST8 | 100 MHz or 125 MHz | 0 | 0 | Captures exactly 100000 samples losslessly then auto-STOP/drain |
| WIDE11 | 100 MHz only | 0 | 0 | Captures exactly 100000 samples losslessly then auto-STOP/drain. 125 MHz rejected by START (INVALID_CONFIG) |

The v1 CONFIG_REQ (frame0x05, 12B) remains the correct frame for bounded
captures with post <= 65535 and for the post=0 stream sentinel on configurations
not covered by the GENERIC_PACKED_BURST contract. Configurations not in the
matrix above receive CONFIG_RESP but START returns INVALID_CONFIG.

### ACK (6 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 2 | session_id | Session identifier |
| 2 | 1 | state | Current state (4 bits) |
| 3 | 3 | actual_rate_khz | Actual sample rate (kHz, little-endian) |

**State Values**:
| Value | State | Description |
|-------|-------|-------------|
| 0 | IDLE | Not configured |
| 1 | CONFIGURED | Ready to start |
| 2 | ARMED | Waiting for trigger |
| 3 | RUNNING | Capturing |
| 4 | STOPPED | Capture complete |

**State progression**: After START_RESP is received, the session transitions through an ordered sequence: START_RESP (state 2 or 3) then EVENT armed (state 2) then EVENT triggered (state 3) then DATA frames then EVENT stopped (state 4). EITHER trigger first snapshots the pin level in firmware before arming the hardware trigger, so there is an arm-time race window. NONE trigger is ungated immediate and does not emit an ARMED EVENT; it starts directly in RUNNING state.

### EVENT (6 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 2 | session_id | Session identifier |
| 2 | 1 | type_detail | Event type (4 bits) + detail (4 bits) |
| 3 | 3 | sample_index | Sample at event |

**Event Types**:
| Value | Type | Description |
|-------|------|-------------|
| 1 | ARMED | Waiting for trigger |
| 2 | TRIGGERED | Trigger detected |
| 3 | RUNNING | Capturing (no trigger) |
| 4 | STOPPED | Capture stopped |
| 5 | OVERRUN | Ring buffer overrun |
| 6 | ERROR | Error occurred |

### DATA (8 + N bytes)

**Meta (8 bytes)**:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 3 | sample_index | First sample index (modulo 24 bits) |
| 3 | 2 | sample_count | Samples in chunk |
| 5 | 1 | compression | Compression type |
| 6 | 2 | channel_mask | Active channel mask |

Sample indices wrap modulo 24 bits; wrap is not a terminal condition.

**Compression Types**:
| Value | Type | Format |
|-------|------|--------|
| 0 | RAW | Uncompressed bytes |
| 1 | BIT_PACK | Dynamic bit packing |
| 2 | RLE | Run-length encoding |
| 3 | BIT_PACK_RLE | Bit packing + RLE |

**Sample Formats**:

**RAW (compression=0)**:
```
[sample_0][sample_1]...[sample_N-1]
Each sample: ceil(popcount(channel_mask)/8) bytes
```

**BIT_PACK (compression=1)**:
```
[packed_byte_0][packed_byte_1]...
Bits packed LSB-first, channel order follows channel_mask
```

**RLE (compression=2)**:
```
[value_0][count_0][value_1][count_1]...
value: ceil(popcount(channel_mask)/8) bytes
count: 2 bytes (little-endian)
```

**BIT_PACK_RLE (compression=3)**:
```
[packed_value_0][count_0][packed_value_1][count_1]...
packed_value: ceil(popcount(channel_mask)/8) bytes
count: 2 bytes (little-endian)
```

### ERROR (3 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | error_code | Error code (4 bits) |
| 1 | 2 | detail | Error detail |

**Error Codes**:
| Code | Name | Description |
|------|------|-------------|
| 1 | INVALID_TYPE | Unknown message type |
| 2 | INVALID_LENGTH | Invalid payload length |
| 3 | UNSUPPORTED_VERSION | Version mismatch |
| 4 | OVERSIZE_PAYLOAD | Payload too large |
| 5 | INTERNAL | Internal error |
| 6 | INVALID_STATE | Invalid state for operation |
| 7 | INVALID_CONFIG | Invalid configuration |
| 8 | BUSY | Hardware busy |

## Acquisition Modes

### Mode 1: Fast8 (mode_id=1)

- **Channels**: GP10-GP17 (8 channels)
- **Pin base**: 10
- **Pin mask**: 0xFF
- **Sample format**: Dynamic (1-8 bits based on selected channels)
- **Compression**: BIT_PACK, RLE

### Mode 2: Wide11 (mode_id=2)

- **Channels**: GP10-GP20 (11 channels; GP29 excluded from LA)
- **Pin base**: 10
- **Pin mask**: 0x07FF
- **Sample format**: Dynamic (1-16 bits based on selected channels)
- **Compression**: BIT_PACK, RLE

### Generic Packed Burst Architecture

The generic packed-burst architecture uses a common packed arena for deep captures.
SINGLE and FAST8 use one capture SM; WIDE11 uses two capture SMs. For WIDE11:

- **SM-A**: captures GP10-GP17 (8 channels), 8-bit autopush32, 100000 B source buffer
- **SM-B**: captures GP18-GP20 (3 channels), 3-bit autopush30, 40000 B source buffer
- **Two DMA channels**: 140000 B total source; no network or flash activity during the
  ~1 ms acquisition window. A triggered WIDE11 deep burst adds a third,
  trigger-only SM running the 3-instruction trigger program (peak 3 SMs)
- **Post-capture transport**: up to 98 DATA frames, maximum 1024 samples per frame,
  140000 B total payload
- **Shared arena**: 144184 bytes; quiesces ADC telemetry, power capture, and normal Sigrok
  pool for the lease lifetime; restores after drain
- **Two-phase START**: ownership and quiesce are ready before the response. NONE
  sends START_RESP in RUNNING state with no ARMED event; triggered captures send
  START_RESP in ARMED state followed by the ARMED event. GO then synchronously
  enables the capture SMs

GP29 is excluded from WIDE11 LA (available as ordinary GPIO/ADC3).

The three physical capture plans:
- **SINGLE**: one 1-bit lane on FAST8 SM (GP10 default), autopush32, 32 samples per 32-bit word,
  12500 B source at 100 MHz (100000 samples × 1 bit). A FAST8 physical plan, not a separate Sigrok wire mode.
- **FAST8**: one 8-bit lane (GP10-GP17), autopush32, 4 samples per 32-bit word,
  100000 B source at 100 MHz (100000 samples × 8 bits)
- **WIDE11**: two capture SMs: SM-A (GP10-GP17, 8-bit autopush32, 100000 B) and SM-B (GP18-GP20, 3-bit autopush30, 40000 B); two DMA channels; 144184 B shared arena

WIDE11 verification is the freeze-final HIL evidence block below; it is not the
same artifact as the historical WIDE12 baseline.

**Historical WIDE12 baseline (not current WIDE11; retained for context only)**:
SM-A (GP10-GP20, 11-bit autopush22, 200000 B) plus SM-B (GP29, 1-bit autopush32,
12500 B); two DMA channels, 212500 B total source; 216684 B shared arena.
See `doc/testing/results/2026-07-26-logic-analyzer-wide12-100k-hil.md` for the
WIDE12 historical evidence.

HELLO server_flags bit 0 advertises CONFIG_V2 capability and bit 1 advertises
GENERIC_PACKED_BURST. The client must use frame0x0b with 16-byte payload (u32LE
pre/post fields) only for bounded post > 65535. The v1 frame0x05 (12B) remains
for bounded captures with post <= 65535 and for the post=0 stream sentinel;
bit1 determines whether supported high-rate post=0 captures exactly 100000
samples and then auto-STOP/drains. Bounded `post=65536..100000` is rate-neutral
within the otherwise supported physical plan limits when both capabilities are
negotiated.

#### Terminal-Freeze Policy (freeze-before-drain)

The packed ring path serves both lossless-or-stop and freeze-before-drain. When
the firmware picks a non-OK terminal cause while the writer is still active
(OVERRUN, possible overrun, transport backpressure, ERROR, or any other writer-
side terminal), the writer is frozen **before** the consumer drains the ring so
the consumer can never read samples the producer is still allowed to overwrite.
The sequence is:

1. Disable the trigger SM.
2. Disable sampler SM A; drain its RX FIFO to a clean state. In dual-lane modes
   (WIDE11), disable sampler SM B and drain its RX FIFO as well.
3. Abort ring DMA channel A (single lane) or channels A and B (dual lane):
   disable the DMA IRQ, clear the EN bit, call `dma_channel_abort`, then clear
   the DMA IRQ. Channel ownership is **retained** — neither DMA channel is
   released back to the pool, and `la_packed_burst_release_dma_locked()` is not
   called.
4. Drain only the already-committed ring data through the normal consumer path.

The freeze is idempotent (the `acquisition_frozen` guard is checked first) so a
second terminal arriving while the writer is already frozen is a no-op. The dual-
lane helper `linkr_debugger_logic_analyzer_ring_freeze_policy(lane_count)` is a
pure policy function returning the bitmask of hardware to stop, so the freeze
policy is independently testable on the host.

#### Lossless-or-stop capacity outcomes on the wire

The matrix `pass` rule has three accepted shapes that map onto the existing wire
events without changing any public frame type, payload field, or enum value:

1. **Exact bounded completion**: the consumer received exactly `post_samples`
   with zero `sample_index_gaps` and zero decode errors, followed by either
   server EVENT STOPPED or a successful client STOP_REQ/STOP_RESP cleanup.
2. **Client duration completion**: a sustained continuous run reached its requested
   duration cleanly and the client received STOP_RESP for its STOP_REQ.
3. **Explicit lossless capacity stop**: a continuous capture received EVENT
   OVERRUN with zero gaps/decode errors/disconnects and successful restart/health
   checks. The delivered DATA count may be zero or non-zero.

A zero-DATA capacity stop (no DATA frames, only a STOPPED/OVERRUN event) is
labeled `capacity_stop_before_data`. It is a capacity-stop diagnostic, not a
continuous-streaming pass: it shows the firmware stopped cleanly before the
consumer saw any data, which is what the lossless-or-stop contract requires,
but it is not a sustained-throughput claim. Rows that observe an immediate
`sample_index=0` OVERRUN or STOPPED with `data_frames=0` must keep this label.

#### Historical 2026-07-27 freeze-final WIDE11 HIL evidence

The freeze-final HIL matrices are historical `pre=0` evidence for the WIDE11
deep-burst path on the representative HIL setup. They are not evidence for the
2026-07-28 pre-trigger implementation:

- **Authoritative ring matrix** (`logic-analyzer-wide11-packed-all-freeze-final.json`):
  **54/54 cases pass**, `overall_pass=true`: 36 bounded NONE cases across
  SINGLE/FAST8/WIDE11, 1/5/25 MHz, post=1024/4096, and TCP/WS, plus 18
  five-second continuous NONE cases across the same mode/rate/transport combinations.
- **High-rate matrix** (`logic-analyzer-high-rate-packed-burst-freeze-final.json`):
  **62/62 cases pass**, `overall_pass=true`. WS SINGLE/FAST8/WIDE11 bounded
  deep bursts plus stream sentinel cases at 100 MHz (WIDE11 native) and the
  quantized 125 MHz (125081 kHz actual) rate for SINGLE/FAST8, with strict
  zero-gap checks.
- **WIDE11 exact 100000/98 DATA / zero gaps** (WIDE11 rows in the high-rate
  matrix): `received_sample_count=100000`, `data_frames=98`,
  `sample_index_gaps=0`, `disconnects=0`, `overrun_events=0`, `error_events=0`,
  `payload_over_budget_frames=0`, `data_decode_error_frames=0`,
  `invalid_sample_count_frames=0`, `stopped_events=1`, `last_sample_index=99999`,
  and the terminal is `server_stopped`; this server auto-completion does not send
  a client STOP_REQ or receive STOP_RESP. Each
  successful WIDE11 deep burst drives 98 DATA frames (100000 samples × 11 bits /
  8 ≈ 1024 samples per frame at the project ceiling of 1024 samples per DATA
  frame).
- **Mapping** (`logic-analyzer-wide11-mapping-freeze-final-sequential.json`): the WIDE11
  mapping HIL drives GP10 with a UART stimulus and holds GP11–GP20 low. The
  full pass was `tcp` WIDE11 at `channel_mask=0x07FF` (11 bits), 100 MHz,
  post=100000, `received_sample_count=100000`, `data_frames=98`,
  `sample_index_gaps=0`, with a clean STOP_RESP and post-capture board-health
  recovery. This is the only physical-stimulus mapping pattern covered by the
  freeze-final HIL; **it does not demonstrate high-state mapping on GP18–GP20
  or simultaneous dual-lane transitions**.
- **Telemetry isolation** (`logic-analyzer-wide11-telemetry-isolation-freeze-final-sequential.json`):
  pass. The arena lease fully quiesces the board's ADC HTTP/telemetry
  (`baseline.connected=true`, `overlap.expected_pause_observed=true`,
  `pre_pause_delivery_grace_records` non-empty during the lease window,
  `pause_record_count=0`, post-pause telemetry resumes cleanly), and the
  post-lease HTTP `/api/v1/status` plus `/api/v1/adc/read` health checks pass
  within the freeze-final envelope.
- **Final freeze-final build footprint**: application FLASH 695868 B,
  application RAM 475832 B, combined UF2 (MCUboot + application) 1443840 B.
  Only the combined UF2 is flashed for initial install or recovery; the
  application binary alone is reserved for OTA updates through MCUboot.

At the requested 125 MHz rate the actual rate returned in the CONFIG_RESP ACK
is the RP2350 quantized 125081 kHz even when the caller asked for 125000 kHz.
The freeze-final HIL still accepts that quantized actual rate for SINGLE/FAST8
and treats a quantized 125 MHz ACK at WIDE11 as a matrix-correct START
rejection (INVALID_CONFIG, error code 7). The matrix-eligibility rule uses the
**requested** rate, not the quantized actual: WIDE11 is capped at requested
100 MHz; SINGLE/FAST8 stay at requested up to 125 MHz.

### Capture Behavior

All acquisitions capture at the requested sample rate until a stop condition:

- **Ring buffer overrun or possible overrun**: Data loss detected, capture stops
- **limit_samples reached**: Requested sample count captured (bounded mode)
- **post_samples=0 at high rate (GENERIC_PACKED_BURST)**: Captures exactly 100000 samples losslessly, then normal auto-STOP/drain
- **post_samples=0 at lower rates**: Runs until host STOP (ring streaming)
- **Host STOP command**: Client requests stop
- **Network failure**: Connection lost

**Duration by rate** (packed ring: SINGLE/FAST8 32768B packed wrap ring; WIDE11 lane A 16384B + lane B 8192B):
| Mode | Rate | Clock Divider | Usable Capacity | Duration at Full Speed |
|------|------|--------------|-----------------|----------------------|
| SINGLE | 125 MHz | 1 | 260096 samples | ~2.08 ms |
| SINGLE | 100 kHz | 1250 | 260096 samples | ~2.6 s |
| FAST8 | 125 MHz | 1 | 30720 samples | ~0.25 ms |
| FAST8 | 100 kHz | 1250 | 30720 samples | ~307 ms |
| WIDE11 | 100 MHz | 1 | 14336 samples | ~0.14 ms |
| WIDE11 | 100 kHz | 1250 | 14336 samples | ~143 ms |

**Key insight**: Continuous protocol captures are bounded by the packed ring and
USB/NCM transport. Higher rates fill the buffer faster; if continuity
can no longer be proven, firmware emits a terminal OVERRUN/ERROR event and stops
instead of silently continuing (lossless-or-stop). The packed ring does not increase
arena beyond 144184B.

## Session Flow

```
Client                          Server
  │                               │
  │──── HELLO_REQ ───────────────▶│
  │◀──── HELLO_RESP ─────────────│
  │                               │
  │──── CAPS_REQ ────────────────▶│
  │◀──── CAPS_RESP ──────────────│
  │                               │
  │──── CONFIG_REQ ──────────────▶│
  │◀──── CONFIG_RESP ────────────│
  │                               │
  │──── START_REQ ───────────────▶│
  │◀──── START_RESP ─────────────│  (trigger-safe barrier)
  │◀──── EVENT (armed) ──────────│  (rising/falling/either only)
  │◀──── EVENT (triggered) ──────│  (on trigger condition)
  │◀──── DATA ───────────────────│
  │◀──── DATA ───────────────────│
  │                               │
  │──── STOP_REQ ────────────────▶│
  │◀──── STOP_RESP ──────────────│
  │◀──── EVENT (stopped) ─────────│
```

## Stop Conditions

| Condition | Event Type | Detail | Notes |
|-----------|------------|--------|-------|
| limit_samples reached | STOPPED | 0 | Lossless-or-stop exact bounded pass when zero gaps |
| High-rate post=0 capacity stop (GENERIC_PACKED_BURST) | STOPPED | 0 | Bounded lossless pass when 100000 samples with zero gaps; `terminal_reason="server_stopped"` |
| Manual user stop | STOPPED | 1 | Host-initiated normal stop |
| Ring buffer overrun or possible overrun | OVERRUN | - | Lossless-or-stop; freeze-before-drain (writer frozen, then committed ring drained) |
| Transport backpressure | OVERRUN | - | Same lossless-or-stop + freeze-before-drain path |
| Zero-DATA capacity stop | STOPPED or OVERRUN | 0 / - | Labeled `capacity_stop_before_data`; capacity-stop diagnostic only, not a sustained-throughput claim |
| Network failure | ERROR | - | Lossless-or-stop with frozen writer |
| DMA error | ERROR | - | Lossless-or-stop with frozen writer |

## Transport Reliability

On the WebSocket transport, all Sigrok control, response, and event sends use a 1000ms
timeout. A send failure disconnects the client, releases capture ownership, and resets
its stream queue. This bounded-send cleanup is state-independent and does not alter
the protocol request/response ordering. On the final freeze build, canonical
sysbuild passed with app flash 695868 B and RAM 475832 B;
only the combined UF2
(`build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`) was flashed. Forced WS RST
immediately after a 125MHz NONE/post=1 START: a fresh session 2s later acquired ownership,
received one sample at actual 125.081 MHz with 0 gaps, received STOP_RESP, and reported HTTP
health. Normal WS SINGLE rising pre=0/post=512 at requested 100 MHz passed with exactly 512
samples, trigger index 0, 0 gaps, no DATA/EVENT before START_RESP, immediate restart, and HTTP
health. HTTP BOOTSEL entry succeeded (picotool lacked permissions; combined UF2 copied through
udisksctl); CDC `/dev/ttyACM2` `bootloader` command entered ROM BOOTSEL (serial read ended with
expected EIO on USB disconnect); same combined UF2 restored normal HTTP startup.

## Performance

### CPU Usage

| Component | Overhead |
|-----------|----------|
| PIO capture | 0% |
| PIO trigger | 0% |
| DMA transfer | 0% |
| IRQ handler | ~0.1% |
| Compression | ~3% |
| Network send | ~2% |
| **Total** | **~6%** |

### Maximum Sample Rates

| Mode | Channels | Max Rate | Notes |
|------|----------|----------|-------|
| Fast8 | 1-8 | 125 MHz finite sampler | Continuous ceiling is lower |
| Wide12 (historical, not current WIDE11) | 1-12 | 125 MHz finite sampler | Retained for archival context; the current WIDE11 contract caps at requested 100 MHz |

125 MHz remains a finite hardware samplerate, not a sustained network streaming
promise. Continuous sessions are stopped on ring overrun, possible overrun,
transport drop, or bounded-capture completion. The current WIDE11 contract caps
mode 2 at requested 100 MHz; a 125 MHz request against WIDE11 is rejected by
START with INVALID_CONFIG.

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

For the WIDE11 deep-burst path specifically, the current HIL evidence in
`logic-analyzer-high-rate-packed-burst-freeze-final.json` shows
`received_sample_count=100000`, `data_frames=98`, `sample_index_gaps=0`,
`disconnects=0`, `overrun_events=0`, `error_events=0`,
`payload_over_budget_frames=0`, `data_decode_error_frames=0`, `stopped_events=1`,
`last_sample_index=99999`, and EVENT STOPPED without a client STOP_REQ/STOP_RESP,
all on the representative
HIL setup. The full matrices are 54/54 (generic) and 62/62 (high-rate) per the
"Final freeze-final WIDE11 HIL evidence" block above.

Adjacent WS SINGLE failure was not measured under the final architecture. The 1MHz
result is a verified operating point, not a claimed absolute ceiling. These are
measured results on the representative HIL setup; they do not claim 125 MHz
sustained continuous streaming, 25 MHz sustained streaming, full GP11–GP20
high-state mapping, or watchdog fault-injection or automatic recovery validation.

## Implementation Notes

### Firmware

- PIO SM1: Capture (1 instruction + autopush, variable clock divider)
- PIO SM2: Hardware trigger (3 instructions, 0 CPU)
- Two capture engines: packed finite DMA and packed DMA ring; SINGLE/FAST8 uses one
  32768B packed wrap ring; WIDE11 uses lane A 16384B plus lane B 8192B inside the same
  32768B slice, with a common capacity of 16384 samples and safety margin of 2048;
  usable capacities are SINGLE 260096, FAST8 30720, WIDE11 14336; WIDE11 writer enforces
  minimum lane sequence and stops when greater than 20 sample skew is detected; the packed
  ring does not increase arena beyond 144184B
- LA sink: protocol-neutral consumer with 8 DATA slots plus 1 terminal slot pool;
  WS SINGLE emits up to 2048 packed samples per fixed DATA slot (protocol frame
  remains standard 8-byte DATA meta plus RLE or BIT_PACK payload; this is an
  implementation fact, not a wire requirement, and does not imply TCP must use
  2048-sample chunks); sink consumer runs at priority 7 and blocks naturally on
  full chunks; legacy callback/TCP/non-SINGLE paths run at priority 8 with yield;
  sender-side one-byte RLE with BIT_PACK fallback; 6144-byte coalescing buffer;
  explicit terminal on buffer pressure; no silent sample drops (lossless-or-stop);
  terminal-freeze policy freezes the trigger SM, the sampler SM(s), and the ring
  DMA channel(s) before the consumer drains the ring (channel ownership retained)
- Bounded pre=0 and post=1..512 use exact finite PIO+DMA: trigger NONE is
  ungated immediate, rising/falling are hardware IRQ-gated, EITHER snapshots
  current level at arm time then waits for opposite edge using same 3-instruction
  trigger path (arm-time race exists because firmware samples level before hardware
  arm); post>512 bounded uses packed ring streaming; at negotiated high rates with
  GENERIC_PACKED_BURST, post=0 uses packed burst (exactly 100000 samples, then
  auto-STOP/drain); at lower non-packed rates post=0 uses packed ring streaming
- Finite max hardware rate: 125 MHz (clock_div=1); continuous transport ceiling is lower

### Capture Behavior

START_RESP is the trigger-safe barrier. It is emitted only after the firmware has acquired capture ownership and successfully prepared the PIO/DMA backend. Ordinary finite/ring paths are already armed or running at this point. For WIDE11 deep burst, ownership, quiesce, and configuration are ready before the response. NONE sends START_RESP in RUNNING state without an ARMED event; triggered captures send START_RESP in ARMED state followed by the ARMED event. GO then synchronously enables both sampler SMs. A host that receives START_RESP may immediately send UART trigger stimulus or any other action that requires the capture engine to be ready; no false ARMED or RUNNING EVENT will precede it. If the start fails, the firmware returns a synchronous FRAME_ERROR instead and emits no ARMED or RUNNING EVENT. Triggered sessions then emit TRIGGERED, DATA frames, and STOPPED; NONE sessions proceed directly to DATA and STOPPED.

EITHER trigger first snapshots the pin level in firmware before arming the hardware trigger, then waits for the opposite edge using the same 3-instruction trigger path. Because the firmware samples the pin level before arming the hardware trigger, there is an arm-time race window: the actual level may change between the firmware sample and the hardware arm. NONE trigger is ungated immediate and starts directly in RUNNING state without emitting an ARMED EVENT.

**Bounded captures (pre=0, post=1..512)**: Use exact finite PIO+DMA engine. NONE is ungated immediate; rising/falling use hardware IRQ-gated detection; EITHER first snapshots level then waits for opposite edge (arm-time race exists).

**Bounded captures (pre=0, post>512 to 65535)**: Use ring streaming.

**Continuous captures (post=0)**:
- At supported high rates (100/125 MHz for SINGLE/FAST8, 100 MHz for WIDE11) with GENERIC_PACKED_BURST: captures exactly 100000 samples losslessly, then normal auto-STOP/drain
- At lower rates: runs until host STOP (ring streaming)

**Terminal-freeze policy (freeze-before-drain)**:
When a non-OK terminal cause fires while the writer is still active, the writer
is frozen **before** the consumer drains the ring: the trigger SM is disabled,
the sampler SM(s) are disabled and their RX FIFOs are drained, the ring DMA
channel(s) are aborted (DMA IRQ disabled, EN bit cleared, `dma_channel_abort`,
DMA IRQ cleared), and channel ownership is retained so `la_packed_burst_release_dma_locked()`
is not invoked. The consumer then drains only the already-committed ring data.
This prevents the failure mode where overrun or error servicing kept hardware
alive long enough that the producer overwrote ring samples the consumer was
still draining; the freeze policy is what eliminates the hidden corruption,
not the sample-index-gap check. See "Terminal-Freeze Policy (freeze-before-drain)"
above for the full sequence and the idempotency / dual-lane policy helper.

Stop conditions are the same regardless of rate:
- Low rates (100 kHz): ring spans ~81.9 ms before safety margin
- High rates (125 MHz): ring spans only ~0.066 ms and is expected to stop on possible overrun

**Lossless-or-stop capacity outcomes**: matrix `pass` accepts exact bounded or
server completion, clean client duration completion with STOP_REQ/STOP_RESP, and
server-explicit OVERRUN capacity stops without requiring STOP_RESP. Zero-DATA
capacity stops are labeled `capacity_stop_before_data` and are capacity-stop
diagnostics only, not sustained-throughput claims. See "Lossless-or-stop
capacity outcomes on the wire" above.

### libsigrok Driver

- TCP client connecting to port 5556
- Parses binary frames, sends SR_DF_LOGIC
- Supports continuous streaming

### PulseView Integration

- Single device per connection
- Dynamic channel selection
- Compression transparent to UI
- Real-time waveform display
