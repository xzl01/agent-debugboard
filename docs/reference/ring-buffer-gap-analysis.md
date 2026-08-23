# Ring Buffer Implementation Analysis

## Summary

**Goal**: Implement a DMA hardware ring buffer architecture (single channel + ring wrap + write_addr polling) for continuous logic analyzer capture.

**Current State**: RP2350 continuous path uses a 32 KiB direct DMA hardware write ring; ping-pong dual buffer remains as fallback if ring setup fails.

**Root Cause of Historical Issues**: Zephyr DMA driver conflicts with Pico SDK's `channel_config_set_ring`. The final repo-local implementation allocates DMA channel through Zephyr, configures the RP2350 ring using Pico SDK, and uses the official `dma_encode_endless_transfer_count()` encoding for `TRANS_COUNT` ENDLESS.

**Historical Baseline** (pre-ring): SINGLE 300 kHz / FAST8 235 kHz / WIDE12 135 kHz 5-second no-gap; adjacent failures at 400 kHz, 236 kHz, 136 kHz.

**Post-ring HIL Verified**: On the representative HIL setup with the final architecture and official RP2350 ENDLESS, the 32 KiB hardware ring achieved bounded 100 kHz / post=65535 for SINGLE, FAST8, and WIDE12 (each exactly 65535 samples, 0 gaps, restart true, HTTP health true). Continuous 5-second no-gap results: WebSocket SINGLE 1MHz verified (10 consecutive runs, ~4.991M-4.997M samples each, 998.16-998.70 ksps effective, zero sample-index gaps, zero disconnects, STOP response, immediate restart and HTTP health; 1MHz is a verified operating point, not a claimed absolute ceiling; adjacent WS failure not measured under the final architecture), FAST8 240 kHz, WIDE12 149 kHz; historical/reference raw-TCP SINGLE 443 kHz, FAST8 241 kHz, WIDE12 147 kHz. Adjacent or upper requests not measured under the final architecture; bounded captures at 100 kHz with post=65535 delivered exactly 65535 samples with zero gaps for all modes and transports.

**Not Claimed**: 125 MHz continuous streaming, watchdog fault-injection or automatic recovery validation. 125 MHz remains a finite hardware samplerate/burst capability, not a sustained network transport promise.

---

## Original Target Implementation

### DMA Ring Buffer Architecture

```
PIO SM1 (in pins, 32)
    │
    ▼
DMA Channel ──▶ Ring Buffer (32768 bytes, hardware wrap)
    │              │
    │              ▼
    │         write_addr polling (100µs)
    │              │
    │              ▼
    │         Software read + compress + send
    │
    └── Named long-period transfer count with polling and necessary renewal
```

**Key Features**:
- Single DMA channel + `channel_config_set_ring(true, 15)` hardware wrap
- Uses Pico SDK official `dma_encode_endless_transfer_count()` for ENDLESS encoding
- `dma_hw->ch[n].write_addr` polling for write position
- 0% CPU for DMA writes; consumer still needs work/timer polling, compression, and transport
- Natural support for pre-trigger (ring buffer stores history inherently)
- 125 MHz is a finite hardware samplerate, not a sustained network transport promise

---

## Legacy Implementation (Ping-Pong Fallback)

### Actual Architecture

```
PIO SM1 (in pins, 32)
    │
    ▼
DMA Channel ──▶ Buffer A (8KB) / Buffer B (8KB)
    │              │
    │              ▼
    │         DMA completion interrupt
    │              │
    │              ▼
    │         Callback reload DMA + submit work
    │              │
    │              ▼
    │         Work handler compress + send
    │
    └── Per 1024 samples interrupt, ~75µs overhead
```

**Fallback Features**:
- Ping-pong dual buffer + DMA callback reload
- Interrupt per 1024 samples
- ~6% CPU overhead
- Separate pre-trigger buffer (512 samples internally)
- Historical implementation does not represent final post-ring continuous ceiling

---

## Gap Analysis Table (Pre-Ring Baseline)

| Dimension | Target | Current | Gap |
|-----------|--------|---------|-----|
| Buffer size | 32768B (RP2350 max) | 16KB (2×8KB ping-pong) | 2× |
| DMA mode | Single channel + hardware ring wrap | Dual channel ping-pong + callback reload | Completely different |
| Transfer count | Named long-period or explicit endless encoding | 1024 (per reload) | Discontinuous |
| Ring wrap | `channel_config_set_ring(true, 15)` | None | Missing |
| CPU overhead | DMA writes 0%, consumer polling | ~6% | Higher |
| 125 MHz | Finite samplerate, continuous should stop on possible overrun | Burst 512 samples | Limited |
| Pre-trigger | Ring buffer natural support | Separate buffer 512 samples | Limited |
| Overrun detection | Hardware support | Software detection | Limited |

---

## Root Cause and Final Implementation

### Zephyr DMA Driver vs Pico SDK Conflict

1. **`dma_start()` overwrites configuration**
   - Zephyr's `dma_start()` calls `dma_channel_configure()` writing all registers
   - Cannot add ring wrap after `dma_start()`

2. **ISR conflict**
   - Zephyr DMA driver registers `dma_rpi_pico_isr` via `IRQ_CONNECT`
   - `irq_connect_dynamic` requires `CONFIG_DYNAMIC_INTERRUPTS`
   - `irq_set_exclusive_handler` panics (handler already exists)

3. **Channel allocation conflict**
   - Zephyr uses `dma_context.atomic` bitmap
   - Pico SDK uses `_claimed` bitmap
   - Both systems may allocate the same channel

4. **Zephyr DMA ISR disables interrupt**
   - `dma_rpi_pico_isr` disables channel interrupt after each block
   - Breaks continuous DMA

5. **Final repo-local solution**
   - Still allocates DMA channel through Zephyr to avoid dual allocation with Zephyr/Pico SDK bitmaps
   - Configures the allocated channel using Pico SDK `dma_channel_configure()`,
     `channel_config_set_ring(true, 15)`, and `dma_encode_endless_transfer_count()`
     for the 32768-byte RP2350 write ring
   - Does not rely on Zephyr block-complete IRQ reload; consumer work polls `write_addr`,
     uses elapsed-time/sequence guards to判断 continuity, and explicit terminal stop on
     definite/possible overrun

---

## Attempted Solutions

| Solution | Result | Reason |
|----------|--------|--------|
| Pico SDK `dma_channel_configure` + ring wrap | Crash | Conflicts with Zephyr DMA driver |
| Zephyr DMA API + `source_burst_length` hack | No data | `dma_start()` overwrites configuration |
| Modify `ctrl_trig` after `dma_start()` | Invalid | DMA already started, modification has no effect |
| `dma_claim_unused_channel` + Pico SDK | Crash | Conflicts with Zephyr channel allocation |
| Modify Zephyr DMA driver for ring wrap | CPU 3230% | DMA configuration abnormal |
| Zephyr `ring_buf` API | Crash | `ring_buf_get_finish` parameter error |
| `CONFIG_DYNAMIC_INTERRUPTS` + `irq_connect_dynamic` | Not tested | Requires further verification |

---

## Recommended Approach

### Maintain repo-local ring backend

Do not modify sibling Zephyr DMA driver to solve this repository's problem.

### Preserve explicit failure semantics

Requests exceeding the measured continuous envelope must explicitly terminate with
OVERRUN/ERROR terminal event while preserving HTTP health and immediate restart.

### Continue distinguishing capability types

Finite 125 MHz samplerate/HTTP burst capability is not the same as sustained
Sigrok/WebSocket continuous transport.

### If adjusting qdepth/rate cap/consumer cadence

Re-run the post-ring HIL envelope including bounded 65535, 5-second no-gap ceiling,
adjacent failures, HTTP BOOTSEL, and CDC ACM BOOTSEL.

---

## Final Post-Ring HIL Results (Final Architecture)

On the measured representative HIL setup, canonical sysbuild passed with FLASH
`655472/847832` (`77.31%`), RAM `511768/532480` (`96.11%`), and combined UF2
generated at `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`.

| Area | Verified Result |
|------|----------------|
| Combined UF2 / HTTP BOOTSEL | HTTP BOOTSEL flashing with combined UF2 and normal startup passed; HTTP BOOTSEL path repeatedly verified |
| WS bounded 100 kHz, post=65535 | SINGLE, FAST8, WIDE12 each received exactly 65535 samples, 0 gaps, restart true, HTTP health true, current CONFIG/START actual-rate ACKs correct |
| TCP bounded 100 kHz, post=65535 | SINGLE, FAST8, WIDE12 each received exactly 65535 samples, 0 gaps, restart true, HTTP health true |
| WS continuous 5 s no-gap operating points | WebSocket SINGLE 1MHz verified (10 consecutive 5-second runs, ~4.991M-4.997M samples each, 998.16-998.70 ksps effective, zero sample-index gaps, zero disconnects, STOP response, immediate restart and HTTP health; adjacent failure not measured under the final architecture); FAST8 240 kHz, WIDE12 149 kHz |
| Historical/reference TCP continuous 5 s no-gap ceilings | TCP SINGLE 443 kHz, FAST8 241 kHz, WIDE12 147 kHz |
| CDC ACM BOOTSEL fallback | `/dev/ttyACM2` `bootloader` entered ROM BOOTSEL as `/dev/sdc1` RP2350; reflashing combined UF2 recovered HTTP in 2 seconds |

These results are post-ring HIL facts for the final measured build. They do
not constitute watchdog fault-injection or automatic recovery verification.
