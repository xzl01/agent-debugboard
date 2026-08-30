# Linkr Logic Analyzer Capture and Trigger Architecture

## Overview

The firmware logic analyzer uses RP2350 PIO2+DMA for high-speed GPIO capture.
The sigrok binary protocol runs over two transports:

- **Web UI**: Creates live session via `/api/v1/live-sessions`, then speaks
  sigrok binary protocol over `/api/v1/ws/<slot>` WebSocket
- **Native sigrok**: Connects via raw-TCP port 5556

The capture engine uses two engines: packed finite DMA and packed DMA ring.
SINGLE/FAST8 uses one 32768B packed wrap ring; WIDE11 uses lane A 16384B plus lane B
8192B inside the same 32768B slice, with a common capacity of 16384 samples and a safety
margin of 2048. Usable capacities are SINGLE 260096, FAST8 30720, and WIDE11 14336 samples.
The WIDE11 writer advances at the minimum completed lane sequence. DMA write-pointer
skew is diagnostic because joined RX FIFOs and in-flight DMA make snapshots non-atomic. The
sticky PIO RXSTALL flag, cleared at arm, is the authoritative lane-stall signal. On overrun, possible overrun, transport backpressure, or bounded-capture
completion, the firmware emits a terminal event and stops rather than silently continuing
(lossless-or-stop). The packed ring reuses the 148856 B total backing allocation (sized to max(normal, burst)=148856 B); the WIDE11 144184 B hardware slice and the 30720 B WS telemetry ring share that backing allocation.
DATA and EVENT sample indices wrap modulo 24 bits; wrap is not a terminal condition.

After an overrun terminal is requested, sink retries yield CPU time so the
lower-priority TCP or WebSocket transport can release queue capacity. If the
sink remains unavailable for one second, the already-invalid remaining ring
data is abandoned so the terminal can complete. Producer and consumer idle
waits are also bounded to two seconds, preventing a capture failure from
permanently blocking cleanup. Raw TCP connections use abortive close after the
session finishes so stale queued data cannot hold the single accept worker.

The terminal selection freezes the writer (trigger SM, sampler SM(s), and ring
DMA channel(s)) before the consumer drains the ring, so a terminal can never
read samples that the producer is still allowed to overwrite. See
"Terminal-Freeze Policy" below for the full sequence.

**Stop conditions** (same for all rates):
- Ring buffer overrun or possible overrun — lossless-or-stop, writer is frozen first
- limit_samples reached (bounded mode) — lossless-or-stop
- Host STOP command — user-initiated normal stop
- Network failure — lossless-or-stop, writer is frozen first

**Lossless-or-stop capacity outcomes**

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

A zero-DATA capacity stop (no DATA frames, only a STOPPED/OVERRUN event) is
labeled `capacity_stop_before_data`. It is a capacity-stop diagnostic, not a
continuous-streaming pass: it shows the firmware stopped cleanly before the
consumer saw any data, which is what the lossless-or-stop contract requires,
but it is not a sustained-throughput claim. Rows that observe an immediate
`sample_index=0` OVERRUN or STOPPED with `data_frames=0` must keep this label.

```
LA Pins: GP10-GP20; GP29 = ADC3 voltage monitor (ADC3-owned, input-only)
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
│  SM1 FIFO ──▶ Packed DMA ring (32768 bytes max)         │
│  Continuous transfer, 0 CPU overhead                     │
│  Two engines: packed finite DMA / packed DMA ring        │
└─────────────────────────────────────────────────────────┘
                    │
                    ▼
┌─────────────────────────────────────────────────────────┐
│ Packed Ring Buffer                                      │
│  SINGLE/FAST8: 32768B packed wrap ring                  │
│  WIDE11: lane A 16384B + lane B 8192B in 32768B slice │
│  Common capacity 16384 samples, safety 2048              │
│  Usable: SINGLE 260096 / FAST8 30720 / WIDE11 14336    │
│  WIDE11: min lane sequence, PIO RXSTALL stops          │
└─────────────────────────────────────────────────────────┘
                    │
                    ▼
┌─────────────────────────────────────────────────────────┐
│ Software (IRQ Handler)                                  │
│  IRQ 0 → mark_trigger_pos → start_output                │
│  On overrun/pressure/completion: terminal + stop         │
│  Lossless-or-stop policy: no silent sample drops        │
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
The capture engine uses two engines: packed finite DMA and packed DMA ring.
SINGLE/FAST8 uses one 32768-byte packed wrap ring; WIDE11 uses lane A 16384B plus lane B 8192B
inside the same 32768B slice, with a common capacity of 16384 samples and safety margin of 2048.

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
trigger, there is an arm-time race window.

Bounded captures with pre=0 and post>512 use ring streaming. At negotiated high rates
with GENERIC_PACKED_BURST, post=0 uses packed burst (exactly 100000 samples, then
auto-STOP/drain), not continuous streaming.

Continuous captures with post=0:
- At supported high rates (100/125 MHz for SINGLE/FAST8, 100 MHz for WIDE11) with GENERIC_PACKED_BURST: captures exactly 100000 samples losslessly, then auto-STOP/drain
- At lower rates: runs until host STOP (ring streaming)

## Performance Characteristics

### CPU Usage

| Component | CPU behavior | Notes |
|-----------|--------------|-------|
| Sampler SM(s) | Hardware | PIO capture; sticky RXSTALL reports a real full-RX-FIFO stall |
| Trigger SM | Hardware | No CPU edge polling |
| DMA transfer | Hardware | ENDLESS write ring with address polling |
| Ring observation | Rate-dependent | Priority-1 poller, bounded 1 to 4 ms interval |
| Decode/compression | Data-dependent | Exact FAST8/WIDE11 paths plus RLE, PACKED_PALETTE2, or raw fallback |
| Network send | Transport-dependent | WS fixed pool or TCP dynamic queue capped at 10 frames |
| Total | Measured by HIL | Use the dated static and activity-qualified operating points, not a fixed percentage |

### Throughput

| Mode | Max Rate | Buffer Duration | Usable Capacity | Notes |
|------|----------|----------------|-----------------|-------|
| SINGLE/FAST8 | 125 MHz finite sampler | ~0.066 ms at 125 MHz | 260096 / 30720 samples | Stop on possible overrun; packed ring |
| WIDE11 | 100 MHz finite sampler | ~0.066 ms at 100 MHz | 14336 samples | Min lane sequence; PIO RXSTALL or overwrite stops |
| Low-rate (any mode) | 100 kHz | ~81.9 ms | Packed ring | Longest continuous margin |

### Measured No-Gap Continuous Operating Points

On the representative HIL setup with the final architecture and official RP2350 ENDLESS:

| Path | Verified Result |
|------|----------------|
| WS bounded 100 kHz, post=65535 | Protocol-v2 SINGLE received exactly 65,535 samples in 32 DATA frames with 0 gaps/errors/overruns; STOP, restart, HTTP health |
| TCP bounded 100 kHz, post=65535 | Protocol-v2 SINGLE received exactly 65,535 samples in 32 DATA frames with 0 gaps/errors/overruns; STOP, restart, HTTP health |
| WS SINGLE continuous 5 s | 16 MHz passed 10/10: 78,004,224–79,855,616 samples, 15.600–15.969 Msamples/s, zero gaps/errors/overruns, STOP, restart, HTTP health |
| TCP SINGLE continuous 5 s | 16 MHz passed 10/10: 77,266,944–79,937,536 samples, 15.451–15.986 Msamples/s, zero gaps/errors/overruns, STOP, restart, HTTP health |
| Adjacent measured boundary | 18.002 MHz failed the 95% negotiated-sample-count rule on both WS and TCP while retaining clean STOP/restart/health and zero gaps/overruns |
| FAST8 static/low-transition continuous | WS 2.600 MHz and TCP 2.600 MHz passed 10/10 |
| WIDE11 static/low-transition continuous | WS 1.050 MHz and TCP 1.175 MHz passed 10/10 |
| FAST8 GP16 active with PACKED_PALETTE2 | WS 1.200 MHz and TCP 1.375 MHz passed 10/10; 1.225/1.400 MHz adjacent cases fail |
| WIDE11 GP16 active with PACKED_PALETTE2 | WS 850 kHz and TCP 950 kHz passed 10/10; 875/975 kHz adjacent cases fail |
| Raw matched-baud GP16 activity before PALETTE2 | FAST8 500 kHz and WIDE11 250 kHz passed both transports; 550/300 kHz failed |

Protocol v2 packs eight chronological SINGLE samples per byte and uses
16,384-sample chunks without increasing arena or slot RAM. The SINGLE 16 MHz input was
static/low-transition, so incompressible SINGLE remains unverified. The multichannel active
profile continuously drives GP16 with UART 0x55 at 921600 baud and verifies low, high, and
transition counts in decoded samples. It is one active line inside the full channel set, not
independent entropy on every channel. See the
[2026-08-30 protocol-v2 dense HIL](../testing/results/2026-08-30-logic-analyzer-v2-dense-hil.md)
and [2026-08-31 multichannel HIL](../testing/results/2026-08-31-logic-analyzer-v2-multichannel-hil.md).
The table does not claim 125 MHz continuous streaming or watchdog fault injection.

### Generic Packed Burst Architecture

The generic packed-burst architecture uses a common packed arena distinct from
the 32 KiB ring path. SINGLE and FAST8 use one capture SM; WIDE11 uses two
capture SMs. For WIDE11:

- **SM-A** (capture): GP10-GP17, 8-bit autopush32, 100000 B source buffer
- **SM-B** (capture): GP18-GP20, 3-bit autopush30, 40000 B source buffer
- **Shared burst slice**: 144184 B; overlays the 148856 B total backing allocation (sized to max(normal, burst)=148856 B); temporarily removes ADC telemetry, power capture, and normal Sigrok pool resources for the lease lifetime; restores after drain
- **Post-capture transport**: up to 98 DATA frames, maximum 1024 samples per frame,
  140000 B total payload
- **Two-phase START**: ownership and quiesce are ready before the response. NONE
  sends START_RESP in RUNNING state with no ARMED event; triggered captures send
  START_RESP in ARMED state followed by the ARMED event. GO then synchronously
  enables the capture SMs

The three physical capture plans:
- **SINGLE**: one 1-bit lane on FAST8 SM (GP10 default), autopush32, 32 samples per 32-bit word,
  12500 B source at 100 MHz (100000 samples × 1 bit). A FAST8 physical plan, not a separate Sigrok wire mode.
- **FAST8**: one 8-bit lane (GP10-GP17), autopush32, 4 samples per 32-bit word,
  100000 B source at 100 MHz (100000 samples × 8 bits)
- **WIDE11**: two capture SMs: SM-A (GP10-GP17, 8-bit autopush32, 100000 B) and SM-B (GP18-GP20, 3-bit autopush30, 40000 B); two DMA channels; 144184 B burst slice (overlays the 148856 B total backing allocation); triggered deep burst adds a third trigger-only SM

GP29 is excluded from WIDE11 LA (ADC3 voltage monitor; ADC3-owned and input-only). WIDE11
verification is the freeze-final HIL evidence block below; it is **not** the same
artifact as the historical WIDE12 baseline.

**Historical WIDE12 baseline (not current WIDE11; retained for context only)**:
SM-A (GP10-GP20, 11-bit autopush22, 200000 B) plus SM-B (GP29, 1-bit autopush32,
12500 B); two DMA channels, 212500 B total source; 216684 B shared arena. See
`docs/testing/results/2026-07-26-logic-analyzer-wide12-100k-hil.md` for the WIDE12
historical evidence.

HELLO server_flags bit 0 advertises CONFIG_V2 capability and bit 1 advertises GENERIC_PACKED_BURST.
The client must use frame0x0b (CONFIG_V2_REQ, 16B) with u32LE pre/post fields only
for bounded post > 65535. The v1 frame0x05 (12B) remains for bounded captures with
post <= 65535 and the post=0 stream sentinel; bit1 determines whether supported
high-rate post=0 captures exactly 100000 samples and then auto-STOP/drains.
Bounded `post=65536..100000` is rate-neutral within the otherwise supported
physical plan limits when CONFIG_V2 and GENERIC_PACKED_BURST are negotiated.

#### Terminal-Freeze Policy

When the writer is still active and the firmware picks a non-OK terminal cause
(OVERRUN, possible overrun, transport backpressure, ERROR, or any other writer-
side terminal), the freeze policy runs **before** the consumer drains the ring so
the consumer never reads samples the producer is still allowed to overwrite. The
freeze sequence for the packed ring path is:

1. Disable the trigger SM (existing).
2. Disable sampler SM A; drain its RX FIFO to a clean state. In dual-lane modes
   (WIDE11), disable sampler SM B and drain its RX FIFO as well.
3. Abort ring DMA channel A (single lane) or channels A and B (dual lane):
   disable the DMA IRQ, clear the EN bit, call `dma_channel_abort`, then clear
   the DMA IRQ. Channel ownership is **retained** — neither DMA channel is
   released back to the pool, and `la_packed_burst_release_dma_locked()` is not
   called.
4. Drain only the already-committed ring data through the normal consumer path.

The pure policy helper `linkr_debugger_logic_analyzer_ring_freeze_policy(lane_count)`
returns the bitmask of hardware to stop for the requested lane count (1 vs 2), so
host unit tests can verify the freeze policy without runtime state. The freeze
itself remains idempotent: it checks the `acquisition_frozen` guard first so a
second terminal that arrives while the writer is already frozen is a no-op.

This avoids the failure mode where overrun or error servicing kept hardware alive
long enough that the producer overwrote ring samples the consumer was still
draining. Sample indices remain contiguous in that regime, so a future
sample-index-gap check cannot detect content corruption by itself — the
freeze-before-drain contract is what eliminates the hidden corruption, not the
gap check.

#### Historical 2026-07-27 freeze-final HIL evidence

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
  or simultaneous dual-lane transitions**. See
  `docs/testing/hil-functional-test-spec.md` and the SKILL before claiming
  full WIDE11 high-state mapping.
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

**High-rate `post=0` capacity-burst matrix**:

| Mode | Rate | pre | post | Notes |
|------|------|-----|------|-------|
| SINGLE | 100 MHz or 125 MHz | 0 | 0 | Captures exactly 100000 samples losslessly then auto-STOP/drain |
| FAST8 | 100 MHz or 125 MHz | 0 | 0 | Captures exactly 100000 samples losslessly then auto-STOP/drain |
| WIDE11 | 100 MHz only | 0 | 0 | Captures exactly 100000 samples losslessly then auto-STOP/drain. 125 MHz is rejected by START (INVALID_CONFIG) |

## Resource Usage

### PIO Resources

| Resource | Used | Available | Notes |
|----------|------|-----------|-------|
| PIO SM | 2–3 | 8 | SM1 for finite/ring modes; WIDE11 deep burst: NONE uses 2 capture SMs (SM-A and SM-B), triggered adds a 3rd trigger-only SM |
| Instructions | 4 max | 32 | 1 capture + 3 trigger instructions; EITHER uses the same 3-instruction path after firmware level-snapshot |
| GPIO pins | 11 | 29 | GP10-GP20 (GP29 excluded from LA; ADC3 voltage monitor; ADC3-owned and input-only) |

### DMA Resources

| Resource | Used | Available | Notes |
|----------|------|-----------|-------|
| DMA channel | 2 | 16 | Two channels for WIDE11 deep burst (SM-A and SM-B lanes); single channel for finite/ring modes |
| DMA IRQ | 0 | 16 | Polling mode |

### Memory Resources

| Resource | Size | Notes |
|----------|------|-------|
| Packed ring (SINGLE/FAST8) | 32768 bytes | One 32768B packed wrap ring; usable SINGLE 260096 / FAST8 30720 samples |
| Packed ring (WIDE11) | 32768 bytes | Lane A 16384B + lane B 8192B; common capacity 16384 samples; safety 2048; usable 14336 samples; min lane sequence; PIO RXSTALL or overwrite stops |
| Stream scratch | 2048 bytes | Shared synchronous compression output |
| Normal Sigrok WS pool | 16624 bytes | Four 4,113-byte frame slots plus one terminal slot and pool metadata; 192 bytes smaller than the former layout |
| TCP dynamic queue | 10 DATA frames max | Terminal-reserved cap prevents the 49,156-byte heap from reaching the measured connection-stalling exhaustion point |
| DMA config | 64 bytes | Runtime |
| WIDE11 deep burst slice | 144184 bytes | Dual-SM packed burst slice; overlays the 148856 B total backing allocation (sized to max(normal, burst)=148856 B); quiesces LA/Sigrok pool during lease; GP29 excluded from LA (ADC3 voltage monitor; ADC3-owned and input-only) |

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
- Two capture engines: packed finite DMA and packed DMA ring
- Packed ring architecture: SINGLE/FAST8 one 32768B packed wrap ring, WIDE11 lane A 16384B plus lane B 8192B in same 32768B slice
- Common capacity 16384 samples, safety 2048; usable capacities SINGLE 260096 / FAST8 30720 / WIDE11 14336
- WIDE11 writer advances at min lane sequence; DMA skew is diagnostic and sticky PIO RXSTALL detects actual FIFO stall
- Lossless-or-stop policy: on overrun, possible overrun, transport backpressure, or bounded-capture completion, firmware emits terminal and stops
- Terminal-freeze policy: writer freezes the trigger SM, the sampler SM(s), and the ring DMA channel(s) before the consumer drains; channel ownership is retained
- Signal verification (0x55 pattern correctly captured)
- Sigrok binary protocol over WebSocket (Web UI) and raw-TCP port 5556 (native)
- Generic matrix 54/54 pass (`logic-analyzer-wide11-packed-all-freeze-final.json`)
- High-rate matrix 62/62 pass (`logic-analyzer-high-rate-packed-burst-freeze-final.json`)
- WIDE11 exact 100000/98 DATA / zero gaps at requested 100 MHz
- WIDE11 mapping at `channel_mask=0x07FF` with GP10 UART stimulus (other pins held low)
- WIDE11 telemetry isolation: arena lease pauses ADC/telemetry cleanly and resumes after drain

### Known Limitations

- **125 MHz**: Finite burst capture only (512 samples internally), not sustained continuous transport
- **Above 16 MHz sustained SINGLE streaming**: the adjacent 18.002 MHz point misses the strict negotiated-sample-count threshold; 25/50 MHz remain boundary diagnostics, not operating points
- **Worst-case 16 MHz SINGLE transport**: only static/low-transition SINGLE_BITS_RLE input passed; incompressible/high-transition capacity remains unverified
- **Pre-trigger**: Not exposed in Web UI; Web always sends `pre_samples=0`
- **GP7-GP9**: Not available in Sigrok modes
- **WIDE11 high-state mapping**: the freeze-final mapping HIL drives GP10 with a UART stimulus and holds GP11–GP20 low; it does not demonstrate high-state mapping on GP18–GP20 or simultaneous dual-lane transitions
- **Watchdog rollback HIL**: not covered by the freeze-final envelope; do not claim watchdog-fault validation for the LA freeze-before-drain path from these files

### Hardware Ring Notes

The capture engine uses two engines: packed finite DMA and packed DMA ring.
SINGLE/FAST8 uses one 32768B packed wrap ring; WIDE11 uses lane A 16384B plus lane B 8192B
inside the same 32768B slice, common capacity 16384 samples, safety 2048, usable capacities
SINGLE 260096 / FAST8 30720 / WIDE11 14336. The packed ring reuses the 148856 B total backing
allocation (sized to max(normal, burst)=148856 B); the WIDE11 144184 B hardware slice and the
30720 B WS telemetry ring share that backing allocation. The WIDE11 writer advances at the minimum completed lane
sequence. DMA write-pointer skew is diagnostic; sticky PIO RXSTALL or ring overwrite is
terminal. See [ring-buffer-gap-analysis.md](ring-buffer-gap-analysis.md)
for the historical gap analysis and final post-ring HIL envelope.
