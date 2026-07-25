# Linkr Logic Analyzer Capture and Trigger Architecture

## Overview

The firmware logic analyzer uses RP2350 PIO2+DMA for high-speed GPIO capture.
The sigrok binary protocol runs over two transports:

- **Web UI**: Creates live session via `/api/v1/live-sessions`, then speaks
  sigrok binary protocol over `/api/v1/ws/<slot>` WebSocket
- **Native sigrok**: Connects via raw-TCP port 5556

The 32 KiB RP2350 hardware write ring is the capture buffer. On overrun,
possible overrun, transport backpressure, or bounded-capture completion, the
firmware emits a terminal event and stops rather than silently continuing.
DATA and EVENT sample indices wrap modulo 24 bits; wrap is not a terminal
condition.

**Stop conditions** (same for all rates):
- Ring buffer overrun or possible overrun
- limit_samples reached (bounded mode)
- Host STOP command
- Network failure

```
GPIO Pins: GP10-GP20, GP29
                    │
                    ▼
┌─────────────────────────────────────────────────────────┐
│ PIO Block                                               │
│  ┌─────────────────────┐    ┌─────────────────────┐     │
│  │ SM1 (Capture)       │    │ SM2 (Trigger)       │     │
│  │ in pins, 32         │    │ wait_gpio edge      │     │
│  │ (autopush)          │    │ irq set 0           │     │
│  └─────────────────────┘    └─────────────────────┘     │
└─────────────────────────────────────────────────────────┘
                    │
                    ▼
┌─────────────────────────────────────────────────────────┐
│ DMA Controller                                          │
│  SM1 FIFO ──▶ RP2350 DMA write ring (32768 bytes)       │
│  Continuous transfer, 0 CPU overhead                     │
└─────────────────────────────────────────────────────────┘
                    │
                    ▼
┌─────────────────────────────────────────────────────────┐
│ Ring Buffer                                             │
│  32768 bytes / 8192 32-bit samples                      │
│  Official RP2350 DMA ENDLESS TRANS_COUNT encoding        │
└─────────────────────────────────────────────────────────┘
                    │
                    ▼
┌─────────────────────────────────────────────────────────┐
│ Software (IRQ Handler)                                  │
│  IRQ 0 → mark_trigger_pos → start_output                │
└─────────────────────────────────────────────────────────┘
```

## PIO State Machines

### SM1: Capture

**Purpose**: Continuously read GPIO pins and push to FIFO for DMA transfer.

**Program** (1 instruction with autopush):

```c
static const uint16_t sm1_program[] = {
    pio_encode_in(pio_pins, 32),     // Read GPIO (1 cycle, auto-push at 32 bits)
};
```

**Configuration**:
- `in_base`: GP10 (for fast8) or GP0 (for wide12)
- `in_count`: 32 bits (reads all GPIO, software extracts relevant bits)
- `autopush`: enabled (threshold=32)
- `wrap`: enabled (loop back to same instruction)
- `clock_div`: calculated from requested sample rate

**Timing**:
- 1 instruction/sample → 125 MHz max (clock_div=1)
- Clock divider adjustable for lower rates:
  - 100 kHz: clock_div=1250
  - 500 kHz: clock_div=250
  - 1 MHz: clock_div=125
  - 10 MHz: clock_div=12.5

### SM2: Trigger Detection

**Purpose**: Wait for trigger condition on specified pin, then signal via IRQ.

**Program** (3 instructions for all armed trigger types):

```c
// Rising edge: wait for currently low, then wait for high, then signal
static const uint16_t sm2_trigger_rising[] = {
    pio_encode_wait_gpio(false, trigger_pin_abs),  // Wait for currently low (settle)
    pio_encode_wait_gpio(true, trigger_pin_abs),   // Wait for high (edge)
    pio_encode_irq_set(0, false),                  // Set IRQ 0
};

// Falling edge: wait for currently high, then wait for low, then signal
static const uint16_t sm2_trigger_falling[] = {
    pio_encode_wait_gpio(true, trigger_pin_abs),   // Wait for currently high (settle)
    pio_encode_wait_gpio(false, trigger_pin_abs),  // Wait for low (edge)
    pio_encode_irq_set(0, false),                  // Set IRQ 0
};

// Either edge: firmware snapshots raw GPIO level during arm, maps low→rising
// or high→falling, then loads the matching 3-instruction program above.
// No jmp_pin is used. There is a small arm-time race: the pin level may change
// between the firmware sample and the first wait_gpio instruction.
static const uint16_t sm2_trigger_either_rising[] = {
    pio_encode_wait_gpio(false, trigger_pin_abs),  // Wait for low
    pio_encode_wait_gpio(true, trigger_pin_abs),   // Wait for high
    pio_encode_irq_set(0, false),
};
static const uint16_t sm2_trigger_either_falling[] = {
    pio_encode_wait_gpio(true, trigger_pin_abs),   // Wait for high
    pio_encode_wait_gpio(false, trigger_pin_abs),  // Wait for low
    pio_encode_irq_set(0, false),
};
```

**Note for RP2350**: Use `wait_gpio` with absolute GPIO numbers, not `wait_pin`.
On RP2350, `wait_pin` uses GPIO_BASE (default=0) for pin remapping, while
`in pins` uses IN_BASE. Using `wait_gpio` with absolute GPIO numbers avoids
this mismatch.

## DMA Ring Buffer

### Memory Layout

```c
#define LINKR_DEBUGGER_LA_RING_BUFFER_BYTES (32768U)

struct linkr_debugger_la_ring_buffer {
    uint32_t buffer[LINKR_DEBUGGER_LA_RING_BUFFER_BYTES / sizeof(uint32_t)];
    volatile uint32_t write_pos;    // DMA write position (updated from hardware)
    volatile uint32_t read_pos;     // Software read position
    volatile uint32_t trigger_pos;  // Position at trigger
    volatile bool triggered;        // Trigger flag
    volatile bool overrun;          // Overrun flag
};
```

### DMA Configuration

```c
dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
channel_config_set_read_increment(&cfg, false);
channel_config_set_write_increment(&cfg, true);
channel_config_set_ring(&cfg, true, 15U);  // RP2350 maximum 32768-byte wrap
channel_config_set_dreq(&cfg, pio_get_dreq(pio, sm1, false));

dma_channel_configure(dma_chan, &cfg,
    ring_buffer,
    &pio->rxf[sm1],
    dma_encode_endless_transfer_count(),  // official RP2350 endless encoding
    false
);
```

RP2350 DMA supports a maximum 32768-byte address ring (`size_bits=15`).
The continuous backend uses one 32768-byte-aligned `uint32_t[8192]` hardware
write ring.

## Trigger Architecture

### Trigger Flow

```
1. ARM
   - Configure SM2 with trigger pin and type
   - Start SM1 and DMA
   - Ring buffer starts filling

2. CAPTURE
   - SM1 reads GPIO at configured rate
   - DMA transfers to ring buffer
   - Ring buffer overwrites old data when full
   - CPU overhead: 0%

3. TRIGGER DETECTED (if configured)
   - SM2 detects trigger condition
   - SM2 sets IRQ 0
   - IRQ handler marks trigger_pos
   - Sets triggered = true
   - CPU overhead: ~0.1%

4. OUTPUT
   - Read pre-trigger samples from ring buffer (if applicable)
   - Continue reading post-trigger samples
   - Compress and send to host
   - CPU overhead: ~6%

5. STOP CONDITION
   - Ring buffer overrun or possible overrun
   - limit_samples reached
   - Host sends STOP command
   - Network failure
```

### Trigger Types

| Type | PIO Program | Behavior |
|------|-------------|----------|
| Rising | `wait_gpio(false)` then `wait_gpio(true)` | Waits for currently low then high, triggers on low→high |
| Falling | `wait_gpio(true)` then `wait_gpio(false)` | Waits for currently high then low, triggers on high→low |
| Either | Firmware snapshots level, maps to rising or falling, then uses same 3-instruction SM | Small arm-time race exists (level sampled before hardware arm); no `jmp_pin` used |
| None | (no SM2) | Immediate start, no trigger |

### Trigger Channel Selection

Any safe GPIO can be used as trigger:

```c
// Safe trigger pins
static const uint8_t safe_trigger_pins[] = {
    10, 11, 12, 13, 14, 15,     // J16: GP10-GP15
    16, 17, 18, 19, 20,         // J16: GP16-GP20
    29                           // J16: GP29 (ADC3)
};
```

Note: GP7-GP9 are not available in Sigrok modes.

## Pre-Trigger

Pre-trigger capture is not exposed in the Web UI. The Web UI always sends
`pre_samples=0`. Bounded captures with pre=0 and post=1..512 use an exact finite
PIO+DMA engine: trigger NONE is ungated immediate, rising and falling edges use
hardware IRQ-gated detection, and EITHER first snapshots the current pin level
in firmware then waits for the opposite edge using the same 3-instruction trigger
path. Because EITHER samples the pin level in firmware before arming the hardware
trigger, there is an arm-time race window. Larger bounded requests (post>512) and
continuous post=0 use ring streaming.

## Performance Characteristics

### CPU Usage

| Component | CPU Overhead | Notes |
|-----------|-------------|-------|
| SM1 capture | 0% | PIO hardware |
| SM2 trigger | 0% | PIO hardware |
| DMA transfer | 0% | DMA hardware |
| Timer polling | ~0.5% | 100 µs interval |
| Data compression | ~3% | Bit-pack + RLE |
| Network send | ~2% | TCP send |
| **Total** | **~6%** | At typical rates |

### Throughput

| Mode | Max Rate | Ring Buffer Duration | Notes |
|------|----------|---------------------|-------|
| Fast8 (1-8 channels) | 125 MHz finite sampler | ~0.066 ms raw ring at 125 MHz | Stop on possible overrun |
| Wide12 (1-12 channels) | 125 MHz finite sampler | ~0.066 ms raw ring at 125 MHz | Stop on possible overrun |
| Low-rate (any mode) | 100 kHz | ~81.9 ms raw ring | Longest continuous margin |

### Measured No-Gap Continuous Ceilings

On the representative HIL setup with the final architecture and official RP2350 ENDLESS:

| Path | Verified Result |
|------|----------------|
| WS bounded 100 kHz, post=65535 | SINGLE, FAST8, WIDE12 each received exactly 65535 samples with 0 gaps; restart true, HTTP health true |
| TCP bounded 100 kHz, post=65535 | SINGLE, FAST8, WIDE12 each received exactly 65535 samples with 0 gaps; restart true, HTTP health true |
| WS continuous 5 s no-gap operating points | SINGLE 1MHz verified (10 consecutive runs, ~4.991M-4.997M samples each, 998.16-998.70 ksps effective, zero sample-index gaps, zero disconnects, STOP response, immediate restart and HTTP health; 1MHz is a verified operating point, not a claimed absolute ceiling; adjacent failure not measured under the final architecture), FAST8 240 kHz, WIDE12 149 kHz |
| Historical/reference TCP continuous 5 s no-gap ceilings | SINGLE 443 kHz, FAST8 241 kHz, WIDE12 147 kHz |

This table describes the measured board/link setup. It does not claim 125 MHz
continuous streaming and does not claim watchdog fault-injection validation.

## Resource Usage

### PIO Resources

| Resource | Used | Available | Notes |
|----------|------|-----------|-------|
| PIO SM | 2 | 8 | SM1 + SM2 |
| Instructions | 4 max | 32 | 1 capture + 3 trigger instructions; EITHER uses the same 3-instruction path after firmware level-snapshot |
| GPIO pins | 12 | 29 | GP10-GP20, GP29 |

### DMA Resources

| Resource | Used | Available | Notes |
|----------|------|-----------|-------|
| DMA channel | 1 | 16 | Single channel |
| DMA IRQ | 0 | 16 | Polling mode |

### Memory Resources

| Resource | Size | Notes |
|----------|------|-------|
| DMA write ring | 32768 bytes | RP2350 hardware ring, `size_bits=15` |
| Stream scratch | 2048 bytes | Shared synchronous compression output |
| DMA config | 64 bytes | Runtime |

## Stop Conditions

| Condition | Trigger | Action |
|-----------|---------|--------|
| limit_samples reached | Sample count | Normal stop |
| Ring buffer overrun or possible overrun | write_pos catches read_pos | Error stop |
| Host STOP command | TCP message | Normal stop |
| Network failure | Send error | Error stop |

## Error Handling

### Overrun Detection

```c
void check_overrun(struct capture_ring *ring) {
    uint32_t write = ring->write_pos;
    uint32_t read = ring->read_pos;

    // Check if write has caught up to read
    uint32_t available = (write - read + ring->size) % ring->size;
    if (available < MIN_FREE_SPACE) {
        ring->overrun = true;
        stop_capture("overrun");
    }
}
```

### Recovery

```c
void handle_error(enum capture_error error) {
    // 1. Stop DMA
    dma_channel_abort(dma_chan);

    // 2. Stop PIO SMs
    pio_sm_set_enabled(pio, sm1, false);
    pio_sm_set_enabled(pio, sm2, false);

    // 3. Release arbiter
    capture_arbiter_release();

    // 4. Notify host
    send_error_event(error);
}
```

## Configuration

### Capture Parameters

```c
struct capture_config {
    // Channel configuration
    uint16_t channel_mask;      // Bitmask of enabled channels
    uint8_t channel_count;      // Number of enabled channels

    // Timing
    uint32_t sample_rate_hz;    // Requested sample rate
    uint32_t actual_rate_hz;    // Actual rate after clock calculation

    // Trigger
    uint8_t trigger_type;       // none/rising/falling/either
    uint8_t trigger_channel;    // Trigger pin (relative to channel_mask)

    // Pre/Post trigger (uint16, bounded 1..65535)
    uint16_t pre_samples;       // Pre-trigger sample count (0 in Web UI)
    uint16_t post_samples;      // Post-trigger sample count (0=unlimited/stream)

    // Output
    uint8_t compression;        // 0=none, 1=bit-pack, 2=RLE, 3=both
};
```

### Rate Calculation

```c
uint32_t calculate_clock_div(uint32_t requested_rate_khz) {
    uint32_t clk_sys = 125000;  // 125 MHz in kHz
    uint32_t div = (clk_sys + requested_rate_khz / 2) / requested_rate_khz;

    if (div < 1) div = 1;      // Max 125 MHz
    if (div > 65535) return 0;  // Rate too low

    return div;
}
```

**Example clock dividers**:
| Requested Rate | Clock Divider | Actual Rate |
|----------------|---------------|-------------|
| 125 MHz | 1 | 125 MHz |
| 500 kHz | 250 | 500 kHz |
| 100 kHz | 1250 | 100 kHz |

## Implementation Status

### What Works

- Dual PIO SM architecture (SM1 capture, SM2 trigger)
- Trigger detection at finite capture rates (100 kHz - 125 MHz)
- Continuous streaming through the 32 KiB RP2350 hardware write ring
- Signal verification (0x55 pattern correctly captured)
- Sigrok binary protocol over WebSocket (Web UI) and raw-TCP port 5556 (native)

### Known Limitations

- **125 MHz**: Finite burst capture only (512 samples internally), not sustained continuous transport
- **Pre-trigger**: Not exposed in Web UI; Web always sends `pre_samples=0`
- **GP7-GP9**: Not available in Sigrok modes

### Hardware Ring Notes

RP2350 continuous backend prefers the 32 KiB hardware DMA write ring described
in this document and retains legacy ping-pong streaming only as a fallback if
ring setup fails. See [ring-buffer-gap-analysis.md](ring-buffer-gap-analysis.md)
for the historical gap analysis and final post-ring HIL envelope.
