# 2026-08-31 Logic Analyzer Protocol-v2 Multichannel HIL

## Scope and verdict

This report covers sustained FAST8 and WIDE11 streaming after the protocol-v2
SINGLE work. The earlier order-of-magnitude optimization was primarily SINGLE;
the pre-change multichannel path still reconstructed every selected pin through
every DMA lane for every sample.

The final candidate removes that hot path, increases packed chunk efficiency
without increasing persistent RAM, replaces false DMA-skew terminals with the
RP2350 PIO RXSTALL hardware signal, adds two-value selector compression, and
bounds the TCP queue before heap exhaustion.

Final strict 10/10 operating points:

| Input profile | FAST8 WS | FAST8 TCP | WIDE11 WS | WIDE11 TCP |
|---------------|----------|-----------|-----------|------------|
| Static or low-transition | 2.600 MHz | 2.600 MHz | 1.050 MHz | 1.175 MHz |
| GP16 transition-rich, PACKED_PALETTE2 | 1.200 MHz | 1.375 MHz | 850 kHz | 950 kHz |

Every PASS requires at least 95 percent requested duration, at least 95 percent
of negotiated-rate times duration samples, zero gaps, disconnects, decode errors,
and overruns, STOP_RESP, a fresh immediate restart, and HTTP health.

A commit-review closure additionally built the exact detached review snapshot and
verified transition-rich SINGLE on GP16 at 1.000 MHz over both transports,
10/10 each under the same strict criteria.

## Original multichannel candidate identity and footprint

This subsection records the candidate used for the original multichannel matrices;
the later exact detached review build is recorded separately below.

- Git HEAD used for build identity: 96d2aa896e3137f6f5eb12c65618483f22fbc286
- Firmware build id: v0.2.1-215-g96d2aa896e31-dirty
- Protocol version: 2
- Combined MCUboot plus application UF2 SHA-256:
  72bdcfb61f1ae4aab5362f2eb58597f7293d3c5bf752c09a71d3205da522a91b
- OTA application SHA-256:
  a6c1640e67a0c3ca26d8c7fa9e310a81f73fee893166a86d378d5c108eeafd5f
- FLASH: 773876 / 847832 bytes
- RAM: 469232 / 532480 bytes
- Capture arena: 148856 bytes, down 192 bytes from the former layout
- TCP DATA queue cap: 10 frames with terminal reserve

ROM BOOTSEL used only the combined UF2. Application-only zephyr.uf2 was never
flashed. Iterative candidates after the first combined-UF2 boot used verified,
test-booted, and confirmed MCUboot OTA images.

## Root causes and retained changes

### 1. Generic per-pin ring reconstruction

The old FAST8 and WIDE11 stream sink called the generic packed-ring decoder.
Each sample rebuilt selected output bits by walking selected pins and physical
lanes, repeatedly doing containment checks, division, modulo, and shifts.

Retained exact-shape paths:

- FAST8 GP10 through GP17 treats the little-endian DMA ring as a byte ring and
  copies a span with one or two memcpy operations.
- WIDE11 GP10 through GP20 tracks lane-A four-sample words and lane-B ten-sample
  words with running counters, then emits the 8-bit low lane and 3-bit high lane.
- Sparse or reordered selections keep the generic decoder.

Large random-data tests cover FAST8 2048-sample wrap, WIDE11 1024-sample lane-A
wrap and 257-sample lane-B wrap, 4/10/20-sample boundaries, and sparse FAST8
ordering.

### 2. Fixed per-frame overhead

The first optimized board results exposed approximately 488 chunks per second as
a common limit: FAST8 used 2048 samples per frame and WIDE11 used 1024. The WS
pool changed from eight 2048-byte payload slots to four 4096-byte payload slots.
Total arena use decreased by 192 bytes. Packed sink chunks are now 4096 FAST8
samples or 2048 WIDE11 samples. Dense SINGLE remains 16384 samples and was not
expanded back to the previously rejected 32768-sample experiment.

### 3. False WIDE11 skew overrun

Clean CDC evidence showed WIDE11 stopping with max_unread only 4666 samples, but
DMA write-pointer skew 68 exceeded the old hard-coded 20-sample threshold.
Joined RX FIFOs, partial autopush words, and in-flight DMA make sequential DMA
write-address snapshots non-atomic. A second capture reproduced skew 84.

The final implementation clears PIO FDEBUG RXSTALL at arm and uses the sticky
RXSTALL bit as the authoritative FIFO-stall signal. It still advances the writer
at the minimum completed lane sequence and still stops on actual ring overwrite.
Unit coverage accepts arbitrary diagnostic skew when RXSTALL is clear and emits
definite overrun when RXSTALL is asserted.

### 4. PACKED_PALETTE2

Compression value 6 carries two distinct packed sample values followed by one
LSB-first selector bit per chronological sample. It is emitted only when a chunk
contains exactly two values and is strictly smaller than both sample-value RLE
and raw BIT_PACK. One-value chunks use RLE; three-or-more-value chunks fall back
losslessly.

The encoder performs byte loads into uint16 values rather than per-sample
memcmp/memcpy. This specialization both saved flash and materially increased the
board boundary.

### 5. TCP heap headroom

An unconstrained dynamic queue reached 48856 of the 49156-byte heap and left the
TCP server permanently unable to accept a fresh connection. Queue-depth A/B:

| Depth | Result |
|-------|--------|
| 6 | Safe, but WIDE11 active boundary only 875 kHz; peak 24792 bytes |
| 8 | WIDE11 925 kHz 10/10; peak 33056 bytes |
| 10 | WIDE11 950 kHz 10/10 and long mixed matrices remain restartable |
| 11 | WIDE11 975 kHz 10/10, but a later FAST8 matrix stuck teardown on run 8 |
| effective 12 or heap limit | 48856-byte peak, permanent fresh-connection failure |

Depth 10 is retained. The fixed-pool experiment is discarded because the smaller
dynamic-cap change preserves throughput with less lifecycle complexity.

## Host decoder A/B

Seven-run medians on the same cache-resident host model:

| Mode and input | Before convert | After convert | After end-to-end |
|----------------|----------------|---------------|------------------|
| FAST8 zero | 33.134 Msamples/s | 69656.046 Msamples/s | 2630.776 Msamples/s |
| FAST8 random | 33.057 Msamples/s | 129272.287 Msamples/s | 5012.652 Msamples/s |
| WIDE11 zero | 23.600 Msamples/s | 1181.156 Msamples/s | 552.490 Msamples/s |
| WIDE11 random | 23.445 Msamples/s | 1175.934 Msamples/s | 474.030 Msamples/s |

These cache-resident host numbers prove the decoder CPU mechanism only. They are
not RP2350 or transport throughput claims.

## Board baseline before multichannel fast paths

| Mode | Transport | Requested range | Effective receive before terminal |
|------|-----------|-----------------|-----------------------------------|
| FAST8 | WS | 200 to 500 kHz | about 58 ksample/s, early overrun |
| FAST8 | TCP | 200 to 500 kHz | about 62 to 63 ksample/s, early overrun |
| WIDE11 | WS | 100 to 250 kHz | about 37 to 38 ksample/s, early overrun |
| WIDE11 | TCP | 100 to 250 kHz | about 40 to 41 ksample/s, early overrun |

## Activity qualification

GP16 was connected to /dev/ttyACM1 TX with common ground. A bounded writer sent
continuous UART 0x55, 8N1. For the raw fallback boundary, UART baud equaled the
sample rate so decoded bit6 changed on nearly every sample. For final palette
matrices, UART ran at 921600 baud for the entire matrix.

The HIL runner now records, over the first 65536 decoded samples, compression
frame counts, sample OR, sample AND, and full-sample transition count. An active
PASS must show bit6 in OR, bit6 absent from AND, and non-zero transitions.

This proves one transition-rich line inside all selected FAST8 or WIDE11
channels. It does not prove independent entropy on all channels or simultaneous
lane-A/lane-B transitions.

## Review-time exact-snapshot SINGLE high-transition closure

The initial review correctly identified that the earlier 16 MHz SINGLE result
used static or low-transition GP10 input. A clean detached review snapshot at
commit e6c40e92344b562651214a38f5713e444ac704ab was therefore built with the full
canonical make firmware path and installed through MCUboot OTA:

- Firmware build id: v0.2.1-217-ge6c40e92344b
- Combined UF2 SHA-256: 9dd57fd658e84896cfdb36de2c5f7bc3a9ffdef3fac87551963674dc5d45d248
- OTA SHA-256: dfaa3b63eb679440679a73417cb6ff1c209df2c9fedebd7265f53045734a7624
- FLASH/RAM: 772128 / 469232 bytes
- Exact-snapshot Web tests: Node 373 pass, 1 skip; Vitest 59 files / 433 tests
- Exact-snapshot Web production build: PASS

GP16 alone was selected with channel mask 0x0040. The /dev/ttyACM1 TX stimulus
sent continuous UART 0x55 at 921600 baud for all twenty five-second runs:

| Path | Requested | PASS | Minimum effective receive | Minimum samples | Minimum transitions |
|------|-----------|------|---------------------------|-----------------|---------------------|
| SINGLE TCP | 1.000 MHz | 10/10 | 1001468 sample/s | 5013504 | 60441 / 65536 |
| SINGLE WS | 1.000 MHz | 10/10 | 995886 sample/s | 4980736 | 60448 / 65536 |

Every accepted DATA frame used SINGLE_BITS compression value 4 rather than RLE,
so this is a transition-rich dense-wire result rather than a static compression
extrapolation. Both rows have zero sample-index gaps, decode errors, disconnects,
and overruns; every run received STOP_RESP, passed a fresh immediate restart,
and retained HTTP health. This closes the original 1 MHz high-transition SINGLE
regression. It does not claim that the separate static 16 MHz result also holds
for incompressible or equivalently transition-rich input.

## Raw active-input baseline before palette compression

Matched UART baud and sample rate; every accepted DATA frame is BIT_PACK:

| Mode | Transport | PASS | Adjacent FAIL | Activity evidence |
|------|-----------|------|---------------|-------------------|
| FAST8 | WS | 500 kHz | 550 kHz | 65485 to 65369 transitions / 65536 |
| FAST8 | TCP | 500 kHz | 550 kHz | 65478 to 65372 transitions / 65536 |
| WIDE11 | WS | 250 kHz | 300 kHz | 65504 to 65502 transitions / 65536 |
| WIDE11 | TCP | 250 kHz | 300 kHz | 65505 to 65504 transitions / 65536 |

## Final activity-qualified 10-run matrices

| Path | Requested | PASS | Minimum effective receive | Minimum samples | Minimum transitions |
|------|-----------|------|---------------------------|-----------------|---------------------|
| FAST8 WS | 1.200 MHz | 10/10 | 1194370 sample/s | 5976064 | 50368 |
| FAST8 TCP | 1.375 MHz | 10/10 | 1365435 sample/s | 6832128 | 43954 |
| WIDE11 WS | 850 kHz | 10/10 | 845476 sample/s | 4229120 | 59841 |
| WIDE11 TCP | 950 kHz | 10/10 | 949262 sample/s | 4753408 | 63627 |

All rows have zero sample-index gaps, decode errors, disconnects, and overruns.
All frames use PACKED_PALETTE2 for the analyzed active window.

Adjacent final active-profile failures:

| Path | Requested | Result |
|------|-----------|--------|
| FAST8 WS | 1.225 MHz | server_overrun before duration; fresh restart remains OK |
| FAST8 TCP | 1.400 MHz | server_overrun before duration; fresh restart remains OK |
| WIDE11 WS | 875 kHz | server_overrun before duration; fresh restart remains OK |
| WIDE11 TCP | 975 kHz | server_overrun before duration; fresh restart remains OK |

## Final static or low-transition 10-run matrices

| Path | Requested | PASS | Minimum effective receive | Minimum samples |
|------|-----------|------|---------------------------|-----------------|
| FAST8 WS | 2.600 MHz | 10/10 | 2595269 sample/s | 12980224 |
| FAST8 TCP | 2.600 MHz | 10/10 | 2615053 sample/s | 13082624 |
| WIDE11 WS | 1.050 MHz | 10/10 | 1047448 sample/s | 5238784 |
| WIDE11 TCP | 1.175 MHz | 10/10 | 1179844 sample/s | 5902336 |

FAST8 WS 2.800 MHz failed; FAST8 TCP 2.700 MHz passed only 7/10. WIDE11 WS
1.100 MHz failed; WIDE11 TCP 1.200 MHz passed only 8/10. Unstable rates are not
advertised as operating points.

## Validation and resource boundaries

- Full offline firmware/model runner: PASS; Python suite reports 95 tests OK
- Web protocol tests: PASS; Node runner 375 passes plus Vitest 60 files / 440 tests
- Web production build: PASS with the repository-pinned wasm-bindgen CLI
- Full canonical make firmware: PASS
- make gates: PASS; 295 contract tests plus all live checkers
- Source/contract whitespace checks: PASS; tracked paths used git diff --check, and the
  new protocol-v2 document used git diff --no-index --check. Byte-exact raw HIL logs and
  embedded patch artifacts are intentionally outside this source-only check.
- Final candidate FLASH/RAM: 773876 / 469232 bytes
- No persistent queue or arena RAM was added; capture arena decreased 192 bytes
- TCP qdepth 10 leaves measured heap headroom and survived long mixed repeated matrices
- Commit-review exact snapshot: full make firmware, Web tests/build, and GP16
  SINGLE 1 MHz WS/TCP 10/10 HIL all PASS

## Limitations

- GP16 is in lane A. This report does not independently toggle a WIDE11 lane-B pin.
- One active line plus ten stable lines is not all-channel incompressible entropy.
- Three-or-more-value chunks fall back correctly but have the lower raw transport envelope.
- Transition-rich SINGLE is verified at 1 MHz on GP16; the separate 16 MHz result
  remains static/low-transition and is not an incompressible-input claim.
- No watchdog fault injection was performed.
- The 100/125 MHz deep packed burst paths are bounded captures, not sustained stream rates.

## Production restoration

The candidate entered ROM BOOTSEL through the HTTP endpoint. Restoration copied only the
archived combined MCUboot plus application UF2 with SHA-256
8d0bc34dfdbcdbe91fa4e5902772965751fbddd626c3c964f3aad5bd7fa0219d.
Application-only zephyr.uf2 was not used.

Final live state after the production protocol-v1 smoke:

- CDC firmware build id: v0.2.1-208-gc72104d89ca1-dirty
- HTTP profile/image: production, 0.3.0+0
- Watchdog: supported, automatic, healthy, armed, no failing service
- OTA: idle, swap type 1, current image confirmed, no test marker
- Persistent configuration: saved 6, pending 0
- Heap: allocated 0, free 49156, boot peak 0
- Fault endpoint: HTTP 404
- No RP2350 BOOTSEL disk remains
- CDC uname returned Zephyr
- Protocol-v1 WS bounded SINGLE smoke: 1 sample, zero gaps/overruns, STOP_RESP,
  fresh restart, and HTTP health
- No rustty or temporary UART/CDC probe process remains

After the exact review candidate HIL, the archived production OTA image was
test-booted and confirmed. The production build id, protocol-v1 one-sample
WS smoke, watchdog/OTA/config/heap/fault/BOOTSEL checks, and absence of
/dev/ttyACM1 owners were all re-verified.

## Evidence

The evidence directory contains baseline and final JSON matrices, host A/B output,
CDC root-cause logs, firmware build output, candidate hashes, queue-depth decisions,
software-gate logs, candidate and restored-production health, protocol-v1 smoke, the relevant
source patch, machine-readable summaries, exact-review build/Web logs, the GP16 SINGLE 1 MHz
10-run matrices, and the second production-restore receipt. SHA256SUMS covers every evidence
file except the manifest itself. Host and removable-media path publication sanitization is recorded in the
evidence directories REDACTIONS.md.

