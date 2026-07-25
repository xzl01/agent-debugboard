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
| 0x05 | CONFIG_REQ | C→S | Config (12B) |
| 0x06 | CONFIG_RESP | S→C | Ack (6B) |
| 0x07 | START_REQ | C→S | None |
| 0x08 | START_RESP | S→C | Ack (6B) — emitted only after capture ownership is acquired and the PIO/DMA backend is successfully armed and running; receiving this frame is the trigger-safe barrier. A failed start returns a synchronous FRAME_ERROR and no false ARMED or RUNNING event. STATE field reads 2 (ARMED) or 3 (RUNNING for no-trigger) immediately after START_RESP. |
| 0x09 | STOP_REQ | C→S | None |
| 0x0a | STOP_RESP | S→C | Ack (6B) |
| 0x10 | EVENT | S→C | Event (6B) |
| 0x11 | DATA | S→C | Meta (8B) + Samples |
| 0x7f | ERROR | S→C | Error (3B) |

## Payload Formats

### HELLO_RESP (5 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | protocol_version | Protocol version (1) |
| 1 | 1 | server_flags | Reserved flags |
| 2 | 1 | mode_count | Number of modes |
| 3 | 2 | max_payload_len | Max payload size |

The current maximum payload is 4104 bytes: 8 bytes of DATA metadata plus up to
4096 bytes of compressed sample data. Current firmware chunks contain at most
1024 points, so WIDE12 bit-pack data is at most 2048 bytes and remains within
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

### Mode 2: Wide12 (mode_id=2)

- **Channels**: GP10-GP20 + GP29 (12 channels)
- **Pin base**: 10
- **Pin mask**: 0x1FFF
- **Sample format**: Dynamic (1-16 bits based on selected channels)
- **Compression**: BIT_PACK, RLE

### Capture Behavior

All acquisitions capture at the requested sample rate until a stop condition:

- **Ring buffer overrun or possible overrun**: Data loss detected, capture stops
- **limit_samples reached**: Requested sample count captured (bounded mode)
- **post_samples=0 with no limit**: Stream mode, runs until host STOP
- **Host STOP command**: Client requests stop
- **Network failure**: Connection lost

**Duration by rate** (RP2350 hardware write ring = 32768 bytes / 8192 32-bit samples):
| Rate | Clock Divider | Duration |
|------|--------------|----------|
| 125 MHz | 1 | ~0.066 ms |
| 10 MHz | 12.5 | ~0.82 ms |
| 1 MHz | 125 | ~8.2 ms |
| 500 kHz | 250 | ~16.4 ms |
| 100 kHz | 1250 | ~81.9 ms |

**Key insight**: Continuous protocol captures are bounded by the hardware ring and
USB/NCM transport. Higher rates fill the 8192-sample ring faster; if continuity
can no longer be proven, firmware emits a terminal OVERRUN/ERROR event and stops
instead of silently continuing.

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

| Condition | Event Type | Detail |
|-----------|------------|--------|
| limit_samples reached | STOPPED | 0 |
| Ring buffer overrun or possible overrun | OVERRUN | - |
| Host STOP command | STOPPED | 1 |
| Network failure | ERROR | - |
| DMA error | ERROR | - |

## Transport Reliability

On the WebSocket transport, all Sigrok control, response, and event sends use a 1000ms
timeout. A send failure disconnects the client, releases capture ownership, and resets
its stream queue. This bounded-send cleanup is state-independent and does not alter
the protocol request/response ordering. Post-patch regression confirmed: canonical sysbuild
passed; final app flash 657020 B, RAM 511856 B; only the combined UF2
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
| Wide12 | 1-12 | 125 MHz finite sampler | Continuous ceiling is lower |

125 MHz remains a finite hardware samplerate, not a sustained network streaming
promise. Continuous sessions are stopped on ring overrun, possible overrun,
transport drop, or bounded-capture completion.

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
result is a verified operating point, not a claimed absolute ceiling. These are
measured results on the representative HIL setup; they do not claim 125 MHz
sustained continuous streaming and do not claim watchdog fault-injection or
automatic recovery validation.

## Implementation Notes

### Firmware

- PIO SM1: Capture (1 instruction + autopush, variable clock divider)
- PIO SM2: Hardware trigger (3 instructions, 0 CPU)
- DMA: RP2350 hardware write ring (32768 bytes, size_bits=15, 8192 32-bit samples)
  using the official RP2350 DMA `TRANS_COUNT` ENDLESS mode; 7168 sample usable
  threshold before safety margin
- LA sink: protocol-neutral consumer with 8 DATA slots plus 1 terminal slot pool;
  WS SINGLE emits up to 2048 packed samples per fixed DATA slot (protocol frame
  remains standard 8-byte DATA meta plus RLE or BIT_PACK payload; this is an
  implementation fact, not a wire requirement, and does not imply TCP must use
  2048-sample chunks); sink consumer runs at priority 7 and blocks naturally on
  full chunks; legacy callback/TCP/non-SINGLE paths run at priority 8 with yield;
  sender-side one-byte RLE with BIT_PACK fallback; 6144-byte coalescing buffer;
  explicit terminal on buffer pressure; no silent sample drops
- Bounded pre=0 and post=1..512 use exact finite PIO+DMA: trigger NONE is
  ungated immediate, rising/falling are hardware IRQ-gated, EITHER snapshots
  current level at arm time then waits for opposite edge using same 3-instruction
  trigger path (arm-time race exists because firmware samples level before hardware
  arm); post>512 bounded and continuous post=0 use ring streaming
- Finite max hardware rate: 125 MHz (clock_div=1); continuous transport ceiling is lower

### Capture Behavior

START_RESP is the trigger-safe barrier. It is emitted only after the firmware has acquired capture ownership and the PIO/DMA backend is successfully armed and running. A host that receives START_RESP may immediately send UART trigger stimulus or any other action that requires the capture engine to be ready; no false ARMED or RUNNING EVENT will precede it. If the start fails, the firmware returns a synchronous FRAME_ERROR instead and emits no ARMED or RUNNING EVENT. After START_RESP, the session follows the ordered progression: START_RESP with state 2 (ARMED) or state 3 (RUNNING for no-trigger NONE), then EVENT armed (rising/falling/either only), then EVENT triggered, then DATA frames, then EVENT stopped.

EITHER trigger first snapshots the pin level in firmware before arming the hardware trigger, then waits for the opposite edge using the same 3-instruction trigger path. Because the firmware samples the pin level before arming the hardware trigger, there is an arm-time race window: the actual level may change between the firmware sample and the hardware arm. NONE trigger is ungated immediate and starts directly in RUNNING state without emitting an ARMED EVENT.

Continuous protocol captures use the hardware ring until an explicit stop condition:
- Low rates (100 kHz): ring spans ~81.9 ms before safety margin
- High rates (125 MHz): ring spans only ~0.066 ms and is expected to stop on possible overrun
- Stop conditions are the same regardless of rate

### libsigrok Driver

- TCP client connecting to port 5556
- Parses binary frames, sends SR_DF_LOGIC
- Supports continuous streaming

### PulseView Integration

- Single device per connection
- Dynamic channel selection
- Compression transparent to UI
- Real-time waveform display
