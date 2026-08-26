# 2026-08-26 Web GPIO Final HIL

## Verdict

Real-board Web GPIO functional HIL: **PASS**.

Release acceptance: **FAIL**, only because the protected process postcondition
`ps -p 3731563 -o pid=` was empty. The run did not create, reuse, probe, replace,
signal, or attach to PID `3731563` or `pts/1`.

Visual QA: **PASS with a recorded reviewer disagreement**. One independent
review returned PASS with high confidence and treated the mobile fixed status
strip as intentional because the source reserves bottom space. The second
review returned REVISE for the same fixed-strip overlap interpretation. DOM
geometry showed no document overflow, no control characters, and complete GPIO
coverage; this remains an observation for a future UI review, not a functional
HIL failure.

## Source And Artifacts

- Board: G3 / RP2350A, board-hosted page at `http://172.29.203.1/`.
- Source HEAD: `a03ef6206d38fe797d3e874c37d7431f263a9ca5`.
- Release identity: `v0.2.1-188-ga03ef62`.
- Tracked diff SHA-256: `7337b6d0d9b70c1cbaa0094f87467d1884d547df6e2b8264192e70e0d4fff8df`.
- Current source manifest SHA-256: `e3e6934055c1b1c3a945b94020f831bcec3a1a8575312291223c8ddfe45162b5`.
- Canonical command: `make firmware`; build completed successfully.
- Combined MCUboot plus application UF2: 1,594,880 bytes,
  SHA-256 `086d3cbd9ff13f41f820e811adf85915392b1b462e2f7c6e3744d88b45420724`.
- OTA BIN: 771,376 bytes, SHA-256
  `dab119255c7464a9d8d7a78e713d250d7dd7ea9e543190f173dfe8cd31e1c1b2`.
- Application-only `zephyr.uf2` was recorded for identity only and was never flashed.

## Starting Baseline

The initial read-only pre-flash state had all advertised GPIOs at `input/0`.
The requested release baseline requires GP10 `output/0`, so one explicit safe
preparation write set GP10 to `output/0`; all other safe GPIOs remained `input/0`.
The prepared baseline used for equality was:

- Power: `5v_out=on`, `vdd_5v=on`, `12v_out=off`, `20v_out=off`.
- Switches: `sd=usb-reader`, `usb=pc`, `tf_wp=writable`, `vin=3.3v`.
- Persistent configuration: v1 snapshot present, `pending=0`, six selected
  entries (`power/5v_out`, `power/vdd_5v`, `switch/sd`, `switch/usb`,
  `switch/tf_wp`, `switch/vin`); no Save or Clear was issued.
- Tasks: zero stored tasks and empty task blob; immutable catalog preserved.
- Watchdog: supported, healthy, armed, 5,000 ms timeout.
- OTA: idle, current image confirmed.

## Flash And Startup

HTTP BOOTSEL entry succeeded. The device was discovered by the strict `RPI`
vendor match, mounted with `udisksctl`, and only
`build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2` was copied.

The first post-flash boot later entered ROM BOOTSEL through the watchdog. No
application-only image was used. The same combined UF2 was dynamically copied
again; HTTP then remained available across 12 consecutive one-second probes,
and the watchdog reported healthy/armed.

## Board-Hosted Browser HIL

The Playwright runner used real Chromium, the board-hosted origin, same-origin
HTTP `/api/v1`, and no request interception or mock API. The bounded live
WebSocket created a real session, subscribed to `live`, received a status
snapshot, and was explicitly deleted.

The final runner produced 30/30 assertions passing:

- Idle GPIO surface issued zero GPIO PUTs.
- GP13 short press issued exactly one output LOW after the 220 ms window.
- GP13 double short press issued exactly one input request and no LOW request.
- GP13 hold issued exactly one output HIGH at the 600 ms threshold; release was inert.
- A pointerup at the 600 ms boundary still issued exactly one HIGH request.
- Movement beyond 8 px and pointercancel each issued zero writes.
- Real network latency exposed the static pending ring; all pins were disabled and
  a second GPIO gesture was dropped while the first request was pending.
- GP29 output was rejected by firmware with HTTP 403 `input_only`; GP29 remained input.
- Every successful action was followed by authoritative board readback.

## Responsive And Visual Evidence

Fresh full-height screenshots were captured for desktop, tablet, and mobile in
English and Chinese:

- `desktop-en.png`, `desktop-zh.png`: 1280 px viewport.
- `tablet-en.png`, `tablet-zh.png`: 768 px viewport.
- `mobile-en.png`, `mobile-zh.png`: 375 px viewport.

All six PNG signatures and dimensions are valid. Geometry checks report
`scrollWidth == viewport width`, zero visible overflow entries, zero control
characters, correct `en`/`zh-CN` document language, and all 15 GPIO controls.
The browser recorded no page errors. The sole console 403 is the expected GP29
firmware rejection and is independently matched in `network.json`.

Two independent read-only visual reviews were run. The layout-integrity review
returned PASS with 0.94 confidence. The CJK-focused review returned REVISE
because it interpreted the intentional fixed status strip as mobile overlap;
the source's `pb-16` reservation and zero document overflow support the
layout-integrity interpretation.

## CDC And BOOTSEL Coverage

- CDC `vin get` before the browser run returned `vin=3.3v`.
- CDC `bootloader` returned `Entering rp2350 BOOTSEL in 250 ms...` and the serial
  device disconnected as expected.
- The CDC BOOTSEL recovery copied the same combined UF2; source and mounted-copy
  SHA-256 values matched.
- HTTP returned after CDC recovery and CDC `vin get` again returned `vin=3.3v`.
- No RPI BOOTSEL block device or mount remained at final cleanup.

## Final Cleanup

After CDC recovery, GP13 was restored to input and GP10 to output/0. Final
independent reads proved:

- `status` projection equals the prepared baseline.
- `config` equals the prepared v1 snapshot and `pending=0`.
- Stored tasks and task catalog equal their baseline captures.
- GP10 is `output/0`; every other safe GPIO is `input/0`.
- Power, switch routes, and VIN equal the prepared baseline.
- Watchdog is healthy and armed; OTA is idle and confirmed.
- No RPI block device remains; PID `3731563` remains absent.

The machine-readable result is `final-comparison.json` with `final: true`.

## Offline Evidence

- Canonical firmware build: PASS, `make-firmware.log`.
- Web canonical entry: Node `392 passed, 1 skipped`; Vitest `430 passed`.
- Runner source syntax and LSP diagnostics: clean.

## Evidence Files

All evidence is under `.omo/evidence/20260826-web-gpio-final-hil/`:

- Source/build: `source-head.txt`, `release-identity.txt`,
  `source-file-hashes.SHA256SUMS`, `tracked-diff.SHA256`,
  `source-manifest.SHA256`, `make-firmware.log`, `artifacts.SHA256SUMS`.
- Baseline/final state: `baseline-*.json`, `prepared-baseline-*.json`,
  `final-*.json`, `final-comparison.json`, `post-cleanup.json`.
- Browser: `report.json`, `network.json`, `websocket.json`, `console.json`,
  `console-audit.json`, `screenshots.json`, six viewport PNGs, and gesture-state PNGs.
- Recovery: `http-bootsel.log`, `copy-combined-uf2.log`,
  `cdc-bootsel.log`, `cdc-bootsel-copy-combined-uf2.log`,
  `cdc-vin-get-before-hil.txt`, `cdc-vin-get-after-bootsel.txt`.
- Integrity: `screenshots.SHA256SUMS`, `core-evidence.SHA256SUMS`, and the
  final `evidence.SHA256SUMS` manifest.
