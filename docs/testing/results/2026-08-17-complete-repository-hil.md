# 2026-08-17 Complete Repository HIL

## Verdict

Complete-repository hardware-in-the-loop validation: **PASS WITH DECLARED BLOCKS**.

The repository passed every gate it could be exercised against on this single
board on 2026-08-17. The six blocked items listed in the
[Blocked Items](#blocked-items) section were not attempts; they are gaps the
host setup cannot reach without sudo, an external 3.3V pattern generator, a
voltmeter, a target board under test, or a safe watchdog fault-injection path.
This report does not claim those gaps passed, does not assert any "62/62 high
rate" claim, does not assert GP10 mapping PASS, and does not claim DORA, full
WIDE11 mapping, watchdog rollback, or VIN 1.8V measurement.

## Scope

This run exercises the full validation lane against one real board. It covers:

- The firmware-authoritative logic-analyzer matrix (bounded + continuous, TCP and
  WebSocket, all modes that do not need external stimulus): 54 cases, all
  PASS.
- The 62-case high-rate packed-burst matrix. The original run hit 29 PASS / 33
  FAIL; a runner
  partial-frame bug was fixed, and the post-fix run is 30 PASS / 32 ARMED-only
  BLOCKED plus the same runner fix. All 32 ARMED-only rows are explained by a
  single physical stimulus gap and are not a product failure.
- The WIDE11 shared-arena telemetry-isolation case, plus a separate reduced
  GP10 positive-control attempt that exposed the unavailable stimulus fixture.
- A residual GP12 anomaly observed mid-run, restored and not reproduced by an
  isolated WIDE11 rerun.
- The persistent-configuration six-flow runner with HTTP and CDC ROM BOOTSEL
  recovery (twice), using only the combined UF2.
- The OTA API negative, auto-confirm, and manual-confirm flows.
- The OTA browser auto and manual flows, plus the locator, card scoping, and
  UI-synchronization TDD fixes that unblocked them.
- The full repository gate (`make check`, Web Node tests, Vitest, Rust unit
  tests, firmware host-model C tests, Python offline tests, Nix flake check,
  repository gate contract test, test registration, shell lint).

## Target And Artifacts

- Board: Radxa Linkr Debugger, RP2350A, USB NCM HTTP at `http://172.29.203.1`.
- CDC ACM identity:
  `usb-Radxa_Radxa_Linkr_Debugger_E6641C31A362C336-if00` (enumerated as
  `/dev/ttyACM2`).
- CH347 target UART: `/dev/ttyACM1` (WCH `1a86:55de`, serial
  `BD7A3BABCD`). Used only as the UART stimulus source for WIDE11 GP10
  single-wire runs and as the optional OpenOCD path; not used for power,
  routing, or persistent configuration.
- Combined ROM BOOTSEL image:
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`, 1,642,496
  bytes, SHA256
  `67cc00fb9b769306735388554ed6205ba7926b6f1ad4fb536f4335c61c8f37a1`. This
  is the rebuilt image after the Web GPIO selector HIL exposed a layout
  metadata regression; the earlier build SHA256 is preserved in
  `artifacts.SHA256SUMS` for traceability.
- MCUboot OTA image:
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350-ota.bin`, 795,176
  bytes, SHA256
  `faf2b42a18bb199f5a41f19504a01091737ffa6a9b40e79f80c041b58195d8ee`.
- App-only `zephyr.uf2` was never flashed. Flashing it through ROM BOOTSEL
  would brick the board.

## Source State

- Source HEAD: `97b24eb959d11e9baa65895e715e0913d3039dee`.
- The worktree is intentionally dirty: `git status` reports 94 modified files
  and 161 untracked files, all reproducible from the patched source.
- The dirty state is the expected end-state of this validation. This HIL run
  added focused fixes to the logic-analyzer Python runner, OTA shell/browser
  runners, repository gate contracts, their regression tests, and current Web
  runner documentation. It did not modify the tested firmware image after the
  recorded canonical UF2/OTA artifacts were built and flashed.

## Logic Analyzer Authoritative Matrix (54/54 PASS)

`logic-authoritative.json` covers every case that does not require external
stimulus beyond what this host can supply: TCP bounded and continuous at the
declared rates, WebSocket bounded and continuous, plus WIDE11 cases that the
reduced single-wire setup can actually exercise (none in this matrix). The
breakdown by transport is:

| Transport | Cases | Passed |
| --- | --- | --- |
| TCP / direct verify | 27 | 27 |
| WebSocket | 27 | 27 |

The TCP and WebSocket legs cover SINGLE, FAST8, and WIDE11 bounded and
continuous cases at the matrix rates. `overall_pass: true`.

## Logic Analyzer High-Rate Packed-Burst Matrix (30 PASS / 32 BLOCKED)

The 62-case matrix covers HELLO capabilities, bounded SINGLE captures with
NONE/rising/either triggers and post sizes 513, 65535, 65536, and 100000,
FAST8/WIDE11 deep bursts, high-rate `post=0` capacity bursts, and lower-rate
manual STOP on TCP and WebSocket. The HELLO server_flags advertise both
`CONFIG_V2` (bit 0) and `GENERIC_PACKED_BURST` (bit 1).

The first run (`logic-high-rate.json`) finished at 29 PASS / 33 FAIL. Two
distinct failure modes sat under those 33 fails:

- 32 rows: triggered captures that armed but never observed a rising or
  either-edge transition because the only physical stimulus available on
  this host is the WCH CH347 TX line on `/dev/ttyACM1`, and both CH347 CDC
  probes leave GP10 low during the configured pre-roll window. These rows
  are ARMED-only: the firmware never reports `triggered`, never emits DATA,
  and is otherwise healthy. The arena is clean, the HTTP `/api/v1/status`
  shows no memory pressure, and the immediate-restart probe after the
  timeout succeeds.
- 1 row: the TCP runner began reading a DATA frame near the manual-stop
  deadline, consumed part of it, then discarded that partial frame on timeout.
  STOP parsing resumed in the payload and reported `bad magic 0x00`. The fix
  makes a started frame indivisible before reading the STOP response.

After the runner fix (`logic-high-rate-after-runner-fix.json`), the same
62-case matrix finished at 30 PASS / 32 ARMED-only BLOCKED. The breakdown
is:

| Trigger | Cases | PASS | BLOCKED |
| --- | --- | --- | --- |
| None (capacity burst) | 28 | 28 | 0 |
| Hello / handshake | 2 | 2 | 0 |
| Rising | 16 | 0 | 16 |
| Either | 16 | 0 | 16 |

The 30 PASS rows cover every capacity-burst case the firmware accepts
(SINGLE 100 MHz and 125 MHz, FAST8 100 MHz and 125 MHz, WIDE11 100 MHz,
WIDE11 125 MHz rejected by START as INVALID_CONFIG) plus the HELLO flag
contract and the four post-100000 SINGLE/FAST8/WIDE11 deep-bursts that
exit through the shared 98-frame arena.

The 32 BLOCKED rows are ARMED-only, not product failures. The capacity-burst
matrix is the documented lossless-or-stop contract; a row that arms, never
triggers, and times out is not evidence of overrun. Calling those 30 PASS
"62/62" would overstate the run; the report records 30 PASS plus 32
ARMED-only BLOCKED and explains both.

A second runner, exercising the manual-STOP path 10 consecutive times in
isolation on the same firmware, returned 10/10 PASS with zero
sample-index gaps, zero disconnects, and zero lost frames. The
`logic-tcp-manual-stop-fixed-10x.json` evidence records every iteration.

## WIDE11 Telemetry Isolation (PASS)

`logic-wide11-telemetry-isolation-final.json` records a WIDE11 100 MHz /
`pre=0`, `post=100000` burst with a concurrent JSON WebSocket telemetry client
at 100 Hz. This case validates shared-arena pause/resume behavior, not physical
pin mapping:

- 98 DATA frames, exactly 100,000 samples, 0 sample-index gaps.
- Baseline-epoch telemetry queued before quiesce honored the 0.05 s
  pause grace; same-epoch post-grace telemetry did not appear.
- Post-release telemetry resumed in a fresh sequence epoch with advancing
  `device_t_mono_us`. No old-epoch samples were emitted after the bounded
  grace period.

`overall_pass: true`.

## GP12 Residual Anomaly (Investigated, Restored)

Before persistence HIL, a fresh status read found GP12 at `output low` while
the original baseline recorded input. The board was explicitly restored to
input (`gp12-anomaly-restore-input.json`). An isolated WIDE11 rerun
(`logic-wide11-telemetry-isolation-gp12-toggle.json`) left all GPIOs in input,
so that WIDE11 case did not reproduce the anomaly. The exact earlier owning
action remains undetermined; the report does not classify it as a firmware or
read-side defect.

## Persistent Configuration (All Six Flows PASS)

`config-persistence-hil.sh --execute ... all` completed with exit status 0.
The runner's full assertion stream is preserved in `persistence-all.log`
and covers the six flows plus the final enumerated cleanup.

| Flow | Result | Evidence |
| --- | --- | --- |
| `safe-reboot` | PASS | Safe `switch/sd=usb-reader` and `switch/tf_wp=protected` saved and applied; both survived two consecutive cold reboots with `snapshot.version=1` and `pending=0`; final cleanup returned the baseline. |
| `dangerous-pending` | PASS | Unconfirmed `switch/usb=pc` save returned HTTP 409 `confirmation_required`; confirmed save returned HTTP 200 with `action=save`, `applied_items=["switch/usb"]`, `snapshot.version=1`, `pending=0`; two cold reboots restored `switch/usb=pc` with `apply_state=applied`; cleanup cleared the snapshot. |
| `capture-busy` | PASS | Real Sigrok TCP START at 100 kHz continuous on GP10 acquired capture; both save and present-snapshot clear returned HTTP 409 `error.code=busy,activity=capture`; STOP released ownership; the prepared snapshot survived. |
| `ota-preserve` | PASS | During a paced 64 KiB/s OTA upload, both save and present-snapshot clear returned HTTP 409 `error.code=busy,activity=ota`; upload reached `state=verified`; test boot rebooted; readiness polling passed; confirm returned `state=idle`; the prepared safe snapshot survived. |
| `bootsel-preserve` | PASS | HTTP BOOTSEL entry enumerated `/dev/sdc1` (vendor `RPI`); only the combined UF2 was copied; mount/unmount succeeded; readiness polling passed; HTTP BOOTSEL preserves the saved snapshot. |
| `cdc-fallback` | PASS | CDC `config show`, `config save power/12v_out`, `config clear`, and CDC `bootloader` all matched the primary response; clear reported `hardware_changed=false`; the CDC BOOTSEL path also used only the combined UF2 and recovered HTTP and CDC. |
| Final enumerated cleanup | PASS | All controllable power outputs off, every enumerated GPIO input, snapshot absent, `pending=0`. |

After all persistence, OTA, and BOOTSEL operations, a confirmed
`power/5v_out=on` save was applied to restore the exact original snapshot,
matching [Final Restoration](#final-restoration) below. No additional reboot
was used as part of this final restoration step.

## OTA API (Negative + Auto + Manual, PASS)

`web-ota-hil.sh --execute --allow-upload-test-reboot` ran the three
executable flows against the board:

- `negative-upload` (fixed): SHA-256 mismatch returns the firmware JSON
  error envelope, unsupported content type preserves the first error,
  and non-`.bin` artifacts are rejected by the runner before they reach
  the firmware. The original negative log had only the SHA-256 mismatch
  case; the G17 runner fix added the other two assertions.
- `api-auto-confirm`: OTA upload reached `state=verified`
  (`expected_size=795176, written_size=795176`); test boot returned
  HTTP 202 with `state=rebooting`; the firmware watchdog health gate
  auto-confirmed in under 16 seconds; final state `idle`,
  `current_image_confirmed=true`.
- `api-manual`: same upload and test boot; manual `confirm` returned
  HTTP 200 and `state=idle` after the test boot reached
  `pending_test`.

## OTA Browser (Auto + Manual, PASS)

The browser runner (`web/scripts/ota-hil.mjs`) drove the board-hosted Web
OTA card end to end. Two HIL flows (`auto`, `manual`) plus a `--flow both`
combined run produced the four final screenshots listed under
[Final OTA Screenshots](#final-ota-screenshots). The combined final log
shows:

- `initial: state=idle, confirmed=true`.
- `results.auto.confirmed: state=idle, confirmed=true`.
- `results.manual.confirmed: state=idle, confirmed=true`.
- `watchdogRollback: BLOCKED: no safe fault-injection path is available;
  not attempted`.

The earlier zero-byte logs were failures of the Playwright locator and
the card scoping, not of the firmware. The TDD fix that unblocked them
scoped the locator to the OTA card and added an explicit DOM-sync gate
(`waitForOtaCardConfirmed`) between the upload and the screenshot. A
separate manual fresh log records one manual flow without the card
scoping; that fresh log was the regression target and it passed.

The two Oracle reviews on the four final screenshots both returned PASS
with HIGH confidence; the synchronous-state synchronisation gate renders
`idle + CURRENT IMAGE confirmed` per the HIL runner gate logic and
matches the post-confirm stat changes (`EXPECTED SIZE 0 B`,
`WRITTEN SIZE 0 B`, `MAX SIZE` empty).

## Final Restoration

After all flows completed, the board was returned to its validated
baseline. The semantic comparison `final-config.json` vs `baseline-config.json`
returned `equal=true`. The single live change applied at restoration was
`power/5v_out=on`, captured in `final-restore-5v-on.json` and
`final-restore-snapshot.json`.

- HTTP `GET /api/v1/config` (`final-config.json`): `snapshot.present=true`,
  `snapshot.version=1`, `pending=0`. Only `power/5v_out` is selected;
  every other power output, every switch route, every GPIO direction, and
  every other item reports `apply_state=not_saved` and `saved=null`,
  matching the baseline snapshot selection and live values.
- HTTP `GET /api/v1/status` (`final-status.json`): `switch/sd=target`,
  `switch_usb=target`, `switch/tf_wp=writable`, `switch/vin=3.3v`; every
  controllable power output off except `5v_out=on` (the restoration
  artifact); watchdog healthy and armed; HTTP memory pressure telemetry
  available.
- HTTP `GET /api/v1/ota` (`final-ota.json`): `state=idle`,
  `current_image_confirmed=true`.
- HTTP `GET /api/v1/watchdog` (`final-watchdog.json`): `supported=true`,
  `automatic=true`, `healthy=true`, `armed=true`,
  `bootloader_on_timeout=true`.
- Host: no owned runner process, no capture holder, no Playwright or
  Chromium process, no port 5556 listener, no tmux server, no
  `RPI-RP2` block device, no BOOTSEL mount.

## Final Gates

| Gate | Status | Evidence |
| --- | --- | --- |
| `make check` | PASS | `final-make-check.log` records the final complete local validation wave. |
| Python offline tests | PASS | `Ran 88 tests` followed by `OK`. |
| Web Node tests | PASS | `# tests 337`, `# pass 336`, `# skipped 1`, `# fail 0`, `# suites 25`. |
| Vitest | PASS | `Test Files 36 passed (36)`, `Tests 285 passed (285)`. |
| Rust `cmd-ng` | PASS | `test result: ok. 361 passed; 0 failed`. |
| Repository gate contract (G17) | PASS | Focused gate suite: 53 passed, 0 failed; checker exit 0. |
| Nix flake check | PASS | `final-nix-flake-check.log` ends with `all checks passed!`. |
| Test registration | PASS | `node scripts/check-test-registration.mjs --root .` returned exit 0 with no orphaned tests. |
| `shellcheck` / `sh -n` | PASS | No findings for the changed OTA shell runner. |
| LSP errors | PASS | Zero LSP errors on every source file changed during this HIL continuation. |

The local validation lane is distinct from this board HIL. Future runs that
change the firmware behavior must rerun this board HIL; local gate PASS is
not a substitute.

## Blocked Items

These items were not attempted; each entry lists the reason the host
cannot reach it. They are not product failures and they are not claimed
PASS.

| Item | Status | Reason |
| --- | --- | --- |
| DHCP DORA option 114 capture | BLOCKED | The host did not authorize sudo or temporary NetworkManager/dhclient takeover of the NCM interface, so a fresh DHCP DORA exchange could not be captured. Captive portal HTTP redirects, `/captive-portal/api`, POST rejections, and the local-only network policy were verified separately. |
| MASKROM / EDL | BLOCKED | No target board was attached, so no MASKROM download or EDL recovery could be exercised. |
| Full WIDE11 independent high-state mapping | BLOCKED | The host has no external 3.3V pattern generator. The reduced on-site WIDE11 setup only validates GP10 DATA bit0 and the GP11-GP20 zero mask; independent high-state mapping for GP11-GP20 and lane-B alignment require the documented external generator and are not exercised here. |
| Current GP10 UART-triggered subset | BLOCKED | With both CH347 CDC probes holding GP10 low at the arm boundary, the high-rate triggered matrix cannot observe a rising or either-edge transition on this host setup. The 32 ARMED-only rows in the high-rate matrix are explained by this gap; the firmware is otherwise healthy. |
| VIN 1.8V measurement | BLOCKED | No voltmeter and no target known to be 1.8V-compatible. Switching VIN to 1.8V without a measurement setup risks damaging the target. VIN remains at 3.3V. |
| Watchdog fault injection / rollback | BLOCKED | No safe fault-injection path exists for this hardware. The firmware watchdog recovery path is exercised through ordinary readiness polling only; rollback after a watchdog reset before auto-confirm completes is recorded as BLOCKED in the OTA browser runner output and is not attempted here. |

State: no newly discovered dangerous value was confirmed during this run.

## Final OTA Screenshots

The four final screenshots from `web/scripts/ota-hil.mjs --execute --flow both`
are archived at
`.omo/evidence/20260817-complete-repository-hil/screenshots/ota-browser/`:

| File | Description |
| --- | --- |
| `ota-hil-auto-verified.png` | Auto flow, captured at `verified` after upload validation. |
| `ota-hil-auto-confirmed.png` | Auto flow, captured at `idle + CURRENT IMAGE confirmed` after the watchdog auto-confirm gate. |
| `ota-hil-manual-verified.png` | Manual flow, captured at `verified` after upload validation. |
| `ota-hil-manual-confirmed.png` | Manual flow, captured at `idle + CURRENT IMAGE confirmed` after the manual confirm click. |

The full SHA-256 set is in
[`2026-08-17-complete-repository-hil.SHA256SUMS`](2026-08-17-complete-repository-hil.SHA256SUMS).

## Evidence Files

| File | Purpose |
| --- | --- |
| `logic-authoritative.json` | Authoritative matrix: 54/54 PASS. |
| `logic-high-rate.json` | Original high-rate matrix: 29/62 PASS (post-runner-fix identified the partial-frame race). |
| `logic-high-rate-after-runner-fix.json` | Post-fix high-rate matrix: 30/62 PASS, 32 ARMED-only BLOCKED. |
| `logic-tcp-manual-stop-fixed-10x.json` | 10 consecutive manual STOP iterations, all PASS. |
| `logic-wide11-telemetry-isolation-final.json` | WIDE11 telemetry isolation case, 100000 samples / 98 frames / 0 gaps, fresh-epoch PASS. |
| `logic-wide11-telemetry-isolation-gp12-toggle.json` | Isolated WIDE11 rerun that did not reproduce the GP12 anomaly. |
| `logic-gp10-positive-control.json` | Single positive-control attempt that did not observe a trigger event. |
| `persistence-all.log` | Persistent-configuration six-flow runner assertion stream. |
| `persistence-capture-holder.json` | Capture-arbiter ownership record during `capture-busy`. |
| `persistence-final-config.json`, `persistence-final-status.json`, `persistence-final-ota.json` | Persistence-flow end state. |
| `ota-api-negative-fixed.log`, `ota-api-negative.log` | OTA API negative-upload assertions (original and post G17-fix). |
| `ota-api-auto.log`, `ota-api-manual.log` | OTA API auto-confirm and manual-confirm flows. |
| `ota-browser-both-final.log`, `ota-browser-manual-fresh.log` | OTA browser combined and manual-fresh flows. |
| `final-config.json`, `final-status.json`, `final-ota.json`, `final-watchdog.json` | Independent post-run HTTP queries. |
| `final-restore-snapshot.json`, `final-restore-5v-on.json` | Restoration: confirmed `power/5v_out` save/apply. |
| `baseline-config.json`, `baseline-status.json`, `baseline-ota.json`, `baseline-watchdog.json`, `baseline-tasks.json`, `baseline-task-blob.txt` | Pre-run baseline snapshots used for the `equal=true` semantic comparison. |
| `dhcp-dora-option114.blocked.json` | DORA gap record (no sudo / interface takeover). |
| `final-make-check.log`, `final-nix-flake-check.log`, `make-firmware.log`, `make-gates-after-g14.log`, `make-gates-after-g15.log` | Final repository/Nix gates plus the canonical firmware build and earlier focused gate logs. |
| `cmd-ng-release-build.log` | Host CLI release build log. |
| `artifacts.SHA256SUMS`, `artifacts-after-gpio-fix.SHA256SUMS`, `artifact-stat.txt` | Build artifact identity and size records. |
| `source-head.txt`, `source-status.txt`, `source-manifest.SHA256SUMS`, `source-manifest.identity`, `source-worktree.patch` | Source state, dirty worktree, and source manifest. |
| `lsusb-before.txt`, `udev-serial-before.txt`, `ip-address-before.txt`, `ip-route-before.txt`, `multihomed-route-check.txt` | USB, CDC, and network identities captured before the run. |
| `capport.headers`, `capport.json`, `captive-redirects.txt`, `http-post-rejections.txt`, `dns-a.txt`, `dns-aaaa.txt`, `capport.headers` | Captive-portal HTTP, DNS, and POST-rejection evidence. |
| `adc-1000hz.ndjson`, `adc-10hz.ndjson`, `adc-client-1..4.ndjson`, `ws-adc-shapes.json`, `ws-five-second-summary.json`, `ws-five-second-timeline.jsonl` | ADC telemetry and live WebSocket evidence. |
| `web-gpio-hil.json` | Board-hosted Web GPIO selector HIL. |
| `gp12-anomaly-restore-input.json`, `gp29-output-rejection.json`, `gp29-output-rejection-fixed.json`, `gp29-ws-input-only.json` | GPIO contract checks. |
| `gpio-mutations.log`, `power-mutations.log`, `switch-mutations.log` | Mutation audit trails. |
| `web/scripts/ota-hil.mjs` OTA screenshots under `screenshots/ota-browser/` | Final Web OTA visual evidence. |

The full SHA-256 set for this report and the evidence above is in
[`2026-08-17-complete-repository-hil.SHA256SUMS`](2026-08-17-complete-repository-hil.SHA256SUMS).
