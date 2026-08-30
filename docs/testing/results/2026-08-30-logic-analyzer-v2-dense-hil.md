# 2026-08-30 Logic Analyzer Protocol-v2 Dense SINGLE HIL

Date: 2026-08-30

## Verdict

GO for protocol-v2 SINGLE continuous streaming at 16 MHz on the measured
representative board/link with static or low-transition GP10 input.

- WebSocket: 16 MHz passed 10/10 strict five-second runs.
- Raw TCP: 16 MHz passed 10/10 strict five-second runs.
- Both transports passed bounded 100 kHz post=65535 exactly.
- The adjacent measured 18.002 MHz point failed on both transports because the
  received sample count was below 95% of negotiated samplerate times requested
  duration. It retained zero gaps/overruns, STOP, restart, and HTTP health.

NO-GO for claiming high-transition, incompressible, or adversarial-input 16 MHz.
The physical UART stimulus path remained exclusively owned by the user's rustty
process and was not killed, interrupted, or written through.

The candidate was removed after HIL and the archived canonical production image
was restored. No commit or push was performed.

## Contract change

The old continuous rule accepted a five-second connection even when the receiver
obtained materially fewer samples than negotiated. Protocol-v2 HIL now requires
all of the following:

- at least 95% requested duration;
- at least 95% of negotiated samplerate times requested duration samples;
- STOP_RESP for client stop;
- zero sample-index gaps;
- zero disconnects;
- zero protocol decode errors;
- zero overruns;
- immediate restart;
- HTTP health after stop.

This stricter rule exposed 18/20/25 MHz runs that stayed connected for five
seconds while accumulating an unsent tail. Those rows are failures, not operating
points.

## Protocol and hot-path changes

The project is pre-release and intentionally retains no runtime compatibility
with protocol v1. Current firmware and clients use protocol version 2.

Protocol-v2 adds:

- SINGLE_BITS, compression value 4: eight chronological samples per byte,
  least-significant bit first;
- SINGLE_BITS_RLE, compression value 5: RLE over those packed bytes;
- exact tail-padding validation;
- 16,384-sample SINGLE chunks represented by 2,048 bytes before RLE.

Firmware changes:

- aligned SINGLE chunks copy directly from the one-bit DMA ring;
- wrapped or non-byte-aligned spans retain a bounded extraction fallback;
- repeated packed bytes encode directly as a three-byte RLE tuple;
- WS frames are pre-encoded in their existing leased slots;
- TCP encodes in place;
- temporary WS slot exhaustion yields and retries without advancing the ring
  reader, while true ring overwrite pressure still terminates lossless-or-stop;
- unused stream OR/AND aggregation, commit timestamps/sequences, public debug
  snapshot state, and per-chunk sink instrumentation were removed;
- no arena, queue, or persistent RAM increase was introduced.

A pre-existing TCP correctness bug was also fixed. The byte-path RLE candidate
previously overwrote its own source before deciding to fall back, corrupting
alternating/high-entropy input. A regression test first reproduced the failure,
then the in-place compressor was changed to preflight overlap safety.

## Host relative benchmark

Synthetic host benchmark, same machine and binary, 64 Mi samples per pass,
-O2 and native ISA. These numbers are relative evidence only and are not RP2350
throughput claims.

| Input | Old byte-path end-to-end | Final dense end-to-end | Relative gain |
|---|---:|---:|---:|
| zero | 1,241.887 Msamples/s | 53,827.085 Msamples/s | 43.34x |
| alternating | 1,502.970 Msamples/s | 53,379.455 Msamples/s | 35.52x |
| pseudorandom | 412.005 Msamples/s | 38,003.782 Msamples/s | 92.24x |

The original in-place alternating reproduction reported payload_matches=false;
the retained regression now passes.

## Final 16 MHz continuous matrix

Every row requested SINGLE, trigger NONE, pre=0, post=0, and five seconds.
The ten-run matrices used the final firmware/protocol logic immediately before a
WebUI-only rebuild that added the 16 MHz preset. The exact final combined UF2 was
then flashed and passed one additional 16 MHz WS run, one 16 MHz TCP run, and the
embedded-browser preset check.

### WebSocket, ten consecutive runs

- passes: 10/10
- samples per run: 78,004,224 to 79,855,616
- effective receive rate: 15.600 to 15.969 Msamples/s
- zero sample-index gaps, decode errors, disconnects, and overruns
- STOP_RESP, immediate restart, and HTTP health on every run

### Raw TCP, ten consecutive runs

- passes: 10/10
- samples per run: 77,266,944 to 79,937,536
- effective receive rate: 15.451 to 15.986 Msamples/s
- zero sample-index gaps, decode errors, disconnects, and overruns
- STOP_RESP, immediate restart, and HTTP health on every run

The measured input produced repeated packed bytes. Each 16,384-sample DATA chunk
therefore used a three-byte SINGLE_BITS_RLE payload. This is why the result is not
an incompressible-input claim.

## Adjacent 18.002 MHz boundary

| Transport | Received | Strict minimum | Result |
|---|---:|---:|---|
| WebSocket | 83,542,016 | 85,509,500 | FAIL: negotiated sample count not met |
| TCP | 78,413,824 | 85,509,500 | FAIL: negotiated sample count not met |

Both boundary rows retained zero gaps/overruns, STOP, restart, and HTTP health.
They are clean capacity diagnostics, not operating points.

## Bounded and embedded-Web regressions

At 100 kHz with pre=0 and post=65535:

- WS received exactly 65,535 samples in 32 protocol-v2 DATA frames;
- TCP received exactly 65,535 samples in 32 protocol-v2 DATA frames;
- both had zero gaps/errors/overruns and passed STOP, restart, and HTTP health.

The final embedded WebUI was loaded in headless Chromium. The rendered Logic
Analyzer exposed 300 kHz, 1 MHz, 16 MHz, and 25 MHz options, and a screenshot was
retained. Web decoder tests cover SINGLE_BITS, SINGLE_BITS_RLE, malformed shape,
and non-zero tail padding.

## Candidate artifact

- profile/version: production, 0.3.0+0
- build id observed over CDC: v0.2.1-215-g96d2aa896e31-dirty
- combined UF2 bytes: 1,595,392
- combined UF2 SHA-256:
  789dab21b162dcd5b69750cde2e2a1e51cf334b7b70a62943d9f9d18c8b7e272
- FLASH: 771,588 / 847,832
- RAM: 469,424 / 532,480

Only the combined MCUboot plus application UF2 was used for ROM BOOTSEL.
Application-only zephyr.uf2 was never flashed.

## Optimization stopping point without UART

The following variants were measured and rejected rather than retained:

- 32,768-sample TCP chunk: no repeatable throughput gain and less stable memory
  allocation behavior;
- TCP sendmsg batching: only marginal movement at 18 MHz;
- zero-timeout busy poll: starved the equal-priority consumer and caused immediate
  ring overrun;
- zero-timeout plus yield: no repeatable gain over the simple one-millisecond
  receive slice;
- larger WS slots or queues: rejected because they consume scarce persistent RAM
  and the retry-on-ring design already removes false slot-pressure overruns.

The remaining meaningful unknown is input entropy. Tomorrow's UART run should
exercise at least 1 Mbaud 0x55 and a pseudorandom pattern over WS and TCP, starting
at 1 MHz and increasing only while the strict sample-count contract passes. Until
that evidence exists, further queue, buffer, or protocol complexity would be
speculative.

## Production restoration

Restoration used the candidate firmware CDC bootloader command. It acknowledged
entry into RP2350 BOOTSEL, disconnected as expected, and the archived canonical
production combined UF2 was copied.

Final live state:

- production build id: v0.2.1-208-gc72104d89ca1-dirty
- profile/version: production, 0.3.0+0
- watchdog healthy and armed
- OTA idle and confirmed, no test marker
- fault endpoint HTTP 404
- saved configuration count 6, pending count 0
- heap allocated bytes 0
- no RP2350 BOOTSEL disk remains
- production protocol-v1 one-sample WS smoke passed
- user rustty PID 1118991 remained alive and untouched

Production restore SHA-256:
8d0bc34dfdbcdbe91fa4e5902772965751fbddd626c3c964f3aad5bd7fa0219d

## Evidence

All retained files are under:

2026-08-30-logic-analyzer-v2-dense-hil/

Important files:

- summary.json
- final-ws-16mhz-10x.json
- final-tcp-16mhz-10x.json
- final-ws-18mhz.json
- final-tcp-18mhz.json
- final-ws-bounded-65535.json
- final-tcp-bounded-65535.json
- final-ui16-embedded-webui.png
- candidate-source.patch
- discarded-experiments.json
- restored-status.json
- restored-production-v1-ws-smoke.json
- restored-uname.txt
- SHA256SUMS
