# Logic Analyzer Pre-Trigger and UART HIL Report

Date: 2026-07-28

## Scope

This report records the bounded Web pre-trigger implementation and the UART
sample-0 decoder regression fix. It does not replace earlier deep or high-rate
reports. Historical pre=0 rows remain historical evidence for those paths.

The current bounded contract is:

- Triggers are `rising`, `falling`, or `either`.
- `pre_samples >= 1`, `post_samples >= 1`, and `pre_samples + post_samples <= 512`.
- Requested rates are 1-25 MHz.
- The selected physical plan must retain at least `2 * ceil(actual_rate / 1000)` samples.
- At current Web discrete rates, SINGLE supports through 25 MHz, FAST8 supports through 10 MHz and rejects 25 MHz, and WIDE11 supports through 5 MHz and rejects 10 MHz and 25 MHz.

The Web UI permits local editing before the first connection when generic
constraints pass. After connection it uses real per-mode CAPS. It rejects or
disables old firmware, or a selected mode without CAPS mode flag bit 5,
`PRE_TRIGGER` (`1 << 5`). This mode flag is separate from HELLO server flags bit
0, `CONFIG_V2`, and bit 1, `GENERIC_PACKED_BURST`.

Pre-trigger remains zero for trigger `NONE`, Stream and `post=0`, unsupported or
high-rate generic packed burst, and ordinary deep capture. Stream forces both
pre and post to zero. Completion is `pre + post`, and `triggerIndex` equals pre.
Firmware reuses the prepared common packed ring and sink lifecycle. After prefill,
packed samples are the sole trigger authority, software scans the edge, and the
firmware freezes and drains the exact `[T-pre,T+post)` window. No new IRQ pairing
or buffer is introduced.

## Firmware Build and Flash

Canonical RP2350 sysbuild footprint:

- FLASH: 701900 / 847832 bytes, 82.79%
- RAM: 475896 / 532480 bytes, 89.37%
- Combined UF2: 1455616 bytes

Only `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2` was flashed.
The application-only `zephyr.uf2` was not used for ROM BOOTSEL.

HTTP BOOTSEL succeeded. Unprivileged `picotool` was detected but could not access
the ROM device, so the approved unprivileged `udisksctl` mount and copy path was
used.

## Protocol HIL

WebSocket SINGLE at 1 MHz was configured for rising trigger, `pre=64`, and
`post=448`. The stimulus was `Press` on `/dev/ttyACM1` at 115200 baud, 8N1.
The run confirmed:

- CAPS mode flag bit 5, `PRE_TRIGGER`.
- CONFIG v1 negotiation.
- 512 samples received.
- Trigger index 64.
- Zero sample gaps and zero disconnects.
- Immediate restart and HTTP health.

The reproducible runner command is:

```sh
nix-shell -p python3Packages.websocket-client python3Packages.pyserial --run 'python3 apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py --matrix ws-bounded --modes SINGLE --tcp-rates-khz 1000 --tcp-pre-samples 64 --tcp-post-samples 448 --trigger-types rising --trigger-channel 0 --uart-stimulus Press --uart-device /dev/ttyACM1 --uart-baud 115200 --timeout 5'
```

Raw GP10 capture was idle high. The first start sample was 20. The five byte
windows were 20-107, 107-194, 194-281, 281-368, and 368-455. Bit runs were 8 or
9 samples, or multiples of those lengths, consistent with the 115200 baud,
1 MHz capture.

## Browser and Decoder HIL

The board-hosted React and Playwright run completed 512 samples. The WASM UART
decoder returned exactly:

```text
50 72 65 73 73
```

It produced five annotations and zero diagnostics. Fresh desktop and 390px
mobile visual QA passed two independent Oracle reviews. The desktop and mobile
checks found no clipping, horizontal overflow, annotation loss, or trigger-grid
misalignment.

The UART root cause was isolated to a frame accepted at sample 0. The old decoder
resumed scanning at sample 1, causing internal transitions to be treated as false
start bits. The resume cursor now mirrors ordinary-frame behavior at frame end
minus one bit and retains back-to-back frames. A known-good `Press` waveform
reproduced the old `50 25 95 CD CD`; the fixed decoder returns exactly
`50 72 65 73 73`. The full decoder suite passed 21/21. The corruption was not
attributed to baud, inversion, or firmware behavior.

## HTTP and CDC Recovery

The CDC `/dev/ttyACM2` help output listed `bootloader`. Issuing the shell
`bootloader` command entered ROM BOOTSEL and disconnected CDC as expected. The
same combined UF2 restored HTTP service and the same serial identity after
re-enumeration.

## Historical Evidence Boundary

Earlier freeze and deep or high-rate matrices remain valid as pre=0 evidence.
They are not rewritten as pre-trigger results. In particular, the existing
WIDE11 and WIDE12 reports continue to describe their original deep post, Stream,
NONE, and freeze behavior.
