# 2026-07-31 ADC3 Telemetry HIL

## Verdict

ADC3 compact telemetry and GP29 ownership HIL: **PASS** for the
directly-observed evidence in this run. This covers the real-board subset of
HIL spec sections 4b.1 through 4b.5 (direct GP29 ownership only) and the
combined-UF2 dual recovery guards from sections 9 and 10. The historical v1
snapshot compatibility subcases of 4b.5 are kept `pending` because this run
did not exercise them. Rolling nightly checks (HIL spec section 12h) remain
`pending`; this run did not run any scheduled or manual nightly, and the
nightly channel is intentionally separate from real-hardware HIL.

No standalone raw JSON or serial transcript was checked into the repository
for this run. The evidence below is exactly the assertions performed against
the board; attempt artifacts that were not kept (raw HTTP response bodies,
per-message WebSocket frames, captured CDC transcripts) are not invented or
back-filled in this report.

## Target And Artifacts

- Board: Radxa Linkr Debugger, RP2350A, USB NCM HTTP at
  `http://172.29.203.1`.
- Session provenance: OpenCode `ses_04c273732ffeB04ACYukBLJxGY`.
- Canonical build footprint:
  - Flash: 734652 / 847832 bytes (86.65%).
  - RAM: 494272 / 532480 bytes (92.82%).
  - Combined UF2: 1521152 bytes,
    SHA256 `9d64df4bba89d6d8b78fa94d6bb0df134e9381b45df8e0ceee3b05cd4f9d8c48`.
  - OTA bin: 734692 bytes,
    SHA256 `fb68a90bce315e7ed2f3b236b128b1fc37df6afb010c50b9d3e95e4f0084284f`.
- Earlier baselines remain historical only:
  605476 / 847832 (71.41%, no A/B), 694920 / 847832 (81.96%) with
  515672 / 532480 (96.84%), 701900 / 847832 (82.79%) with
  475896 / 532480 (89.37%) and combined UF2 1455616 bytes,
  695868 / 475832 / 1443840 (freeze build). Treat the dated-report numbers
  as evidence of that dated build, not as a present-day claim.
- Recovery path: the combined `radxa-linkr-debugger-rp2350.uf2` is the only
  image used for both BOOTSEL recoveries. The app-only
  `zephyr.uf2` was **not** flashed in this run.

## Validation

### ADC3 HTTP rich read shape (HIL spec 4b.1)

- `GET /api/v1/adc/read` returned a readings array of exactly four entries in
  the fixed order `5v_out, 12v_out, 20v_out, adc3`.
- For the current channels, `sensor_channel="current"`, `unit="A"`,
  `current_ua` is a signed integer in µA, and `power_enabled` is a bool.
- For `adc3`, `sensor_channel="voltage"`, `unit="V"`, no `value`,
  no `current_ua`, no `power_enabled`. The `raw`, `mv`, and `sensor_value`
  fields are present on the rich HTTP shape so the host can cross-check
  against the firmware ADC pipeline.
- ADC3 sample observed in this run on the rich HTTP shape:
  `raw=390`, `mv=314`, `sensor_value` decoded to 314208 µV. The host
  receives the firmware's direct reading without applying any host-side
  ADC calibration or zero-point correction.

### ADC3 WebSocket compact single-sample shape (HIL spec 4b.2)

- A 10 Hz `topic=adc` subscription produced compact single-sample frames
  whose `readings` array carries exactly four entries in the same fixed
  order.
- Each reading's `value` is a signed integer in its `unit` (`uA` for the
  current channels, `uV` for `adc3`).
- `power_enabled` is present only on the three current channels; `adc3`
  does not carry `power_enabled`.
- The compact single-sample frame does not carry `raw`, `mv`,
  `current_ua`, or `sensor_value` on any reading.

### ADC3 WebSocket compact batch shape (HIL spec 4b.3)

- A 250 Hz compact batch subscription produced `telemetry-batch` frames
  whose `channels` array declares exactly four entries, with `adc3`
  carrying `kind="voltage"` and `unit="uV"`.
- Each `samples[].values[]` entry is a single signed integer, so
  `values.len() == channels.len()` (four scalar values per sample).
- The compact batch frame does not carry `raw`, `mv`, `current_ua`, or
  `sensor_value` per sample.

### Web sparkline 10 Hz / 90-sample (HIL spec 4b.4)

- The Power & measurements card on the board-hosted Web UI rendered the
  shared 10 Hz / 90-sample sparkline scope. Live ADC3 reached exactly 90
  points in the SVG history after the rolling window filled, at both
  1280 px and 375 px viewports, with zero horizontal overflow and no
  browser console errors.
- The two captured screenshots are
  `/tmp/opencode/adc3-scope-desktop-zh.png` (1280 px) and
  `/tmp/opencode/adc3-scope-mobile-zh.png` (375 px). They are local
  evidence under `/tmp/opencode/` and are not checked into the repository.

### GP29 ADC3 ownership (HIL spec 4b.5, direct ownership only)

- `GET /api/v1/gpio` and `GET /api/v1/gpio/GP29` list GP29 in the
  persisted/safe catalog as expected.
- A `PUT /api/v1/gpio/GP29` with `{"direction":"output","value":1}` was
  rejected at the firmware layer with HTTP 500 and
  `error.code=configure_failed`, surfacing the firmware-owned input-only
  rule rather than retrying or silently coercing the request.
- A `PUT /api/v1/gpio/GP29` with `{"direction":"input"}` was idempotent
  and did not return an error.
- The historical v1 snapshot compatibility subcases of 4b.5 (v1 input
  snapshot decode + safe auto-restore, and v1 output snapshot partial-apply
  stopping at GP29 with later entries still `pending`) were **not** exercised
  in this run and remain `pending` against a future real-board HIL.

### Combined-UF2 dual BOOTSEL recovery (HIL spec 9 and 10 guards)

- HTTP `POST /api/v1/bootloader` entered ROM BOOTSEL; the board re-enumerated
  as the `RPI` vendor block device and was restored by writing the
  combined `radxa-linkr-debugger-rp2350.uf2` to it.
- CDC ACM shell `bootloader` command entered ROM BOOTSEL from the same
  board; the combined UF2 was the only image used for recovery.
- `picotool load` was attempted but lacked permission in this environment;
  the successful recoveries used the `udisksctl` mount path on Linux and a
  plain copy of the combined UF2 into the mounted `RPI-RP2` partition. The
  app-only `zephyr.uf2` was not used in either recovery path.
- After both recovery paths, post-recovery ADC3 HTTP and WebSocket smoke
  passed (compact single-sample frame at 10 Hz and the rich HTTP `adc3`
  reading decoded as a signed integer µV).

## Explicitly Out Of Scope

- No post-token firmware sysbuild, reflash, or board HIL occurred. The
  firmware footprint, combined UF2 size, SHA256 digests, OTA bin size, and
  OTA SHA256 recorded above are the pre-token build; the post-token Web-only
  one-line CSS source change was not reflashed, so it does not influence
  the figures above.
- The historical v1 snapshot compatibility subcases of HIL spec 4b.5 were
  not exercised in this run and remain `pending`.
- Rolling nightly checks (HIL spec section 12h) are not addressed by this
  run. The nightly channel is intentionally separate from real-hardware
  HIL; this report does not claim that any scheduled or manual nightly
  ran.
- The dated reports under `doc/testing/results/2026-07-25`,
  `2026-07-26`, `2026-07-27`, `2026-07-28`, and the three
  `2026-07-30` reports remain unchanged. This report is additive to that
  set, not a replacement for any earlier HIL evidence.

## Post-HIL Web-Only Local Production-Build Validation Addendum

The post-token Web-only one-line change
(`web/src/components/PowerSparkline.tsx` `text-[9px]` to `text-[11px]`)
was not reflashed, but its local production build and visual QA pass
succeeded. The evidence is local-only and does not constitute board HIL.

- Local Node test counts: `npm --prefix web test` passed 134 Node tests
  and 17 script tests; the Vitest run passed 74 tests.
- `npm --prefix web run build` completed successfully; the production
  build under `web/dist/` is newer than the source files exercised by
  the harness.
- Visual QA evidence: `/tmp/opencode/adc3-final-visual-qa/evidence.json`
  recorded the local run with a fully mocked API/WS stack. Four sparklines
  (three current rows plus the ADC3 row) each held exactly 90 points at
  viewports 375, 768, and 1280 px, sampling 100 frames at 10 Hz over
  9913 ms. All endpoint and footer labels rendered at 11 px. No overlap,
  clipping, overflow, console error, or request to the board was
  observed, and the harness shut down cleanly.
- Independent Oracle passes reviewed the same evidence:
  - Pass A: `bg_75f868d2`, session
    `ses_04b2ebb53ffeSj0Rvb9fAB3Z8h`, verdict PASS high.
  - Pass B: `bg_9a66d0b2`, session
    `ses_04b2e8d58ffeD3yAJczUCnA93j`, verdict PASS high.
- This addendum validates the Web-only token fix in a mocked local
  production build. It does not extend this report's firmware HIL
  verdict: no post-token firmware sysbuild, reflash, or board HIL
  occurred. The pre-token firmware footprint and digests above remain
  the figures of record.
- The `/tmp/opencode/` artifacts listed above are local evidence and are
  not checked into the repository.

## Evidence Files

- This report: `doc/testing/results/2026-07-31-adc3-telemetry-hil.md`.
- No standalone raw JSON, serial transcript, or `SHA256SUMS` file was
  checked in for this run. The firmware/OTA identities above are the
  canonical build artifacts at the recorded sizes and SHA256 digests; the
  two screenshots listed under HIL spec 4b.4 live under
  `/tmp/opencode/` and are local evidence, not repository artifacts. The
  post-HIL local visual QA evidence lives under
  `/tmp/opencode/adc3-final-visual-qa/` and is also local-only.
