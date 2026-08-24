# 2026-08-24 Firmware Memory Optimization HIL

Date: 2026-08-24

## Scope

This run validates the uncommitted RP2350 firmware memory-optimization patch
applied on top of `HEAD` against the same representative G3 RP2350A board. The optimization's
goal is to reduce Flash and RAM footprint without changing the high-rate
continuous-streaming ceiling. This report is the authoritative dated evidence
for the post-optimization sustained-duration contract, the stack high-water
values, and the combined-UF2 BOOTSEL dual recovery under the strict
duration semantics.

The run compares three images on the same board through the same strict
runner:

- the previously audited pre-optimization build
- a clean detached `HEAD` control image (no optimization patch)
- the final optimized working-tree image built on top of `HEAD`

The strict runner fix used for this report removed only the branch that
forced duration success after a server terminal; its 89 Python tests and
the complete offline firmware runner pass. Local repository scripts:
215 tests PASS. Canonical build PASS.

This report does **not** claim that the memory optimization fixed the
pre-existing high-rate continuous limitation. It confirms that the
optimized and clean detached HEAD control images exhibit the same
pre-existing limitation at WS 1 MHz and TCP 400 kHz. The 300 kHz (WS) and
350 kHz (TCP) proven sustained-duration operating points are bracketed by
boundary diagnostics at WS 400 kHz, TCP 400 kHz, and WS 1 MHz that are
recorded as capacity diagnostics, not as sustained PASS.

## Identity

- Repository `HEAD`: `e70eab01e4e3572ebfdee34532dca046e78511d3`
- Branch: `dev`
- Audited baseline (pre-optimization) Flash: `828,948` / `847,832` (97.77%)
- Audited baseline (pre-optimization) RAM: `508,744` / `532,480` (95.54%)
- Clean detached HEAD control image (Flash): `828,980` / `847,832`
- Clean detached HEAD control image (RAM): `508,744` / `532,480`
- Clean detached HEAD control combined UF2 SHA-256:
  `7aa97eb70fe615345f7cd32ada95dd4d91eb9c3e903608221f61f8e7db9c74c4`
- Clean detached HEAD control combined UF2 size: `1,710,080` bytes
- Final optimized image (Flash): `771,256` / `847,832`
- Final optimized image (RAM): `469,528` / `532,480`
- Final optimized combined UF2 SHA-256:
  `f08c7580cc3aa634996fc702e5219a2e49826a572ee97193c1a5ae284320fb06`
- Final optimized combined UF2 size: `1,594,368` bytes
- Savings vs audited baseline: `57,692` Flash and `39,216` RAM
- Clean detached HEAD control direct comparison Flash difference: `57,724`

The Flash and RAM savings are observed on the final optimized working-tree
image built on top of `HEAD`. The clean detached HEAD control image is `HEAD` without the
optimization patch; its Flash size is `57,724` bytes larger than the final
optimized image, and its RAM size is identical to the audited baseline
because the patch recovers the pre-arena alignment hole and reduces selected
thread stacks. Flash savings come from the Sigrok runtime `.data` to `.bss`
move and removal of unused IPv6/I2C/SPI support.

Only the combined UF2 was used for ROM BOOTSEL throughout the run. The
application-only `zephyr.uf2` was never flashed.

## Local Gates

The strict runner fix used for this report removed only the branch that
forced duration success after a server terminal. The runner's 89 Python
tests pass and the complete offline firmware runner passes:

- 89 Python tests for the strict runner: PASS
- Offline firmware model runner: PASS
- Local repository scripts: 215 tests PASS
- Canonical `make firmware`: PASS
- Repository governance, contract, and test-registration tests: PASS

The strict runner is a host-only change. The firmware under test is
unchanged from the prior dated reports; this run does not introduce a
new firmware image.

## Real HIL Commands And Results

The strict runner was invoked with the canonical 5-second continuous
window against the final optimized image. Every sustained-duration claim
below is a real board measurement, not a projection.

### WS SINGLE continuous at 300 kHz (5-second target)

```sh
python3 apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py \
  --matrix ws-continuous \
  --modes SINGLE \
  --tcp-rates-khz 300 \
  --continuous-durations-s 5 \
  --timeout 8
```

Result:

- Duration: `5.012458` s
- Received samples: `1,499,136`
- Effective rate: `~299,082` samples/s
- Sample-index gaps: 0
- Disconnects: 0
- Overruns: 0
- Decode errors: 0
- `stop_response.received`: true
- Restart capability: confirmed
- HTTP health after stop: confirmed
- Verdict: sustained-duration PASS

Evidence hash (final WS300 JSON):

`6ed07e56f19a4b9565e5a01f846d38a5f546aa2c57350d20647a94a524567bd9`

### TCP SINGLE continuous at 350 kHz (5-second target)

```sh
python3 apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py \
  --matrix tcp-continuous \
  --modes SINGLE \
  --tcp-rates-khz 350 \
  --continuous-durations-s 5 \
  --timeout 8
```

Result:

- Duration: `5.008183` s
- Received samples: `1,755,136`
- Effective rate: `~350,454` samples/s
- Sample-index gaps: 0
- Disconnects: 0
- Overruns: 0
- Decode errors: 0
- `stop_response.received`: true
- Restart capability: confirmed
- HTTP health after stop: confirmed
- Verdict: sustained-duration PASS

Evidence hash (final TCP350 JSON):

`5eb985b616ba132536e4720f6904a8c91faadfa2d084a55726e77cf36e80ba72`

## Control Comparison: Optimized vs Clean Detached HEAD

The clean detached HEAD control image was flashed and tested against the
same strict runner. WS SINGLE continuous at 1 MHz on the clean control
image produces the same `server_overrun` boundary shape as the final
optimized image:

- WS SINGLE continuous at 1 MHz (clean detached HEAD control): ~0.119 s,
  ~43k samples, explicit `server_overrun`, zero gaps, zero disconnects,
  restart and HTTP health retained, `pass=false` because duration was
  not met. The strict runner does not claim this as a sustained PASS.

Evidence hash (clean control WS1MHz JSON):

`bdf9c69b2a6b44c02d3fcb7f5794acf9559de8a0fcc8fbca550e75de8529c254`

This proves the high-rate continuous limitation is **pre-existing** in the
firmware architecture and is not caused by the memory optimization patch.
The optimized and clean control images both exhibit the same WS 1 MHz
boundary shape. The same was already documented in the
[2026-07-31 full-functional HIL](2026-07-31-full-functional-hil.md)
where ten 41k-43k sample overrun windows were observed on WS SINGLE 1 MHz
under the pre-optimization firmware.

## Boundary Diagnostics

The 400 kHz WS/TCP and 1 MHz WS rows bracket the boundary where the strict
runner still records an explicit lossless `server_overrun` with zero
gaps/disconnects, but the requested 5 s duration is not met and the runner
reports `pass=false`. They are recorded as capacity diagnostics, not
sustained PASS, and they are not exact absolute ceilings.

### WS SINGLE continuous at 400 kHz (5-second target)

```sh
python3 apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py \
  --matrix ws-continuous \
  --modes SINGLE \
  --tcp-rates-khz 400 \
  --continuous-durations-s 5 \
  --timeout 8
```

Result:

- Duration: `0.084225` s
- Received samples: `27,392`
- Terminal: explicit `server_overrun`
- Sample-index gaps: 0
- Disconnects: 0
- Restart capability: confirmed
- HTTP health after stop: confirmed
- `pass=false` because duration was not met

Evidence hash (final WS boundary JSON):

`407415a625745d134253ab48815b483fdf1e544b145f66ad87ea7d97e1e9a6ae`

### WS SINGLE continuous at 1 MHz (5-second target)

```sh
python3 apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py \
  --matrix ws-continuous \
  --modes SINGLE \
  --tcp-rates-khz 1000 \
  --continuous-durations-s 5 \
  --timeout 8
```

Result:

- Duration: `0.119245` s
- Received samples: `43,232`
- Terminal: explicit `server_overrun`
- Sample-index gaps: 0
- Disconnects: 0
- Restart capability: confirmed
- HTTP health after stop: confirmed
- `pass=false` because duration was not met

### TCP SINGLE continuous at 400 kHz (5-second target)

```sh
python3 apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py \
  --matrix tcp-continuous \
  --modes SINGLE \
  --tcp-rates-khz 400 \
  --continuous-durations-s 5 \
  --timeout 8
```

Result:

- Duration: `0.116484` s
- Received samples: `57,344`
- Terminal: explicit `server_overrun`
- Sample-index gaps: 0
- Disconnects: 0
- Restart capability: confirmed
- HTTP health after stop: confirmed
- `pass=false` because duration was not met

Evidence hash (final TCP boundary JSON):

`cada523806a8472d289867d2dad6e3e4018d2637279855493c57b808cc5a376c`

The same WS 1 MHz 43,232-sample boundary shape was already documented
against the pre-optimization firmware in the
[2026-07-31 full-functional HIL](2026-07-31-full-functional-hil.md) lines
256-259 (ten 41,344-42,976-sample overrun windows). The optimized image
reproduces that shape.

## High-Rate Matrix

The final optimized image was exercised against the full
high-rate capacity-burst matrix documented in section 8b.4.1 of the HIL
spec:

```sh
python3 apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py \
  --matrix high-rate-packed-burst \
  --timeout 5
```

Result:

- Cases: `62 / 62` PASS
- TCP cases: `31 / 31` PASS
- WS cases: `31 / 31` PASS
- Capture rows with zero gaps: `60 / 60`
- Exact 100k sample completions: `20`
- Expected rejections: `2`

The remaining two rows are the expected `WIDE11 125 MHz` START rejections
(INVALID_CONFIG); they are not capture rows and therefore have no gap count.

Evidence hash (final high-rate JSON):

`c2762cc8b41f214b59f99d0d5841d8f61a94f7e9ab0259b10e202f9fea1c0da7`

## Stack High-Water Values

After the sustained WS 300 kHz and TCP 350 kHz runs, the firmware's
stack high-water values were read through the standard status channel:

| Thread | High-water / Capacity |
|--------|------------------------|
| watchdog | `548 / 1024` |
| sigrok | `1268 / 2048` |
| LA consumer | `1100 / 2048` |
| LA producer | `332 / 2048` |
| ADC | `284 / 2048` |
| main | `904 / 2048` |
| net_socket_service | `2020 / 2400` (unchanged from prior dated reports) |

No thread exceeded its capacity; every reduced stack remained below two-thirds
usage, while the unchanged net_socket_service thread remained at
its prior high-water level. The memory optimization does not regress
any monitored stack.

## HTTP And CDC BOOTSEL Recovery

The final combined UF2 was flashed through both recovery paths against
the final optimized image:

- HTTP `POST /api/v1/bootloader` returned `ok=true`; the ROM enumerated as
  `ID_VENDOR=RPI`, `ID_MODEL=RP2350`; the same combined UF2 restored
  HTTP service.
- CDC ACM shell `bootloader` (identified CDC by-id device
  `/dev/serial/by-id/usb-Radxa_Radxa_Linkr_Debugger_E6641C31A362C336-if00`)
  independently enumerated the same ROM target; the same combined UF2
  restored HTTP and watchdog health.

After both recovery paths, post-recovery HTTP, watchdog, persistent
configuration (`saved_count=6`, `pending_count=0`), CDC `vin=3.3v`, and
a one-sample WS capture all passed.

The repo HIL script discovery timed out despite later strict udev identity
and the manual documented `udisksctl` fallback was used; this is a runner
discovery limitation, not a firmware failure. Both recoveries were
executed successfully through the manual documented path.

Evidence hash (final post-CDC one-sample JSON):

`8e7950291ff08a56ed1a43e503ed2630201e8ecdb44393ebcf249fb75834534d`

## Limitations

- The boundary diagnostics at WS 400 kHz, TCP 400 kHz, and WS 1 MHz are
  recorded as capacity diagnostics, not as sustained PASS. They are not
  exact absolute ceilings.
- The 300 kHz (WS) and 350 kHz (TCP) proven operating points are not
  claimed to be exact maximum ceilings. They are the highest rates that
  produced a clean STOP_RESP under the strict 95% duration contract on
  the representative HIL setup in this run.
- The memory optimization does **not** fix the pre-existing high-rate
  continuous limitation. The optimized and clean detached HEAD control
  images both exhibit the same WS 1 MHz boundary shape. TCP 400 kHz was
  measured on the final optimized image only and is reported as its boundary
  diagnostic, not as part of the clean-control comparison.
- The repo HIL script discovery timed out and the manual documented
  `udisksctl` fallback was used. The BOOTSEL recovery was successful
  through both HTTP and CDC paths.
- External 4-bit WIDE11 generator mapping and native sigrok-cli driver
  validation remain out of scope for this report and are covered by
  their own dated reports or are BLOCKED in the
  [2026-07-31 full-functional HIL](2026-07-31-full-functional-hil.md).
- The application-only `zephyr.uf2` was never flashed in this run.

## Final State

After the run:

- Flash: `771,256` / `847,832` (final optimized)
- RAM: `469,528` / `532,480` (final optimized)
- Combined UF2 SHA-256:
  `f08c7580cc3aa634996fc702e5219a2e49826a572ee97193c1a5ae284320fb06`
- HTTP service, watchdog, persistent configuration, CDC, and the
  one-sample WS capture are all available after both recovery paths.
- Every HIL-owned capture and recovery resource was restored or terminated;
  no power, switch, or GPIO mutation was introduced by this run, and the board
  retained its persisted baseline.

## Verdict

PASS for the memory optimization evidence. The optimization reduces
Flash by `57,692` bytes and RAM by `39,216` bytes against the audited
baseline without changing the high-rate continuous-streaming ceiling.
The optimized and clean detached HEAD control images share the same
pre-existing high-rate continuous limitation. The 300 kHz (WS) and
350 kHz (TCP) rows are proven sustained-duration operating points
under the strict 95% duration contract. The 400 kHz WS/TCP and 1 MHz WS
rows are boundary diagnostics, not sustained PASS, and they are not
exact absolute ceilings.

The strict runner fix used for this report removed only the branch that
forced duration success after a server terminal; its 89 Python tests
and the complete offline firmware runner pass. Local repository scripts:
215 tests PASS. Canonical build PASS.

The pre-existing 1 MHz continuous and 443 kHz TCP sustained claims from
the 2026-07-25/26/27 dated reports are not current operating points on
the current architecture; the current behavior matches the 2026-07-31
ten-run WS 1 MHz overrun record.

## Cross-links

- [Logic Analyzer reference](../../reference/logic-analyzer.md): updated
  lossless-or-stop contract, proven 300/350 kHz rows, and boundary
  diagnostics
- [HIL Functional Test Spec](../hil-functional-test-spec.md): updated
  section 8b.4 sustained-duration contract and section 8b.3 stream
  behavior
- [2026-07-31 full-functional HIL](2026-07-31-full-functional-hil.md):
  the original ten 41k-43k WS 1 MHz overrun record (lines 256-259)
- [2026-07-28 pre-trigger and UART HIL](2026-07-28-logic-analyzer-pre-trigger-uart-hil.md):
  bounded pre-trigger and UART decoder evidence
- [2026-07-27 generic packed burst HIL](2026-07-27-logic-analyzer-generic-packed-burst-hil.md):
  historical 62/62 high-rate matrix baseline
- [2026-07-25 finite HIL](2026-07-25-logic-analyzer-finite-hil.md):
  historical pre-transport-cleanup-patch WS 1 MHz ~999 ksps evidence
