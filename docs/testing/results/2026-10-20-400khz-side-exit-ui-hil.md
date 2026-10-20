# 400 kHz production default and side-exit GPIO UI HIL — 2026-10-20

## Verdict and scope

**PASS for the software changes and the bounded functional/visual checks below.**
The production default is now 400 kHz under the operator-confirmed hardware
contract: SDA/SCL each use 4.7 kOhm pull-ups to always-powered VCC_3V3; J8 is
otherwise unloaded or its external load is separately qualified. CH224A keeps
the existing startup probe/status-request polling and five-second failure
backoff. No ADC0/VHV gate was added.

This is not an electrical certification: no powered-off Ioff, analog rise-time,
VOL, single-sided power-down, or 50 ns spike-suppression test was performed.
Those limitations remain as recorded in the
[400 kHz characterization report](2026-10-20-pio-i2c-400khz-functional-hil.md).

Only USB topology `1-1.4.2`, its control CDC ACM (`ttyACM3`), and its NCM path
`eth1` / `172.29.201.1` were operated. Picotool was constrained to the current
bus/address read from that exact sysfs path. No CH347, ACM2, eth0, or adjacent
USB target was used.

## Artifacts

- Final complete MCUboot-plus-application UF2 SHA-256:
  `9e71d819c6b1e9b13ea6cc43ffff323b3208d4d45dab4d58335af1418289dbec`.
- The first integrated image used for TUI navigation HIL was
  `ce5bb0d1749cccaca57214bce5431b18a8106b570ba257212d6e3b8e3f55957f`.
  The final image additionally contains the narrow-header layout fix; firmware
  GPIO metadata and Rust TUI code did not change between those two images.
- Both images were built through `make firmware`. Generated DTS confirms
  `clock-frequency = <0x61a80>` (400000), Schmitt input, and disabled internal
  pulls. All five prohibited generic shell modules remain disabled.

Working HIL artifacts are in `/tmp/opencode/sideout-final/`: firmware hashes,
flash logs, API/WS JSON, browser geometry reports and screenshots, terminal
captures, and the reboot observation. This path is local working evidence,
not an assertion that the files are durably archived in Git.

## Pin identity and layout contract

The view is the component-side assembly top view, not the connector mating
face. The operator confirmed J8's left column runs from pin 10 at the top to
pin 1 at the bottom; its right column runs from pin 20 to pin 11.

| GPIO | Connector pin | Row | Column |
| --- | --- | ---: | ---: |
| GP12 | J8 pin 1 | 9 | 0 |
| GP13 | J8 pin 2 | 8 | 0 |
| GP14 | J8 pin 3 | 7 | 0 |
| GP15 | J8 pin 4 | 6 | 0 |
| GP16 | J8 pin 5 | 5 | 0 |
| GP29 / ADC3 | J8 pin 6 | 4 | 0 |
| GP9 / USER | J13 pin 2 | 0 | 0 |
| GP8 / RSET | J13 pin 3 | 0 | 1 |
| GP7 / MASKROM | J13 pin 1 | 1 | 0 |

J13 pin 4 is GND, not 3.3 V. It has no control. GP17/GP18 stay in the existing
safe-GPIO allowlist but have no J8/J13 placement; they appear in the generic
GPIO group rather than at fabricated connector positions. The diagrams show
only the control subset and must not be treated as complete physical pinouts.

HTTP `/status`, `GET /gpio`, and WS status snapshots return the same
`gpio_layout_groups` catalog: J8/right/order 0, J13/right/order 1. Hosts consume
that catalog without connector-name special cases or coordinate mirroring.

## Offline verification

- Canonical firmware build and complete firmware offline runner: passed.
- I2C/electrical policy: 15/15, no skips after the build.
- Full representative production-router status fixture: 5779/6144 bytes;
  metadata expansion triggers `response_too_large` rather than a partial JSON
  success. The fixture includes real rails, ADC/GPIO descriptors, PD profiles,
  monitoring and configuration summary.
- HTTP/WS string tests cover escaped quotes/backslashes/control characters,
  valid/invalid UTF-8, null arguments, and exact-fit/short buffers. Canonical
  control-character encoding uses valid six-byte `\u00XX` sequences.
- Rust formatting, Clippy with warnings denied, 611 tests, and the release
  build: passed. The release build used a temporary Nix environment providing
  pkg-config and udev after the bare environment lacked `libudev.pc`.
- Final Web: 395 Node tests passed, one pre-existing skip; 588 Vitest tests
  passed; TypeScript, production Vite and decoder WASM build passed.
- `make gates persistent-configuration-docs`, test registration, doc-layout,
  persistent-document contracts, and diff whitespace checks: passed.

The firmware build was run after the standalone Web build because it runs
`npm ci`; the two builds were not allowed to race over `node_modules`.

## Board-level functional checks

### Startup, API and telemetry

Every flash/restart was followed by enabling `5v_out`, requesting fixed PD
20 V, and validating the enabled rail through INA226 before further checks.
On the final image, this preflight reported approximately 5.095 V and valid
current data, with watchdog healthy/armed.

On the final post-reboot run:

- `/status`: 5314/6144 bytes for the observed state; size may vary with runtime
  numbers. `/gpio`: 1677 bytes.
- HTTP and WS each exposed all 11 safe GPIOs, nine exact mapped coordinates,
  the two layout groups, and no connector-coordinate fields for GP17/GP18.
- 100 ADC reads plus 100 PD status reads: 200/200 successful.
- WS telemetry: 300 samples; five values and four rail-voltage values per
  sample; median period 4000 us; reported and timestamp-inferred drops both 11.
- Watchdog remained healthy and armed after the run.
- CDC `pd status` and `ip subnet` returned fixed 20000 mV and subnet 201.
- Release CLI `gpio list` returned the same group metadata and 11 GPIOs.

### Web geometry and visual checks

Chromium visited the real embedded dashboard at widths 320, 375 and 1280 px
in the light scheme, plus 375 px in the dark scheme. It opened hardware I/O
controls without sending any GPIO, power, or route write.

- Document/body width exactly matched each viewport; no page horizontal
  overflow. A discovered 13 px overflow from the real MCU/USB identity chip
  was fixed with normal-flow wrapping and verified on the final image.
- All 11 GPIOs appeared in J8, J13 and the generic group.
- Every active r17 target measured at least 44.045 CSS px.
- Pin centers matched the API rows/columns; right-side decorations were inert.
- Independent screenshot review found no P0-P2 clipping, contrast, overlap or
  misleading GND controls in the four captures. Empty non-safe positions are
  intentional; the UI remains a control-subset view.

### TUI

The current Rust release binary connected to the real board in 80x24,
120x32, 48x16 and 47x16 terminal geometries. Each run used only `g`, bounded
page navigation, and `q`; it did not drive GPIO outputs.

- All mapped labels and generic GP17/GP18 were reachable through navigation.
- J8 preceded J13; the right-edge marker was visible at the tested sizes.
- Terminal captures retained the state suffixes, and every process exited 0.
- Projection, selection/hit identity, gesture and resize behavior have the
  associated offline regression coverage. This run does not claim fresh live
  LOW/HIGH gesture or physical output-voltage measurements.

### Reboot

The final image accepted normal reboot with the 250 ms delay. Bounded 20 ms
sysfs observations recorded application device 112, disappearance, then
application device 113 at the same path; no BOOTSEL identity was observed.
This sampled observation is not a continuous USB bus trace. Post-reboot
mandatory power/PD/INA preflight and the API/telemetry checks above passed.

## Not exercised

No PD-source unplug/replug, physical BOOTSEL-button press, fault injection,
OTA cycle, external logic-pattern generator, arbitrary J8 device, or analog
electrical/leakage test was performed. The separate raw-TCP listener's fixed
subnet binding limitation remains outside this change.
