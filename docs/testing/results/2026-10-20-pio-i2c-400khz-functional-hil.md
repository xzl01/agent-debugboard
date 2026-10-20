# PIO I2C 400 kHz functional HIL — 2026-10-20

## Verdict

**PASS for bounded functional characterization of this x1.2 sample and fixture.**

**Verdict at the time of this run: NO-GO for changing the then-200 kHz
production default.** This run did not measure analog rise time, `VOL`,
powered-off leakage, or arbitrary cross-domain power sequencing. The historical
run therefore did not, by itself, close every electrical requirement for a
production 400 kHz claim.

## Post-run promotion decision

After the run, the user explicitly authorized the 400 kHz production Device Tree
default under this product contract:

- Production formally populates 4.7 kOhm SDA/SCL BOM pull-ups to the
  always-powered board `VCC_3V3` rail.
- The qualified 400 kHz baseline leaves J8 unloaded. Any external device,
  cable, pull-up, or foreign power domain requires a new total-capacitance,
  rise-time, `VOL`/sink-current, and powered/unpowered-path validation; arbitrary
  or cross-domain pull-ups are not allowed.
- The actual software contract probes CH224A at startup and polls it on status
  requests. With no PD source/VHV, failures enter a five-second probe backoff;
  later status polling recovers after source insertion, while the VCC_3V3-
  powered INA226 bus remains operational.
- CH224A CFG has a 6.5 V absolute maximum and still has no vendor-guaranteed
  powered-off `Ioff`. Polling does not prove Ioff safety; this remains a
  non-frequency-specific hardware boundary.
- No-PD polling behavior may be characterized in a separate future HIL, but it
  is not a prerequisite for the 400 kHz promotion.

The bounded digital functional PASS below supports that product decision, but
is not a complete UM10204 analog or 50 ns spike-filter certification.

## Scope and safety boundary

**Post-run fixture evidence correction:** the user/operator corrected the
previously recorded flying-wire pull-up value to **4.7 kOhm**. This changes only
the fixture metadata and derived calculations below. It is not a new HIL execution
and does not change the bounded functional observations.

- Only USB topology path `1-1.4.2`, its `ttyACM3` control interface, and
  `eth1` / `172.29.201.1` were operated.
- SDA and SCL each had a 4.7 kOhm flying-wire pull-up to board `VCC_3V3`.
- The pull-ups followed board power, J8 was otherwise unloaded, and CH224A VHV
  remained powered. No one-sided power-down test was performed.
- Firmware retained `bias-disable`, output latch zero, output-enable-only
  open-drain control, startup fail-close, and disabled generic GPIO/MMIO/I2C
  shell write paths.
- Every startup/reboot was followed by `5v_out=on`, fixed PD 20 V, and a valid
  INA226 voltage/current read before further HIL.

The 4.7 kOhm external pull-ups in parallel with the CH224A specified
7/10/15 kOhm internal pull-ups give calculated effective values of
2.812/3.197/3.579 kOhm. With the known 34 pF load component, the calculated
rise-time range is 81.0–103.1 ns and the 3.3 V low-state pull-up current is
0.922–1.174 mA. The 4.7 kOhm external pull-up alone gives `tr = 135.4 ns` and
`Cbmax = 75.33 pF` at the 300 ns limit. The worst-case parallel value gives
`Cbmax = 98.94 pF`, leaving 64.94 pF beyond the known 34 pF. These are
calculations, not oscilloscope measurements.

## Software under test

- Git base: `e00d7bc` (`feat(web): refine power telemetry controls`).
- Working-tree patch SHA-256:
  `ef283a801a3a9dd18cc0913095bd700f82fc14e16ab81f8917411d7b66c42446`.
- HIL overlay content SHA-256:
  `8ba4dcf0448ec4f99a5fadb6d357cb93c2db5f640012a2974caf77537ae36107`.
- 400 kHz combined UF2 SHA-256:
  `9e0f28af3d36e400c8b46725cbeea066aa7b65dced76406536df5c487435c025`.
- Restored 200 kHz combined UF2 SHA-256:
  `83412907674c06d164623958bc1843210356f4615e8a8b07c371dca65637f77d`.

The 400 kHz image was a complete MCUboot-plus-application UF2. At the time of
the run it appended a temporary, per-image overlay whose complete effective
content was `&pio_i2c0 { clock-frequency=<400000>; }`; the overlay content
SHA-256 is retained above. After 400 kHz was promoted into the base product
Device Tree, that redundant HIL fixture was deleted. There is intentionally no
reproduction command referring to the deleted path.

Generated `zephyr.dts` contained `clock-frequency = <0x61a80>` (400000).
The then-production restoration build contained `<0x30d40>` (200000).
At the time, rebuilding with the temporary overlay reproduced the exact same
400 kHz combined-UF2 SHA-256 recorded above; the fixture was deleted only after
the later product promotion.

## Static evidence before HIL

- PIO/electrical/state policy: 14/14 passed with no skips after the generated
  build existed.
- Firmware offline runner: passed, including 96 Python tests and the dedicated
  PD/CH224A, BOOTSEL-button, and reboot host tests.
- Full 200 kHz canonical firmware build: passed.
- Independent review closed three defects before HIL:
  - STOP, repeated START, and recovery now wait for physical SCL high before
    counting setup/high time.
  - SDA/SCL Schmitt input is explicitly enabled while pulls remain disabled.
  - NAK cleanup clears FIFO, EXEC/WAIT, ISR/OSR state, shift counters, and old
    prefetched words before it accepts a cleanup STOP record.
- At 150 MHz, the static model covers all 32 fractional-divider phases. The
  fastest ordinary period is 400 kHz; the average is about 399.933 kHz.

The static proof does not establish 50 ns spike suppression or analog bus
compliance.

## Functional results

### Startup and basic I2C

The 400 kHz image booted normally. The mandatory preflight reported watchdog
healthy/armed, fixed PD 20 V, five ADC channels, and a valid enabled 5 V rail.

### Concurrent stress

A 250 Hz WebSocket telemetry subscription ran concurrently with 300 iterations
of `GET /api/v1/adc/read` plus `GET /api/v1/pd`:

- HTTP requests: 600/600 successful.
- WebSocket samples: 1510 in 83 batches.
- Reported source drops: 435.
- Drops inferred from timestamp intervals: 437.
- Median sample interval: 4100 us.
- 5 V measured range: 5.0975–5.24625 V.
- No malformed samples, channel mismatch, I2C request failure, watchdog fault,
  or post-run ADC failure occurred.

The drop count is expected under deliberate HTTP/telemetry contention and was
reported rather than hidden.

### Uncontended 250 Hz telemetry

- Samples: 1500 in 75 batches.
- Reported drops: 43.
- Drops inferred from timestamps: 43.
- Median interval: 4000 us.
- Minimum/maximum interval: 2600/9400 us.

### GP20/SCL read-only digital capture

WIDE11 sampled GP20 at 5 MHz for 100000 samples while the host issued repeated
ADC reads. The logic-analyzer path left GP20/GP21 under PIO0 I2C ownership.

- DATA frames: 98; sample-index gaps: 0; decode errors: 0.
- ADC reads across the complete capture-session flow, from before the binary
  handshake through STOP/cleanup: 17/17 successful. The worker deliberately
  established one successful read before the handshake, so this count is not
  presented as 17 reads wholly contained in the 20 ms sample window. The run
  did not retain a separate overlap-only request count.
- SCL transitions: 752; bounded active low/high pairs: 356.
- Observed low runs: minimum 7 samples (about 1.4 us), mean 7.42 samples.
- Observed high runs: minimum 5 samples (about 1.0 us), mean 5.42 samples.
- Mean bounded period: 12.84 samples; digital estimate: about 389.5 kHz.

The 5 MHz digital sampler quantizes edges in 200 ns steps, and the bounded run
set includes transaction-boundary timing. It is suitable evidence that the
board generated near-400 kHz digital SCL with Fast-mode-sized high/low periods,
not a substitute for 30%–70% analog rise-time measurement.

After STOP/cleanup, ADC and PD remained readable and watchdog remained healthy.

### Reboot and restoration

The 400 kHz image completed a normal cold reboot and then passed mandatory
preflight plus a bounded repeated-ADC-read loop. The loop completed without an
observed read failure, but its per-iteration results were not retained as a
separate artifact, so this report does not claim an independently reproducible
exact request count. A sysfs observation script encountered
a host-side removal race while reading `devnum`; the device nevertheless
was next observed as the application, and the accepted API response identified
the operation as normal reboot rather than BOOTSEL. Because of that observation
race, this run does not claim proof that no unobserved intermediate USB identity
appeared.

The saved 200 kHz complete UF2 was then flashed back through the same USB path.
The restored image passed watchdog, PD 20 V, 5 V INA226, and five-channel ADC
checks.

## Important limitation discovered during capture setup

Raw-TCP port 5556 was unavailable on subnet 201 because the listener is still
bound to the fixed address `172.29.203.1`. The SCL capture therefore used the
same Sigrok v2 protocol over the board's WebSocket transport. This is a separate
network-listener defect and does not change the I2C verdict.

## Unclosed characterization limits after product promotion

- Measure SCL and SDA 30%–70% rise time and verify `tr <= 300 ns` across voltage,
  temperature, and component tolerance.
- Measure `VOL` for RP2350, every INA226, and CH224A pull-down cases.
- Prove powered-off behavior for VCC_3V3-only, VHV-only, and external-J8-only
  conditions; INA226 SCL must never exceed `VS + 0.3 V`.
- Resolve or isolate the CH224A internal pull-up rail and missing powered-off
  `Ioff` guarantee.
- Establish 50 ns spike suppression if full UM10204 conformance is claimed.
- Repeat on enough boards to cover the CH224A 7–15 kOhm pull-up tolerance.

These limits prevent claiming complete UM10204 analog, spike-filter, arbitrary
J8-load, or powered-off-domain certification. They do not change the later
user-authorized 400 kHz production default under the explicit BOM/VHV/J8
contract above. The recorded run remains sample-specific functional evidence.
