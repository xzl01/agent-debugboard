# Linkr Sigrok Protocol v1

Binary framed protocol over TCP port 5556 for continuous logic analyzer streaming with hardware trigger and compression.

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
| 0x08 | START_RESP | S→C | Ack (6B) |
| 0x09 | STOP_REQ | C→S | None |
| 0x0a | STOP_RESP | S→C | Ack (6B) |
| 0x10 | EVENT | S→C | Event (6B) |
| 0x11 | DATA | S→C | Meta (7B) + Samples |
| 0x7f | ERROR | S→C | Error (3B) |

## Payload Formats

### HELLO_RESP (5 bytes)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | protocol_version | Protocol version (1) |
| 1 | 1 | server_flags | Reserved flags |
| 2 | 1 | mode_count | Number of modes |
| 3 | 2 | max_payload_len | Max payload size |

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
| 5 | PRE_TRIGGER | Supports pre-trigger capture |

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
| 8 | 2 | pre_samples | Pre-trigger depth |
| 10 | 2 | post_samples | Post-trigger depth (0=unlimited) |

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
| 0 | 3 | sample_index | First sample index |
| 3 | 2 | sample_count | Samples in chunk |
| 5 | 1 | compression | Compression type |
| 6 | 2 | channel_mask | Active channel mask |

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
- **Max sample rate**: 125 MHz
- **Sample format**: Dynamic (1-8 bits based on selected channels)
- **Compression**: BIT_PACK, RLE

### Mode 2: Wide12 (mode_id=2)

- **Channels**: GP10-GP20 + GP29 (12 channels)
- **Pin base**: 10
- **Pin mask**: 0x1FFF
- **Max sample rate**: 125 MHz
- **Sample format**: Dynamic (1-16 bits based on selected channels)
- **Compression**: BIT_PACK, RLE

### Capture Behavior

All acquisitions capture continuously at the requested sample rate until a stop condition:

- **Ring buffer overrun**: Data loss detected, capture stops
- **limit_samples reached**: Requested sample count captured
- **Host STOP command**: Client requests stop
- **Network failure**: Connection lost

**Duration by rate** (ring buffer = 98304 samples):
| Rate | Clock Divider | Duration |
|------|--------------|----------|
| 125 MHz | 1 | ~0.79 ms |
| 10 MHz | 12.5 | ~9.8 ms |
| 1 MHz | 125 | ~98 ms |
| 500 kHz | 250 | ~196 ms |
| 100 kHz | 1250 | ~983 ms |

**Key insight**: All captures are continuous. Higher rates fill the ring buffer faster, but the capture behavior is the same.

## Dynamic Channel Compression

### Channel Selection

`channel_mask` selects which channels to capture:

```
Mode 1 (Fast8): channel_mask bits 0-7 → GP10-GP17
Mode 2 (Wide12): channel_mask bits 0-11 → GP10-GP20, GP29
```

### Bytes Per Sample Calculation

```c
uint8_t channel_count = popcount(channel_mask);
uint8_t bytes_per_sample = (channel_count + 7) / 8;
```

| Selected Channels | Bytes Per Sample | Compression Ratio |
|------------------|-----------------|-------------------|
| 1 | 1 (8 bits) | 8x vs 1-byte raw |
| 2 | 1 (8 bits) | 4x vs 1-byte raw |
| 3-4 | 1 (8 bits) | 2-4x vs 1-byte raw |
| 5-8 | 1 (8 bits) | 1-2x vs 1-byte raw |
| 9-12 | 2 (16 bits) | 1-2x vs 2-byte raw |

### Bit Packing Format

Bits are packed LSB-first in order of channel_mask:

```
channel_mask = 0b00000101  (channels 0 and 2)
sample = [bit0: GP10][bit1: GP12]
packed_byte = (GP10_value << 0) | (GP12_value << 1)
```

### RLE Encoding

### Format

```
[value][count][value][count]...
- value: bytes_per_sample bytes
- count: 2 bytes (little-endian, max 65535)
```

### Example (8-bit, 1 channel)

```
Raw samples: 0x00 0x00 0x00 0x00 0xFF 0xFF 0xFF 0xFF
RLE encoded: 0x00 0x04 0x00 0xFF 0x04 0x00
             ^^^^ ^^^^^^^^ ^^^^ ^^^^^^^^
             value count=4 value count=4
```

### Example (16-bit, 3 channels)

```
Raw samples: 0x05 0x05 0x05 0x03 0x03
RLE encoded: 0x05 0x03 0x00 0x03 0x02 0x00
             ^^^^ ^^^^^^^^ ^^^^ ^^^^^^^^
             value count=3 value count=2
```

### Compression Ratios

| Signal Type | Typical Ratio |
|-------------|---------------|
| UART idle | 90-95% |
| SPI clock | 50-70% |
| I2C data | 40-60% |
| High-freq PWM | 10-20% |

## Trigger Architecture

### PIO Dual-SM Design

```
SM1 (Capture): in pins, 32 (autopush)
SM2 (Trigger): wait_pin(trigger_pin); irq_set(0)
```

### Trigger Flow

1. **ARM**: Configure SM2 with trigger pin and type
2. **CAPTURE**: SM1 continuously captures to ring buffer
3. **TRIGGER**: SM2 detects condition, sets IRQ 0
4. **OUTPUT**: Software reads ring buffer, sends pre-trigger + post-trigger

### Pre-trigger Capture

- Ring buffer continuously overwrites old data
- When trigger fires, marks position in ring buffer
- Outputs pre-trigger samples from ring buffer history
- Continues with post-trigger samples

### Trigger Types

| Type | PIO Program | Behavior |
|------|-------------|----------|
| Rising | `wait_pin(high)` | Low→High transition |
| Falling | `wait_pin(low)` | High→Low transition |
| Either | `jmp_pin` + `wait_pin` | Any edge |

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
  │◀──── START_RESP ─────────────│
  │◀──── EVENT (armed) ──────────│  (if trigger)
  │◀──── EVENT (triggered) ──────│  (on trigger)
  │◀──── EVENT (running) ────────│
  │◀──── DATA ───────────────────│  (continuous)
  │◀──── DATA ───────────────────│
  │                               │
  │──── STOP_REQ ────────────────▶│
  │◀──── STOP_RESP ──────────────│
  │◀──── EVENT (stopped) ────────│
```

## Stop Conditions

| Condition | Event Type | Detail |
|-----------|------------|--------|
| limit_samples reached | STOPPED | 0 |
| Ring buffer overrun | OVERRUN | - |
| Host STOP command | STOPPED | 1 |
| Network failure | ERROR | - |
| DMA error | ERROR | - |

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

### Bandwidth Optimization

| Optimization | Savings |
|--------------|---------|
| Header (9B) | Minimal |
| DATA meta (7B) | Optimized |
| ACK (6B) | Includes actual rate |
| Bit packing | 2-8x |
| RLE | 10-100x |

### Maximum Sample Rates

| Mode | Channels | Max Rate | Limited By |
|------|----------|----------|------------|
| Fast8 | 1-8 | 125 MHz | PIO / Ring buffer |
| Wide12 | 1-12 | 125 MHz | PIO / Ring buffer |

**Note**: All rates support continuous capture. Higher rates fill ring buffer faster.

## Error Handling

### Protocol Errors

- Invalid magic/version → ERROR + disconnect
- Oversize payload → ERROR + disconnect
- Invalid state → ERROR (no disconnect)
- Invalid config → ERROR with detail

### Capture Errors

- Overrun → EVENT (overrun) + stop
- DMA error → EVENT (error) + stop
- Network failure → stop + cleanup

## Implementation Notes

### Firmware

- PIO SM1: Capture (1 instruction + autopush, variable clock divider)
- PIO SM2: Hardware trigger (2 instructions, 0 CPU)
- DMA: Ring buffer (98304 bytes)
- Compression: Bit-pack + RLE in DMA callback
- Max rate: 125 MHz (clock_div=1)

### Capture Behavior

All captures are continuous by nature:
- Low rates (100 kHz): Ring buffer lasts ~983 ms, long continuous streaming
- High rates (125 MHz): Ring buffer fills in ~0.79 ms, stops on overrun
- Stop conditions are the same regardless of rate

### libsigrok Driver

- TCP client connecting to port 5556
- Parses binary frames, sends SR_DF_LOGIC
- Handles pre-trigger data output
- Supports continuous streaming

### PulseView Integration

- Single device per connection
- Dynamic channel selection
- Compression transparent to UI
- Real-time waveform display

### HTTP LA Unification (Future)

The existing HTTP logic analyzer will be migrated to use this same protocol:
- Same message types and framing
- Same compression and trigger architecture
- Unified capture arbiter
- Single protocol for all capture modes

## Current Implementation Status

> **Note**: The current implementation uses ping-pong DMA buffers instead of the
> hardware ring buffer described in the architecture document. See
> [ring-buffer-gap-analysis.md](ring-buffer-gap-analysis.md) for detailed gap
> analysis.

### Verified Working

- Protocol framing (9-byte header, all message types)
- HELLO/CAPS/CONFIG/START/STOP/EVENT/DATA flow
- Trigger detection (rising/falling/either) at 100 kHz - 100 MHz
- Signal verification (0x55 UART pattern correctly captured)
- Bit-pack compression
- Actual sample rate negotiation (125081 kHz for 125 MHz request)

### Known Limitations

- **125 MHz**: Only burst capture (512 samples), not continuous streaming
- **Pre-trigger**: Software circular buffer (4096 samples max)
- **Sustainable rate**: ~13 kHz effective throughput at high rates
- **Ring buffer**: Not implemented (uses ping-pong DMA)

### Tested Rates

| Rate | Status | Notes |
|------|--------|-------|
| 100 kHz | ✅ PASS | Continuous streaming |
| 500 kHz | ✅ PASS | Continuous streaming |
| 1 MHz | ✅ PASS | Continuous streaming |
| 5 MHz | ✅ PASS | Continuous streaming |
| 10 MHz | ✅ PASS | Continuous streaming |
| 50 MHz | ✅ PASS | Continuous streaming |
| 100 MHz | ✅ PASS | Continuous streaming |
| 125 MHz | ⚠️ BURST | 512 samples max, crashes on continuous |
