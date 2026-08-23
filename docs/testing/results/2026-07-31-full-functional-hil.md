# 2026-07-31 Full Functional and HIL Report

Overall verdict: **FAIL**. The end-to-end run produced extensive real-board
passes across firmware, host CLI, WebSocket telemetry, captive portal, Web
UI, logic analyzer, persistence, OTA, and GP29 v1 input snapshots, but it
also surfaced nine concrete product or test-harness failures that the run
itself could not clear and several environment-bound items that are recorded
as BLOCKED. The run is not CI/HIL-ready. Board, artifacts, and host were
returned to a documented safe state.

Artifact warnings remain in force: the only valid ROM BOOTSEL image is
`build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`; the app-only
`build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.uf2` was
never flashed. During this run the combined UF2 was the artifact flashed
through ROM BOOTSEL; the MCUboot-format OTA bin was uploaded through the
`/api/v1/ota/upload` path and never reflashed via BOOTSEL.

## Target and artifacts

- Board: Radxa Linkr Debugger, RP2350A, USB NCM at `http://172.29.203.1`.
- RP2350 serial: `E6641C31A362C336` (CDC by-id
  `/dev/serial/by-id/usb-Radxa_Radxa_Linkr_Debugger_E6641C31A362C336-if00`,
  dynamically enumerated as `/dev/ttyACM2`).
- Combined ROM BOOTSEL image: 1,521,152 bytes, SHA256
  `dfe3a1f62efd097ace5e85cb131e79f9869a1bd88d7cf3231787bed822d7cc63`.
- MCUboot OTA image: 734,656 bytes, SHA256
  `e647bf5d5ee65ba659f365227514c0101b82a845cae17e2e9cee7f8701fe087e`.
- App-only `zephyr.uf2`: forbidden, never flashed.
- App flash 734,616 of 847,832 (86.65%), app RAM 494,272 of 532,480
  (92.82%), MCUboot flash 25,856 of 65,536 (39.45%), MCUboot RAM 19,528
  of 532,480 (3.67%).

Evidence roots:

- `/tmp/opencode/full-functional-hil-20260731/canonical-build/11-artifact-identities-final.log`
  and `12-final-decision.log` (canonical sysbuild PASS, 243 input files,
  input manifest SHA `97e436ed22212f3d6aacc89dd43da0eed8f227e506d84894d867faa3e74fa1e0`,
  aggregate manifest SHA `046769b41eb68ae7ea711f99738055ccede93cc82a0826a2dcbde7787343727f`).
- Per-phase verdict and evidence dirs under
  `/tmp/opencode/full-functional-hil-20260731/{local-gates,canonical-build,core-hil,browser-hil,browser-apply-clear-hil,logic-hil,persistence-hil,ota-api-hil,ota-browser-hil,gp29-v1-snapshot-hil,final-safety-audit}`.

## Phase matrix (PASS / FAIL / BLOCKED)

| # | Phase | Result | Headline evidence |
| - | ----- | ------ | ----------------- |
| 1 | Local gates (refreshed) | PASS 33/33 | `local-gates/49-local-gates-refresh-decision.log`, `44-local-gate-matrix.tsv` |
| 2 | Canonical sysbuild | PASS | `canonical-build/12-final-decision.log`; combined UF2 + OTA identities match |
| 3 | Initial udisks mount race + bounded recovery | PASS after recovery | `core-hil/flash-mount.log` (initial mount failed `Error looking up object for device /dev/sdc1`); `core-hil/flash-recovery-copy.log` (mount `/run/media/chen/RP2350`, copy_exit=0); `flash-recovery-readiness.log` (HTTP/CDC ready); `flash-recovery-cdc.log` (`/dev/ttyACM2`); post-recovery `flash-recovery-status.json` shows safe state; the stale `flash-final-cleanup-verify.log` `board_http_unavailable_during_cleanup` is post-reboot and is not the final flash verdict |
| 4 | Core control (HTTP/CLI/TUI/power/ADC/switch/GPIO/watchdog/CDC) | FAIL (73 PASS / 2 FAIL / 1 BLOCKED) | `core-hil/control/run-20260731T051957Z-602457/summary.tsv` (76 cases), `final-verdict.json` (`CORE_CONTROL_HIL: FAIL`, counts `{PASS:73,FAIL:2,BLOCKED:1}`); all runnable HTTP/CLI/TUI/power/ADC/switch/GPIO/watchdog/CDC cases passed; the two failures are `status_body_length` (>4096 contract) and `memory_ws_phase2` (WS status snapshot cadence); the single BLOCKED is `gpio25_heartbeat_visual` (no real visual observation input available, HTTP watchdog health recorded separately); `final_cleanup.{config,status,power,gpio,ota}.json`; dynamic CDC by-id; raw ANSI-interleaved CDC bytes retained at `core-hil/control/run-20260731T051957Z-602457/cdc-serial.log` |
| 5 | Realtime / WebSocket telemetry low-rate 3 records | PASS | `core-hil/realtime-network/low-rate-3-validation.json` |
| 6 | Realtime / WebSocket telemetry high-rate 1000 records | PASS | `core-hil/realtime-network/high-rate-1000-validation.json` (3,351–21,057,734 ns host interval; 900–2,100 µs device interval; `dropped_samples=204` on row 0 is the expected ring-overrun sentinel) |
| 7 | Four concurrent WebSocket clients | PASS | `core-hil/realtime-network/four-client-concurrency.json` |
| 8 | Captive portal (HTTP 200, AAAA NOERROR, 302 redirects, 405) | PASS for HTTP/AAAA/302/405 | `core-hil/realtime-network/capport-network-validation.json` |
| 9 | Wildcard DNS A | FAIL | `capport-network-validation.json` returns `198.18.0.41`, not `172.29.203.1` |
| 10 | DHCP DORA option 114 | BLOCKED | `core-hil/realtime-network/dhcp-dora-blocked.json` — sudo requires a password, so the documented exclusive NM handoff + tcpdump + dhclient DORA were not attempted |
| 11 | Final cleanup verdict | FAIL (overall) / PASS (HTTP/WS) | `core-hil/realtime-network/final-cleanup-verdict.json` reports `low_rate_verdict=PASS`, `high_rate_verdict=PASS`, `four_client_verdict=PASS`, `network_verdict=FAIL`, `dhcp_verdict=BLOCKED`, `cleanup_failures=[]`, `safe_state=ok`; `overall_verdict=REALTIME_NETWORK_HIL: FAIL` |
| 12 | Browser UI: layout/structure/overflow/State-A/State-B/State-C/state-2b/telemetry/LA panel/serial setup dialog (except position)/bridge | PASS (15/20 sub-cases) | `browser-hil/VERDICT.md` |
| 13 | Browser Web Serial setup dialog positioning | FAIL | `browser-hil/VERDICT.md` row 17: dialog not portaled to body; ancestor grid keeps `transform: matrix(1,0,0,1,0,0)`; at 375x812 dialog renders at y=1108 (viewport 812) unreachable, at 1280x900 dialog bottom clipped (y=575..1064); violates `DESIGN.md` S4 portal contract |
| 14 | Apply-success clears selection (broad browser HIL) | BLOCKED | `browser-hil/VERDICT.md` row 11 — firmware marks saved items `applied`; `apply_state=pending` only from boot-skip(requires_confirm) or failed apply; UI gates Apply on `pending\|failed`; not reachable with safe `tf_wp` |
| 15 | Browser Apply-clear real pending flow | PASS 16/16 | `browser-apply-clear-hil/VERDICT.md`; dangerous `switch/usb=target` (firmware-confirmed), Save -> cold reboot -> re-apply; 3 DOM samples across 1.2 s during the held 200 response: `aria-checked=true` + tint every sample; cleared at 09:32:25.715Z, 110 ms after the 09:32:25.605Z release, after the authoritative GET |
| 16 | Logic analyzer authoritative 54/54 TCP+WS | PASS | `logic-hil/authoritative/VERDICT.md`, `runner-validation.json` (36 bounded + 18 continuous, 793 DATA frames, 1,496,750 samples, zero gaps, zero disconnects, 4 capacity-stop-before-data) |
| 17 | Logic analyzer special/deep (high-rate 62/62, telemetry isolation, reduced GP10 mapping) | PASS | `logic-hil/special-deep/VERDICT.md` |
| 18 | Logic analyzer stability/recovery (WS SINGLE 1MHz sustained 5s + owner exclusion + 6 trigger captures + forced RST + native sigrok) | FAIL | `logic-hil/stability-recovery/VERDICT.md`; see findings 5 and 6 below |
| 19 | Persistence 6-flow runner `all` | PASS | `persistence-hil/VERDICT.md` (115 s, exit 0) and `runner.raw.jsonl` (85 assertions) |
| 20 | OTA API/CLI: auto-confirm / manual-confirm / direct full SHA mismatch / direct full unsupported Content-Type / bounded interrupted upload | PASS | `ota-api-hil/CASE_MATRIX.md`, `api-auto-confirm.log`, `api-manual-confirm.log`, `direct-sha-mismatch.*`, `direct-unsupported-content-type.*`, `interrupted-upload.*` |
| 21 | OTA API `web-ota-hil.sh --flow negative-upload` | FAIL | `ota-api-hil/VERDICT.md` finding 1: shell hard-coded 5 s timeout aborted the 734,656-byte bad-SHA upload; firmware reported `upload_aborted`; direct full bad-SHA separately passed |
| 22 | OTA current-source CLI non-`.bin` extension guard | FAIL | `cmd-ng/src/app.rs:610-630` `prepare_ota_upload` checks only `metadata.is_file()` and `size != 0` then computes SHA and prepares `/api/v1/ota/upload`; no extension guard. Current-source empty `.uf2` test in `ota-api-hil/current-cli-invalid-extension/` produced `invalid_file` ("OTA image ... is empty") only because `size == 0`; `cli.exit=2`, `network-check.log` reports `network_syscalls=absent upload_path_observed=false`, `state-check.log` reports `temp_removed=true ota_state_unchanged=true`, `ota-after.json` shows `ota_state=idle`. This proves the empty-file/no-network check; it does not prove extension-specific rejection, so the `.bin` extension-validation contract is not satisfied. |
| 23 | OTA browser auto flow | PASS | `ota-browser-hil/runner-auto/runner-stdout.json` (`passed:true`), `ota-state-timeline.log` |
| 24 | OTA browser manual flow (repo runner) | FAIL | `ota-browser-hil/runner-manual/runner-stderr.txt` and `runner-manual-retry/runner-stderr.txt`: `Confirm image did not enable in pending_test` (10 s enable budget exceeded twice) |
| 25 | OTA browser instrumented manual flow | PASS | `ota-browser-hil/instrumented-manual/evidence.json`; Confirm enabled at +9,184 ms after `pending_test`, clicked before the 16 s auto-gate; final idle+confirmed via browser action |
| 26 | GP29 v1 input snapshot | PASS | `gp29-v1-snapshot-hil/VERDICT.md` (firmware-enumerated `gpio/GP29` saved, v1 decoded and auto-restored as input/value 0; clear without hardware change) |
| 27 | Legacy GP29 v1 output snapshot | BLOCKED | no safe injection path |
| 28 | OTA watchdog rollback | BLOCKED (multiple phases) | no safe liveness fault-injection path before the 16 s health gate |
| 29 | DHCP option 114 / full multihomed validation | BLOCKED | sudo required; see phase 10 |
| 30 | Physical GPIO25 LED observation | BLOCKED | no physical observation performed |
| 31 | Physical VIN 3.3 V / 1.8 V measurement | BLOCKED (1.8 V conditional) | only firmware `switch get vin` and `3.3v` default; 1.8 V is conditional and intentionally not executed |
| 32 | External 4-bit WIDE11 generator mapping | BLOCKED | no external 3.3 V generator available; `external-generator-mapping.blocked.json` |
| 33 | Native sigrok-cli driver | BLOCKED | `native-sigrok-scan.stderr`: `Driver linkr-debugger not found` in packaged libsigrok |
| 34 | Web Serial chooser / device selection / UART RX-TX | BLOCKED | browser security chooser must not be bypassed; no human/echo source |
| 35 | Mobile LA / 768 px Serial dialog visual captures | BLOCKED | Oracle reported incomplete evidence |
| 36 | Rolling nightly GitHub Actions execution | BLOCKED | outside this local HIL scope; not claimed |
| 37 | Final safety audit | PASS | `final-safety-audit/VERDICT.md` (`FINAL_SAFETY_AUDIT: PASS`), `summary.json` |

## Required failures (recap, in execution order)

1. **Status response >4096 bytes.** Every fresh `GET /api/v1/status` body
   measures 5,143–5,145 bytes across this run (recorded 5,133 in the
   deferred local-gate matrix). The pinned `<4096` protocol contract is
   violated; the report does not pretend otherwise. This was first
   surfaced as a `status_body_length` FAIL in
   `core-hil/control/run-20260731T051957Z-602457/summary.tsv` and is
   reproduced in the realtime-network `capport-network-validation.json`
   runs.
2. **WebSocket `snapshot/status` cadence.** Each subscription receives only
   two initial `snapshot/status` frames before telemetry-only frames.
   The strict 3-in-5 s snapshot cadence contract is not met. The four-client
   timeline confirms the same shape (per-client initial two `snapshot`
   frames then a steady `telemetry` stream). This was first surfaced as a
   `memory_ws_phase2` FAIL in
   `core-hil/control/run-20260731T051957Z-602457/summary.tsv` and is
   reproduced in browser-hil `p3 step ws-frame-summary`.
3. **Wildcard DNS A returns `198.18.0.41`, not `172.29.203.1`.** The
   board-local DNS responder does not match the captive-portal contract on
   this run; AAAA NOERROR/NODATA, port-80 CAPPORT, 302 redirect, and 405
   rejection still pass.
4. **Web Serial setup dialog positioning defect (new in-scope product
   bug).** `web/src/components/SerialCard.tsx` renders the setup modal
   inline under a transformed `.animate-fade-up` grid. `position: fixed`
   resolves against the grid, so at 375 x 812 the dialog renders at
   y=1108 (viewport 812) and is unreachable with body scroll locked, and
   at 1280 x 900 the dialog bottom is clipped (y=575..1064). `DESIGN.md`
   Section 4 requires body-portaled overlays. The persistent-config dialog
   portals correctly; the same fix applies here.
5. **WS SINGLE 1 MHz stability fails the >=950 ksps contract.** All 10
   runner invocations passed in the lossless/explicit-stop sense, but the
   effective receive rate ranged 347,793.888–364,310.690 samples/s with
   median 362,253.247; each run ended by capacity overrun, not the
   required five-second lossless/STOP_RESP outcome. The product/host
   delta is not proven.
6. **LA mutual-exclusion reverse direction fails.** TCP owner -> WS
   `HELLO` returns BUSY error code 8 as required. WS owner after
   `CONFIG/START` -> TCP `HELLO` succeeds, and subsequent TCP `CONFIG`
   returns no parseable BUSY frame, so the required WS-to-TCP BUSY
   contract is not satisfied.
7. **`web-ota-hil.sh --flow negative-upload` returns `upload_aborted`,
   not `sha256_mismatch`.** The shell hard-codes a 5 s command timeout
   that aborts the full 734,656-byte bad-SHA request. Firmware behavior
   on a direct full-body bad-SHA request is independently verified
   (`direct-sha-mismatch.body`, `direct-sha-mismatch.status.json`).
8. **OTA browser repo manual runner loses the Confirm-enable race twice.**
   The page-side OTA poll uses `fetch` with no timeout, so a request
   hangs across the test-boot reboot; the runner hard-codes a 10 s
   enable budget and the page needs ~9.2 s to re-enable the Confirm
   button, so two consecutive runs exceeded it. The instrumented
   manual flow (independent driver) clicks Confirm at +9,184 ms and
   finishes idle+confirmed before the 16 s auto-gate. Firmware behavior
   is correct; the test runner is the failure.
9. **Current-source CLI `prepare_ota_upload` (`cmd-ng/src/app.rs:610-630`)
   has no `.bin` extension guard.** `prepare_ota_upload` checks only that
   the path is a regular file (`metadata.is_file()`) and that `size != 0`,
   then computes SHA and prepares the `/api/v1/ota/upload` request. There
   is no extension validation. The current-source empty `.uf2` test in
   `ota-api-hil/current-cli-invalid-extension/` proves only that the CLI
   returns `{"code":"invalid_file","message":"OTA image ... is empty"}`
   because the file is empty (`cli.exit=2`, `cli.strace` exits with 2,
   `network-check.log` `network_syscalls=absent upload_path_observed=false`,
   `state-check.log` `temp_removed=true ota_state_unchanged=true`,
   `ota-after.json` shows OTA still `idle`). This rejects on empty size,
   not on extension; the extension-validation contract is therefore not
   proven by the empty-file path. The old installed release CLI's
   `unsupported command "ota" over HTTP` rejection is superseded
   historical context for this finding and is not the current failure
   basis.

## BLOCKED items (not failures or passes)

These are recorded but not counted as pass/fail. They have documented
reasons and the run does not claim them as resolved.

- DHCP DORA / option 114 (sudo not passwordless, no `CAP_NET_RAW`).
- Physical GPIO25 LED observation.
- Physical VIN 3.3 V / 1.8 V measurement; 1.8 V is conditional and
  intentionally not executed.
- Full external 4-bit WIDE11 generator mapping (no external generator;
  reduced GP10 mapping passed).
- Native sigrok-cli driver not present in the packaged libsigrok.
- Web Serial chooser / device selection / UART RX-TX needs human/echo
  source.
- Watchdog rollback has no safe liveness fault-injection path before
  the 16 s gate.
- Legacy GP29 v1 output snapshot has no safe injection path; GP29 input
  snapshot passed.
- Mobile LA and 768 px Serial-dialog visual captures were incomplete per
  Oracle; product PASS is not claimed for those specific visual states.
- Rolling nightly GitHub Actions run is outside this local HIL and is
  not claimed.

## Attempt history (preserved, not hidden)

- Browser Apply-clear Phase B was initially blocked when the route glob
  `**/api/v1/config*` did not match `/api/v1/config/apply` (Playwright
  `*` does not cross `/`), so the harness did not hold the confirm POST.
  Harness fixed to regex `/\/api\/v1\/config(\/|$)/`; the full Save ->
  reboot -> Apply sequence was re-executed and produced the 16/16 PASS
  matrix above. Evidence under `browser-apply-clear-hil/` (excluding
  `attempt1/`).
- The core HIL udisks mount race failed on the first attempt
  (`flash-mount.log`: `Error looking up object for device /dev/sdc1`).
  The bounded recovery (`flash-recovery-copy.log`,
  `flash-recovery-readiness.log`, `flash-recovery-cdc.log`,
  `flash-recovery-status.json`) copied the same verified combined UF2
  at `/run/media/chen/RP2350`, recovered HTTP at attempt 1, and re-
  enumerated CDC at `/dev/ttyACM2`. The post-reboot
  `flash-final-cleanup-verify.log` line
  `board_http_unavailable_during_cleanup` reflects the post-reboot
  device unbind and is not the final flash verdict. The same
  dfe3a1f6… UF2 is the artifact used by every subsequent phase,
  including persistence BOOTSEL recovery, OTA, browser, and GP29
  runs.
- The "old installed CLI" OTA path that earlier runs flagged is
  superseded by the current-source CLI behavior in phase 22; the
  missing `.bin` extension guard remains a real finding.
- The earlier 2026-07-30 persistence run's "broad browser HIL Apply-
  success clears selection" item (browser-hil #11) is BLOCKED in the
  current run; the dedicated real-pending Apply-clear flow (Phase B
  above) superseded that gap with a precise timeline.

## Final state (verified)

Independent final audit (`final-safety-audit/summary.json`,
`final-safety-audit/VERDICT.md`):

- HTTP: snapshot absent, `pending=0`, config_absent=true.
- All four controllable power rails `off`: `12v_out`, `5v_out`,
  `vdd_5v`, `20v_out`.
- Switches: SD `target`, USB `target`, TF-WP `writable`, VIN `3.3v`.
- Every firmware-enumerated GPIO: `input`.
- OTA: `idle`, `current_image_confirmed=true`.
- Watchdog: `supported`, `automatic`, `healthy`, `armed`.
- CDC: present, no holders; `/dev/ttyUSB0` (rustty) preserved.
- Host: no owned Playwright/Chromium/serial-bridge/device-bridge
  process, no VENDOR=RPI disk or mount, no port 5556 connection, no
  tmux server.
- Combined UF2 + OTA bin + canvas assets match the fresh build;
  no flash operations during the audit.

## Key measurements carried into the report

Preserved here so the report is useful if `/tmp` is gone.

- Status response body: 5,143–5,145 bytes (deferred local-gate baseline
  5,133), violating `<4096`.
- `GET /api/v1/status` core HIL HTTP latency: 27–62 ms, watchdog
  latency: 11–45 ms.
- ADC telemetry low-rate 3 records: 100,416,638 ns and 100,436,762 ns
  host deltas; sample sequence 3880, 3881, 3882; device_t_mono_us
  4,769,419,000; 4,769,519,500; 4,769,619,900.
- ADC telemetry high-rate 1000 records: wire sequence first/last
  4,087 / 5,086; sample_sequence first/last 4,087 / 5,086;
  device_t_mono_us first/last 4,801,344,000 / 4,802,353,700; host
  interval min 3,351 ns / max 21,057,734 ns; device interval min 900 µs
  / max 2,100 µs; `metadata.dropped_samples=204` reported on row 0
  (the expected ring-overrun sentinel, not a per-sample loss).
- Four concurrent WS clients: all opened, all closed cleanly, all
  `delete` responses after close were the expected
  `unknown_session_id` 404; per-client HTTP 200 status snapshot in
  every iteration; max observed health latency 0.558 s.
- Logic analyzer authoritative 54/54: 1,496,750 received samples, 793
  DATA frames, 0 gaps, 0 disconnects, 4 capacity-stop-before-data.
- Logic analyzer high-rate 62/62: exact 100,000 samples in 98 DATA
  frames on both TCP and WS for the 18 accepted CONFIG_V2 post=100000
  cases; WIDE11 125 MHz rejected twice as the documented invalid
  configuration.
- WS SINGLE 1 MHz stability 10 runs: 41,344 / 41,568 / 42,848 /
  42,912 / 42,976 / 41,536 / 41,536 / 41,472 / 41,408 / 41,280 samples
  per 5 s window; min 347,793.888, median 362,253.247, max
  364,310.690 samples/s.
- Six isolated trigger captures (1 / 2 MHz x rising/falling/either):
  exact 512 samples, valid trigger offset, zero gap/disconnect, STOP
  response, restart, HTTP health.
- Persistence runner: 85 assertion rows, exit 0, 115 s. 6 flows
  PASS, 9-step final cleanup PASS.
- Browser Apply-clear: Save held ~1.2 s with 3 DOM samples; release at
  09:32:25.605Z; clear at 09:32:25.715Z (110 ms after release, after
  authoritative GET).
- GP29 v1 input snapshot: snapshot present v1, post-reboot
  `apply_state=applied` for the safe value, cleared with no
  hardware change.
- OTA browser auto: 17915..707579 progressive upload, 14 s pending_test,
  auto-confirm by firmware.
- OTA browser instrumented manual: Confirm enabled +9,184 ms after
  pending_test, clicked before the 16 s auto-gate, final idle+confirmed.
- App flash 734,616 / 847,832 (86.65%), app RAM 494,272 / 532,480
  (92.82%).

## Release-readiness statement

The 2026-07-31 run is **not CI/HIL-ready**. The nine required failures
above must be remediated and the affected lanes must be rerun on real
hardware. The browser Apply-clear real-pending path may stay on its
dedicated evidence base once the SerialCard setup dialog positioning
defect is fixed and the full-pending flow is re-validated. The CI
artifact-warning note (only combined UF2 for ROM BOOTSEL; app-only
`zephyr.uf2` forbidden) remains in force for any rerun.
