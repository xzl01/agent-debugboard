# Linkr Logic Analyzer Capture & Trigger Architecture

## Overview

Dual PIO state machine architecture for continuous capture with hardware trigger and pre-trigger support.

**Capture behavior**: All acquisitions capture continuously at the requested sample rate until a stop condition. There is no separate "burst" or "continuous" mode - the sample rate determines how long the ring buffer lasts before filling. Low rates (100 kHz) can stream for seconds; high rates (125 MHz) fill the buffer in milliseconds.

**Stop conditions** (same for all rates):
- Ring buffer overrun
- limit_samples reached
- Host STOP command
- Network failure

```
┌─────────────────────────────────────────────────────────┐
│                    GPIO Pins                             │
│  GP10 GP11 GP12 GP13 GP14 GP15 GP16 GP17               │
│  GP18 GP19 GP20 GP29                                    │
└─────────────────────────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────┐
│ PIO Block                                               │
│  ┌─────────────────────┐    ┌─────────────────────┐     │
│  │ SM1 (Capture)       │    │ SM2 (Trigger)       │     │
│  │ in pins, 32         │    │ wait_pin(trigger)   │     │
│  │ (autopush)          │    │ irq set 0           │     │
│  └─────────────────────┘    └─────────────────────┘     │
└─────────────────────────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────┐
│ DMA Controller                                          │
│  SM1 FIFO ──▶ Ring Buffer (98304 bytes)                 │
│  Continuous transfer, 0 CPU overhead                     │
└─────────────────────────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────┐
│ Ring Buffer                                             │
│  ┌─────────────────────────────────────────────────┐    │
│  │ pre_trigger_region │ post_trigger_region        │    │
│  │   (overwritable)   │   (protected after trigger)│    │
│  └─────────────────────────────────────────────────┘    │
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

**Continuous behavior**: All captures run continuously until stop condition, regardless of rate. Higher rates fill ring buffer faster.

### SM2: Trigger Detection

**Purpose**: Wait for trigger condition on specified pin, then signal via IRQ.

**Program** (2 instructions):

```c
static const uint16_t sm2_trigger_rising[] = {
    pio_encode_wait_gpio(true, trigger_pin_abs),   // Wait for high
    pio_encode_irq_set(0, false),                   // Set IRQ 0
};

static const uint16_t sm2_trigger_falling[] = {
    pio_encode_wait_gpio(false, trigger_pin_abs),   // Wait for low
    pio_encode_irq_set(0, false),                   // Set IRQ 0
};

static const uint16_t sm2_trigger_either[] = {
    pio_encode_jmp_pin(trigger_pin_abs),            // Jump if high
    pio_encode_wait_gpio(true, trigger_pin_abs),    // Wait for high
    pio_encode_irq_set(0, false),                   // Set IRQ 0
};
```

**Configuration**:
- `jmp_pin`: absolute trigger pin GPIO number (GP10-GP20, GP29)
- `wait_gpio`: uses absolute GPIO number
- `clock_div`: same as SM1 (synchronized)
- `wrap`: program loops until trigger fires

**Note for RP2350**: Use `wait_gpio` with absolute GPIO numbers, not `wait_pin`. On RP2350, `wait_pin` uses GPIO_BASE (default=0) for pin remapping, while `in pins` uses IN_BASE. Using `wait_gpio` with absolute GPIO numbers avoids this mismatch.

**Behavior**:
- Rising: waits for pin to go high, then triggers
- Falling: waits for pin to go low, then triggers
- Either: jumps if high (captures high→low), waits if low (captures low→high)
- After trigger: SM2 stops, IRQ 0 fires

## DMA Ring Buffer

### Memory Layout

```c
#define LINKR_DEBUGGER_LA_RING_BUFFER_BYTES (65536U)

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
channel_config_set_ring(&cfg, true, 16U);  // 64KB wrap (2^16)
channel_config_set_dreq(&cfg, pio_get_dreq(pio, sm1, false));

dma_channel_configure(dma_chan, &cfg,
    ring_buffer,
    &pio->rxf[sm1],
    UINT32_MAX,  // Continuous (never stops)
    false
);
```

### Write Position Tracking

The DMA write position is read directly from the hardware register:

```c
uint32_t dma_write_addr = dma_hw->ch[dma_channel].write_addr;
uint32_t buffer_start = (uint32_t)ring_buffer;
uint32_t dma_write_offset = (dma_write_addr - buffer_start) / sizeof(uint32_t);
write_pos = dma_write_offset % buffer_size_samples;
```

### Polling Mechanism

A periodic timer (100 µs interval) polls the DMA write position and processes available data:

```c
static struct k_timer ring_timer;

static void ring_timer_handler(struct k_timer *timer)
{
    if (ring_active) {
        k_work_submit(&ring_work);
    }
}

// In ring_handler:
uint32_t available = (write_pos - read_pos + buffer_size) % buffer_size;
while (available >= 1024U) {
    // Process chunk of 1024 samples
    // ...
    read_pos = (read_pos + 1024U) % buffer_size;
    available -= 1024U;
}
```

**Note**: DMA ring wrap requires power-of-2 buffer size. Use 128KB (2^17) for wrap, but only use first 98304 bytes. Software handles wrap-around for non-power-of-2 sizes.

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
   - Read pre-trigger samples from ring buffer
   - Continue reading post-trigger samples
   - Compress and send to host
   - CPU overhead: ~6%

5. STOP CONDITION
   - Ring buffer overrun
   - limit_samples reached
   - Host sends STOP command
   - Network failure
```

### Trigger Types

| Type | PIO Program | Behavior |
|------|-------------|----------|
| Rising | `wait_pin(true)` | Triggers on low→high transition |
| Falling | `wait_pin(false)` | Triggers on high→low transition |
| Either | `jmp_pin` + `wait_pin` | Triggers on any edge |
| None | (no SM2) | Continuous capture, no trigger |

### Trigger Channel Selection

Any safe GPIO can be used as trigger:

```c
// Safe trigger pins
static const uint8_t safe_trigger_pins[] = {
    7, 8, 9,                    // J13: CON_MAS, CON_REST, CON_USER
    10, 11, 12, 13, 14, 15,     // J16: GP10-GP15
    16, 17, 18, 19, 20,         // J16: GP16-GP20
    29                           // J16: GP29 (ADC3)
};
```

**Configuration**:
```c
sm2_config_set_jmp_pin(&sm2_config, trigger_pin);
sm2_config_set_wait_pin(&sm2_config, trigger_pin);
```

## Pre-trigger Capture

### Mechanism

```
Ring Buffer State:

Before trigger:
┌─────────────────────────────────────────────────────────┐
│ write_pos advances →                                    │
│ Old data is overwritten                                 │
│ Trigger condition checked on each sample                │
└─────────────────────────────────────────────────────────┘

At trigger:
┌─────────────────────────────────────────────────────────┐
│ trigger_pos marked at current write_pos                 │
│ triggered = true                                        │
└─────────────────────────────────────────────────────────┘

After trigger:
┌─────────────────────────────────────────────────────────┐
│ Pre-trigger data │ Post-trigger data                    │
│ (ring history)   │ (new captures)                       │
└─────────────────────────────────────────────────────────┘
```

### Output Sequence

```c
void output_capture(struct capture_ring *ring, uint32_t pre_samples) {
    // 1. Calculate pre-trigger start position
    uint32_t pre_start = (ring->trigger_pos - pre_samples + ring->size) % ring->size;
    
    // 2. Output pre-trigger samples
    for (uint32_t i = 0; i < pre_samples; i++) {
        uint32_t sample = ring->buffer[(pre_start + i) % ring->size];
        send_compressed(sample);
    }
    
    // 3. Output post-trigger samples
    uint32_t pos = ring->trigger_pos;
    while (still_capturing()) {
        uint32_t sample = ring->buffer[pos % ring->size];
        send_compressed(sample);
        pos++;
        
        // Check stop conditions
        if (pos - ring->trigger_pos >= limit_samples) break;
        if (ring->overrun) break;
    }
}
```

### Pre-trigger Depth

Limited by ring buffer size (65536 bytes = 16384 samples at 32-bit):

| Mode | Ring Buffer | Max Pre-trigger |
|------|------------|-----------------|
| 32-bit raw | 65536 bytes | 16384 samples |
| bit-pack 8ch | 65536 bytes | 16384 samples |

**Example**: At 100 kHz 32-bit, max pre-trigger = 16384 samples = 0.16 seconds

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

**Note**: At high rates (>10 MHz), CPU overhead is minimal during capture. CPU only active during data output.

### Latency

| Event | Latency | Notes |
|-------|---------|-------|
| Trigger detection | 1-6 cycles | PIO hardware |
| Timer poll | 100 µs | Periodic polling |
| Data output start | ~100 cycles | Software setup |
| Sample delivery | 1 cycle/sample | PIO rate (autopush) |

### Throughput

| Mode | Max Rate | Ring Buffer Duration | Notes |
|------|----------|---------------------|-------|
| Fast8 (1-8 channels) | 125 MHz | ~0.79 ms | Continuous until overrun |
| Wide12 (1-12 channels) | 125 MHz | ~0.79 ms | Continuous until overrun |
| Low-rate (any mode) | 100 kHz | ~983 ms | Long continuous streaming |
| Network output | ~10 Mbps | - | USB NCM bandwidth |
| Ring buffer | 500 MB/s | - | DMA throughput |

**All modes support continuous capture.** Higher rates fill the buffer faster, but the capture behavior is identical.

## Resource Usage

### PIO Resources

| Resource | Used | Available | Notes |
|----------|------|-----------|-------|
| PIO SM | 2 | 8 | SM1 + SM2 |
| Instructions | 3 | 32 | 1 (capture) + 2 (trigger) |
| GPIO pins | 12 | 29 | GP10-GP20, GP29 |

### DMA Resources

| Resource | Used | Available | Notes |
|----------|------|-----------|-------|
| DMA channel | 1 | 16 | Single channel |
| DMA IRQ | 0 | 16 | Polling mode |

### Memory Resources

| Resource | Size | Notes |
|----------|------|-------|
| Ring buffer | 65536 bytes | Static allocation |
| DMA config | 64 bytes | Runtime |
| PIO program | 2 bytes | 1 instruction |
| Timer | 32 bytes | 100 µs polling |

## Stop Conditions

| Condition | Trigger | Action |
|-----------|---------|--------|
| limit_samples reached | Sample count | Normal stop |
| Ring buffer overrun | write_pos catches read_pos | Error stop |
| Host STOP command | TCP message | Normal stop |
| Network failure | Send error | Error stop |
| DMA error | DMA callback | Error stop |

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
    
    // Pre/Post trigger
    uint32_t pre_samples;       // Pre-trigger sample count
    uint32_t post_samples;      // Post-trigger sample count (0=unlimited)
    
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

**Clock divider precision**: PIO only supports integer clock dividers. Fractional rates are rounded to the nearest achievable rate. The actual rate is always `125 MHz / clock_div`.

## Testing

### Unit Tests

```c
// Test 1: Ring buffer wrap-around
test_ring_buffer_wrap() {
    // Fill ring buffer completely
    // Verify oldest data is overwritten
    // Verify write_pos wraps correctly
}

// Test 2: Trigger detection
test_trigger_detection() {
    // Configure rising edge trigger
    // Feed samples with edge at known position
    // Verify trigger_pos matches expected position
}

// Test 3: Pre-trigger output
test_pre_trigger_output() {
    // Set pre_samples = 100
    // Trigger at position 1000
    // Verify output contains samples 900-1000 (pre) + 1000+ (post)
}

// Test 4: Overrun detection
test_overrun_detection() {
    // Fill ring buffer without reading
    // Verify overrun flag is set
    // Verify capture stops
}
```

### Integration Tests

```c
// Test 5: Full capture flow
test_full_capture() {
    // Configure capture with trigger
    // Start capture
    // Generate trigger signal
    // Verify pre-trigger + post-trigger data received
    // Verify no samples missing
}

// Test 6: Continuous capture
test_continuous_capture() {
    // Configure unlimited capture
    // Run for 10 seconds
    // Verify continuous data flow
    // Verify no overrun
}
```

## Future Extensions

### Multi-trigger

Support multiple trigger conditions (AND/OR):

```c
struct multi_trigger {
    uint8_t condition_count;
    struct {
        uint8_t pin;
        uint8_t type;  // rising/falling/either
    } conditions[MAX_TRIGGERS];
    uint8_t logic;  // AND/OR
};
```

### Trigger Qualifiers

Add additional conditions before trigger:

```c
struct trigger_qualifier {
    uint32_t min_samples;  // Minimum samples before trigger valid
    uint32_t max_time_us;  // Maximum time window
    uint16_t pattern_mask; // Pattern match mask
    uint16_t pattern_value;// Pattern match value
};
```

### Multi-channel Trigger

Trigger on pattern across multiple channels:

```c
struct pattern_trigger {
    uint16_t channel_mask;   // Channels to monitor
    uint16_t pattern;        // Pattern to match
    uint16_t pattern_mask;   // Don't care bits
    uint8_t edge_type;       // Rising/falling/either on pattern change
};
```

## Current Implementation Status

> **Note**: The current implementation uses ping-pong DMA buffers instead of the
> hardware ring buffer described in this document. See
> [ring-buffer-gap-analysis.md](ring-buffer-gap-analysis.md) for detailed gap
> analysis and attempted solutions.

### What Works

- Dual PIO SM architecture (SM1 capture, SM2 trigger)
- Trigger detection at all rates (100 kHz - 125 MHz)
- Continuous streaming at 100 kHz - 100 MHz
- Signal verification (0x55 pattern correctly captured)
- Pre-trigger capture via software circular buffer

### What Doesn't Match This Document

- DMA uses ping-pong buffers (2×8KB) instead of single ring buffer (64KB)
- DMA callback re-arms every 1024 samples (~6% CPU overhead)
- 125 MHz limited to burst capture (512 samples), not continuous
- Pre-trigger uses separate buffer (4096 samples), not ring buffer
- No hardware ring wrap (`channel_config_set_ring` not used)
- `write_addr` polling not implemented (uses DMA callback instead)

### Root Cause

Zephyr's DMA driver conflicts with Pico SDK's `channel_config_set_ring`:
- `dma_start()` overwrites all DMA registers
- Zephyr DMA ISR disables channel IRQ after each block
- Pico SDK and Zephyr use independent channel allocation bitmaps
