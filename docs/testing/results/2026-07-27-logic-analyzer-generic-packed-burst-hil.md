# Logic Analyzer Generic Packed Burst HIL Evidence — 2026-07-27

## Summary

| Field | Value |
|---|---|
| Authoritative TCP/WS matrix | `54/54` pass |
| High-rate packed-burst matrix | `62/62` pass |
| Explicit continuous capacity stops | 18 |
| `capacity_stop_before_data=true` | 4 |
| High-rate exact 100000-sample passes | 20 |
| WIDE11 reduced mapping | pass |
| WIDE11 telemetry isolation | pass |

## Build Artifact

Canonical build used for this validation:

- **UF2**: `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`
- **FLASH**: 695868 / 847832 bytes used (82.08%)
- **RAM**: 475832 / 532480 bytes used (89.36%)
- **Heap**: 49156 bytes
- **Combined UF2 size**: 1443840 bytes

Combined UF2 (MCUboot + app) was flashed via HTTP BOOTSEL command.
CDC ACM shell `bootloader` path also confirmed.

## Hardware Wiring

- **Stimulus**: `/dev/ttyACM1` (WCH serial, 115200 8N1) TX → GP10
- **Ground**: shared between host and Linkr Debugger
- **GP11-GP20**: unconnected, expected-low

The reduced single-wire setup validates only GP10 DATA bit 0 activity and
absence of high/crosstalk on bits 1..10. It does **not** validate independent
high-state mapping for GP11-GP20 or simultaneous lane-B transition alignment.
GP29 is excluded from LA and remains ordinary GPIO/ADC3.

## Physical Lane Facts (verified by design, confirmed by HIL)

| Plan | PIO config | Source bytes at 100 MHz | Samples @ 1-bit / 8-bit |
|---|---|---|---|
| **SINGLE** | 1-bit lane, autopush32, 32 samples/word | 12500 B | 100000 × 1 bit |
| **FAST8** | 8-bit lane, autopush32, 4 samples/word | 100000 B | 100000 × 8 bits |
| **WIDE11** | SM-A GP10-GP17 8-bit autopush32 100000 B + SM-B GP18-GP20 3-bit autopush30 40000 B | 140000 B | 100000 × 11 bits |

## Matrix Coverage

The runner covered the following cases (all transports: TCP and WebSocket):

### HELLO capability flags
- TCP and WS: `server_flags=3` (CONFIG_V2 + GENERIC_PACKED_BURST), both advertised

### Bounded captures — v1 frame0x05 (CONFIG_V2 not required)

| Mode | Rate | post | Triggers | Transport |
|---|---|---|---|---|
| SINGLE | 100 MHz | 513 | NONE / rising / EITHER | TCP + WS |
| SINGLE | 100 MHz | 65535 | NONE / rising / EITHER | TCP + WS |
| SINGLE | 125 MHz | 513 | NONE / rising / EITHER | TCP + WS |
| SINGLE | 125 MHz | 65535 | NONE / rising / EITHER | TCP + WS |

### Bounded captures — frame0x0b CONFIG_V2 (CONFIG_V2 required, post > 65535)

| Mode | Rate | post | Triggers | Transport |
|---|---|---|---|---|
| SINGLE | 100 MHz | 65536 | NONE / rising / EITHER | TCP + WS |
| SINGLE | 100 MHz | 100000 | NONE / rising / EITHER | TCP + WS |
| SINGLE | 125 MHz | 65536 | NONE / rising / EITHER | TCP + WS |
| SINGLE | 125 MHz | 100000 | NONE / rising / EITHER | TCP + WS |

### FAST8 sparse (GP10, GP13, GP17 selected)

| Mode | Rate | post | Transport |
|---|---|---|---|
| FAST8_SPARSE | 100 MHz | 100000 | TCP + WS |
| FAST8_SPARSE | 125 MHz | 100000 | TCP + WS |

### WIDE11 dual-lane

| Mode | Rate | post | Transport | Expected result |
|---|---|---|---|---|
| WIDE11 | 100 MHz | 100000 | TCP + WS | pass — exactly 100000 samples, 98 DATA frames, 0 gaps |
| WIDE11 | 125 MHz | 100000 | TCP + WS | **expected rejection** (INVALID_CONFIG) |

### post=0 capacity captures

| Mode | Rate | post | Transport | Expected result |
|---|---|---|---|---|
| SINGLE | 100 MHz | 0 | TCP + WS | exactly 100000 samples, server_auto_stopped |
| SINGLE (lower rate) | 1 MHz | 0 | TCP + WS | explicit lossless capacity OVERRUN on the representative build |

The 54-case authoritative matrix treats an explicit OVERRUN as a successful
lossless-or-stop contract outcome only when there are no gaps, decode errors,
disconnects, restart failures, or HTTP health failures. Eighteen rows ended this
way; four stopped before first DATA and are explicitly labeled
`capacity_stop_before_data=true`. These rows are not sustained-throughput claims.

### WIDE11 reduced mapping

The sequential reduced mapping run used `/dev/ttyACM1` TX -> GP10 at 115200 8N1
with GP11-GP20 left low. It captured exactly 100000 samples in 98 DATA frames at
100 MHz with zero gaps and zero decode errors. Across 8192 checked samples, GP10
showed both levels and nine transitions; the GP11-GP20 zero mask had no
violations. This is reduced single-wire evidence only.

### Shared-arena telemetry isolation

During a raw-TCP WIDE11 100 MHz/post=100000 burst, the JSON WebSocket telemetry
client remained connected with zero binary contamination, malformed JSON, or
old-epoch post-grace samples. The burst delivered exactly 100000 samples in 98
DATA frames with zero gaps. Telemetry resumed in a reset sequence epoch with
advancing device time, and the post-capture ADC HTTP health check passed.

## Failure and Fix History

1. A requested 125 MHz SINGLE/FAST8 capture reports a quantized actual ACK of
   125081 kHz on this board. Packed-plan eligibility now applies the product
   limit to the requested rate and requires only that the actual rate is nonzero;
   this prevents a valid 125 MHz request from falling back to the ring path.
2. Continuous transport pressure exposed that a terminal-aware client must stop
   reading on STOPPED/OVERRUN/ERROR instead of waiting for the later idle close.
   TCP and WebSocket HIL paths now preserve and report the terminal reason.
3. Terminal selection now disables the trigger and active sampler SMs and aborts
   ring DMA A/B before draining already committed ring data. DMA channels remain
   owned until common cleanup, so hardware cannot overwrite the drain window.
4. The final freeze build passed the 54/54 and 62/62 matrices.

## Explicit Limitations

The following are **not** verified by this HIL and remain unverified:

- **GP11-GP20 independent high-state mapping**: unconnected pins are expected-low only in this HIL. Independent high-state behavior and simultaneous dual-lane transitions are not validated without the external generator.
- **WIDE11 at 125 MHz**: rejected by START with INVALID_CONFIG; not a HIL failure — this is the documented contract.
- **Full FAST8 8-channel capture at high rate**: only sparse FAST8 (GP10, GP13, GP17 selected) was tested; full 8-channel GP10-GP17 continuous high-rate is not covered.
- **Physical mapping beyond GP10 bit 0**: the single-wire stimulus only exercises GP10 DATA bit 0; bits 1..10 are passively expected-low.

## Flash and Boot Paths Confirmed

- **HTTP BOOTSEL**: confirmed through `GET /api/v1/bootloader` using combined UF2 `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`
- **CDC ACM shell `bootloader`**: confirmed

Both paths use the combined UF2 exclusively; application-only UF2 (`zephyr.uf2`) was **not** used and would cause a brick.

## JSON Source

Final local runner outputs:

- `/tmp/opencode/logic-analyzer-wide11-packed-all-freeze-final.json` (54/54)
- `/tmp/opencode/logic-analyzer-high-rate-packed-burst-freeze-final.json` (62/62)
- `/tmp/opencode/logic-analyzer-wide11-mapping-freeze-final-sequential.json`
- `/tmp/opencode/logic-analyzer-wide11-telemetry-isolation-freeze-final-sequential.json`
