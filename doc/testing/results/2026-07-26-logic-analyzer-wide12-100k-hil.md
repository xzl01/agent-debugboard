# Logic Analyzer WIDE12 Deep Burst HIL Evidence — 2026-07-26

## Build and Flash

Canonical build:

```sh
west build -p always -b rpi_pico2/rp2350a/m33/mcuboot --sysbuild apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
```

Safe combined UF2 for ROM BOOTSEL flashing:

```
build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2
```

**Never flash the app-only `zephyr.uf2` through ROM BOOTSEL. It does not contain
MCUboot and will brick the board. Always use the combined UF2.**

Final firmware footprint:

| Metric | Value |
|--------|-------|
| Flash | 693504 / 847832 bytes |
| RAM | 515544 / 532480 bytes |
| Heap | 49152 bytes |

## Board Endpoints and Setup

- Default device URL: `http://172.29.203.1`
- Sigrok WebSocket transport: `/api/v1/live-sessions` → `/api/v1/ws/<slot>` (binary Sigrok protocol)
- Sigrok raw-TCP transport: port `5556`
- HIL stimulus: GP10 UART at 115200 baud

## WIDE12 Deep Burst Contract

The large-depth WIDE12 capture uses a dual-SM packed arena architecture. The
exact supported large-depth case is:

- **Mode**: WIDE12 (GP10-GP20 + GP29, 12 channels, GP29 is bit 11)
- **Rate**: 100 MHz requested
- **Pre-trigger**: 0 (only pre=0 is supported for this depth)
- **Post-trigger**: 100000 samples
- **CONFIG_V2 capability negotiation**: HELLO server_flags bit 0 advertises CONFIG_V2;
  client must use frame0x0b with 16-byte payload containing u32LE pre/post fields
- **v1 frame0x05 / 12B** remains for bounded captures with post <= 65535 and
  post=0 sentinel for continuous stream
- **Rejection**: other post > 65535 configurations receive CONFIG_RESP but START
  returns INVALID_CONFIG
- **Acquisition architecture**: SM-A captures GP10-GP20 using 11-bit autopush at
  22-entry threshold, 200000 B source; SM-B captures GP29 using 1-bit autopush at
  32-entry threshold, 12500 B source; two DMA channels with 212500 B total source;
  no network or flash activity during the 1 ms acquisition window. NONE deep burst
  uses two capture SMs; triggered deep burst adds a third SM running the
  3-instruction trigger program (peak 3 SMs).
- **Post-capture transport**: up to 98 DATA frames, maximum 1024 samples per frame,
  200000 B total payload
- **Arena lifecycle**: the 216684 B shared arena quiesces ADC telemetry, power
  capture, and normal Sigrok pool for the lease lifetime and restores after drain
- **Two-phase START**: ownership and quiesce are ready before the response. NONE
  sends START_RESP in RUNNING state with no ARMED event; triggered captures send
  START_RESP in ARMED state followed by the ARMED event. GO then synchronously
  enables both sampler SMs

## Pass Summary

All tests used the combined UF2 only. The latest combined UF2 containing the Web
exact-mask and telemetry isolation fixes was flashed and recovered normal HTTP
startup. First triggered-attempt runs failed because
`/dev/ttyACM1` was busy (WCH serial held by another process); the enhanced runner
now records exception type and message in that case. Retests after freeing the
port passed.

| Test | Mode | Trigger | Rate | Result |
|------|------|---------|------|--------|
| Stress WS bounded | WIDE12 | NONE | 100 MHz | PASS — 10/10 runs, each 100000 samples, zero gaps, zero disconnects, fresh restart succeeded after every run |
| Bounded WS rising | WIDE12 | rising | 100 MHz | PASS — trigger index 0, exactly 100000 samples |
| Bounded TCP rising | WIDE12 | rising | 100 MHz | PASS — trigger index 0, exactly 100000 samples |
| Bounded WS NONE | WIDE12 | NONE | 100 MHz | PASS — exactly 100000 samples, zero gaps |
| Bounded TCP NONE | WIDE12 | NONE | 100 MHz | PASS — exactly 100000 samples, zero gaps |
| Reduced pin mapping | WIDE12 | rising | 100 MHz | PASS — 100000 samples, 98 frames, GP10 bit 0 low/high with 4 transitions in 4096 checked samples; GP11/GP20/GP29 zero mask `0x0c02` had 0 violations |
| Shared-arena telemetry isolation | WIDE12 | NONE | 100 MHz | PASS — concurrent JSON WS remained connected; 0 old-epoch post-grace samples, 0 binary/malformed frames; telemetry resumed in a reset epoch with advancing device time |
| Old v1 WS SINGLE | SINGLE | rising | 100 MHz, post=512 | PASS — exactly 512 samples, zero gaps |
| Continuous WS 1 MHz | SINGLE | — | 1 MHz, 5 s | PASS — 4,999,168 samples at 999,252.9/s, zero gaps, zero disconnects |

## Supporting Tests

| Test | Result |
|------|--------|
| ADC read | PASS |
| Manual power capture (begin 1/sample 9/complete 1, post=8) | PASS |
| HTTP health after capture | PASS |
| HTTP restart after capture | PASS |
| Combined UF2 flash recovery | PASS |
| Post-release ADC HTTP read after telemetry-isolation burst | PASS — 3 readings |

## Notes

The WIDE12 deep burst implementation uses a distinct architecture from the finite
capture path. NONE deep burst uses two capture SMs (SM-A, SM-B); triggered deep
burst adds a third SM running the 3-instruction trigger program, so peak is 3
SMs. The 216684 B shared arena temporarily removes ADC telemetry, power capture,
and normal Sigrok pool resources for the lease lifetime; normal operation resumes
after the capture drain completes.

The HELLO server_flags bit 0 advertises CONFIG_V2 capability. Clients must use
frame0x0b with 16-byte payload (u32LE pre/post) for deep burst CONFIG. The v1
frame0x05 (12B) remains the correct CONFIG frame for bounded captures with
post <= 65535 or post=0 stream sentinel. Other larger configurations receive
CONFIG_RESP but START returns INVALID_CONFIG.

The available wiring connected only `/dev/ttyACM1` TX to GP10; GP11, GP20, and
GP29 remained externally low. The reduced mapping test therefore validates GP10
bit 0 activity and absence of unexpected high/crosstalk on bits 1, 10, and 11.
It does **not** validate independent high-state mapping for GP11, GP20, or GP29,
and does not validate GP29 dual-SM transition alignment. The documented external
four-channel generator test remains the procedure for that stronger claim.

The telemetry-isolation HIL kept a JSON WebSocket ADC subscriber active while a
raw-TCP WIDE12 deep burst acquired and drained. Thirteen already queued baseline-
epoch records were classified inside the bounded grace window; after that there
were zero old-epoch records before the observable sequence reset. The socket
remained connected with zero binary or malformed frames. Sequence reset plus
advancing device time marked arena release, after which fresh telemetry and ADC
HTTP reads resumed.

The first triggered-attempt test runs failed with a busy `/dev/ttyACM1` error.
The runner was enhanced to record exception type and message, and subsequent
retests passed. This was a runner issue, not a firmware issue.

The 1 MHz continuous result (4,999,168 samples at 999,252.9/s) confirms the
WS SINGLE 1MHz stable operating point with the current firmware. The effective
rate of 999,252.9 samples/s is consistent with the ~4.991M-4.997M samples per
5-second run recorded in prior HIL.

## CI Status

The five-way final independent review and the skill validation passed after the
Web exact-mask and telemetry-isolation fixes. Full external four-channel
high-state mapping remains unexecuted because the current physical setup exposes
only `ttyACM1` TX to GP10; this report does not claim that stronger mapping
validation.
