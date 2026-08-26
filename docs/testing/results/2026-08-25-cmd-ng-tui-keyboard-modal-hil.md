# 2026-08-25 cmd-ng TUI Keyboard Modal HIL

## Verdict

**PASS.** The current `cmd-ng` release build passed the focused real-board
keyboard modal and Saved Config error-boundary matrix. All TUI-originated
Power, Switch, GPIO, and Config writes were intercepted locally; no hardware,
persistent configuration, or task mutation reached the board. The normalized
source, release, UF2, hardware, config, and task baselines remained exactly
equal, and the required PID `3731563` was alive at the final process check.

## Scope

This run validates the new typed `KeyOutcome::{Continue, Redraw, Exit}` path at
the real terminal and board boundary:

- keyboard Power and Switch activation redraws a modal before draining stale
  mouse input
- keyboard confirmation redraws before two queued stale modal/control Downs
- three queued Enter presses advance one visible stage per redraw
- Saved Config confirmation redraws before queued stale row/modal Downs
- a proxy-crafted `storage_error` blocks every non-dismiss key under test
- Esc dismisses with a redraw and global q exits cleanly
- the live firmware-advertised `tf_wp` switch is visible in Controls and Saved
  Config, matching the current English and Chinese TUI guides' TF write-protect
  entry

No product source, firmware, API, Web UI, non-TUI CLI behavior, dependency,
existing report, persistent snapshot, or task blob was changed by this HIL.
The only product-document addition is this dated report.

## Target And Artifacts

| Item | Value |
| --- | --- |
| Board | G3 RP2350A, HTTP `http://172.29.203.1` |
| Firmware CDC | `/dev/serial/by-id/usb-Radxa_Radxa_Linkr_Debugger_E6641C31A362C336-if00` -> `/dev/ttyACM2` |
| Git HEAD | `a03ef6206d38fe797d3e874c37d7431f263a9ca5` |
| cmd-ng source manifest SHA-256 | `b26ae3c1645178ab19bf53903d0a51b38f55095e232099901a5468e971713bce` |
| cmd-ng tracked diff SHA-256 | `7695c74b017e63f65d5ed5c5a1c75d85f000ffd4d2d61f29222273a09c417b09` |
| Release binary | `cmd-ng/target/release/radxa-linkr-debuggerctl` |
| Release version | `0.2.1` |
| Release SHA-256 | `5ef7c9fbb0d87170c390277a701aead106e0182e30bae6423f23e5f5cd2b0705` |
| Release size | 12,969,816 bytes |
| Combined UF2 SHA-256 | `f08c7580cc3aa634996fc702e5219a2e49826a572ee97193c1a5ae284320fb06` |
| Combined UF2 size | 1,594,368 bytes |
| Evidence | `.omo/evidence/20260825-tui-keyboard-modal-final-release/` |

`cargo build --release --locked --manifest-path cmd-ng/Cargo.toml` completed
successfully against the recorded 151-file source manifest and produced the
release hash above.

## Recovery Evidence Boundary

The earlier
[full TUI mouse and confirmation HIL](2026-08-25-cmd-ng-tui-mouse-confirmation-hil.md)
passed its product matrix but remains an overall historical FAIL because its
required user PID was absent. The later
[post-review real-board HIL](2026-08-25-cmd-ng-tui-mouse-post-review-hil.md)
is the current PASS for the broader mouse, switch, visual, and transport delta.
This report adds only the final keyboard modal/error boundary proof and does not
rewrite either result.

BOOTSEL, flashing, and OTA were deliberately not rerun. The combined UF2 is
byte-identical to the prior runs, no firmware behavior changed, and preserving
PID `3731563` and `pts/1` without USB disconnect or terminal interference was a
hard requirement. Neither the combined UF2 nor the application-only
`zephyr.uf2` was used.

## Preserved Baseline

Before the first focused TUI session, the real board reported:

- Power: `12v_out=off`, `5v_out=on`, `vdd_5v=on`, `20v_out=off`.
- Switches: `sd=usb-reader`, `usb=pc`, `tf_wp=protected`, `vin=3.3v`.
- GPIO: `GP10=output/0`; every other advertised GPIO was `input/0`.
- Watchdog: supported, automatic, healthy, armed, 5,000 ms timeout, no failing
  service.
- Persistent configuration: v1 snapshot present, six selected entries,
  `pending=0`; live `tf_wp=protected` intentionally differed from saved
  `writable`.
- Tasks: zero stored tasks and a zero-byte task blob; the task catalog was
  captured separately.

| Projection | SHA-256 |
| --- | --- |
| Hardware/config/task projection | `4b1aa04843207d1399575e9043fd71fa5d84dabcd46fab18ad26f4c906f48bcd` |
| Normalized config GET | `84a7e9ac83705cde82b15991dfd44382f30006a61581e43b10fbd0ac9e29e7d7` |
| Normalized stored tasks GET | `320524b11c7ecac0929fb8d60bd186ee0c8999f1769ce0789bcfd7e3bd7c1e2b` |
| Normalized task catalog GET | `7f56c75dfe3f585a2437058974cc94fd939c3197b4a48304dc709fc127a922c8` |
| Decoded task blob | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |

## Real xterm.js Keyboard And SGR Matrix

The release binary ran through task-owned node-pty 1.1.0 into xterm.js 6.0.0
under Playwright Core 1.55.0 and Chromium 151.0.7922.173. Browser keyboard and
pointer events were converted by xterm.js into real terminal bytes. Each queued
boundary burst was then delivered as one PTY write, so all later inputs were
already ready when the first event was processed. No tmux, direct model call,
ANSI-to-HTML substitute, pasted screen, or mock board data was used.

| Group | Result |
| --- | --- |
| 1a. Power Enter + stale Down | PASS: payload `0d 1b5b3c303b353b31334d`; Power modal was visible at the first redraw, stale Down used modal geometry, zero mutation |
| 1b. Switch Enter + stale Down | PASS: the same real Enter/SGR shape opened `sd -> target`, first redraw preceded stale Down drain, zero mutation |
| 2. Existing modal + Enter + two stale Downs | PASS: payload `0d 1b5b3c303b33353b31334d 1b5b3c303b353b31334d`; first Enter produced one intercepted Power PUT, both Downs waited for redraw, no hidden second PUT |
| 3. Three queued Enters | PASS: payload `0d0d0d`; visible stages were modal open, action result with modal closed, and modal open again; exactly one intercepted PUT |
| 4. Saved Config Enter + two stale Downs | PASS: payload `0d 1b5b3c303b393b31324d 1b5b3c303b33353b31344d`; busy redraw preceded both Downs, one Config request was intercepted, no hidden second request |
| 5a. Power/error keys | PASS: Tab, c, r, s, x, and selected Power Enter preserved the same page/error pixels and emitted no request after the crafted error |
| 5b. GPIO/error keys | PASS: selected GPIO l, o, and i preserved the same page/error pixels and emitted no request after the crafted error |
| 6. Live `tf_wp` | PASS: live switch list was `sd`, `usb`, `tf_wp`, `vin`; `tf_wp=protected` appeared in 120x32 Controls and `switch/tf_wp` appeared in Saved Config |

All eight task-owned PTYs were `/dev/pts/20`, never `pts/1`, and exited with
code 0. The normal case cleanup used q while modal and non-modal states were
active. Both error cases directly showed Esc dismissal followed by q exit; the
current source and local regression suite retain the global Ctrl-C exit branch.

## Saved Config Error Isolation

The proxy returned a real HTTP `storage_error` envelope after intercepting each
confirmed Save. Polling was paused before the error so a blocked key had no
legitimate telemetry difference to hide behind.

- Power error screen plus Tab/c/r/s/x/Enter/final-error: eight PNGs, all exact
  SHA-256 `e4b0ee6aad10da20798c31c502d84ab9c06f91bc398c131fa05209fcca5153d5`.
- GPIO error screen plus l/o/i/final-error: five PNGs, all exact SHA-256
  `ff44362cf12da609c3ef6880dd7620fffce28cae6b5124cf916f53ae45b6c75c`.
- The page remained Controls, the error/keybar remained visible, no worker or
  request started, and no hardware modal or GPIO action appeared.
- Esc changed the screen only by dismissing the error and restoring the normal
  keybar, which is direct visual evidence of the redraw outcome.

## Request Safety Audit

The deny-by-default loopback proxy recorded 33 exact TUI requests:

| Class | Count | Board effect |
| --- | ---: | --- |
| Forwarded GET | 28 | Read-only status, ADC, config, and switch authority |
| Intercepted Power PUT | 2 | Local synthetic success; not forwarded |
| Intercepted Config PUT | 3 | Local confirmation/storage-error response; not forwarded |
| Forwarded mutation | 0 | None |
| Unexpected/denied request | 0 | None |

No Power, Switch, GPIO, Config Save/Clear, task, OTA, BOOTSEL, or other TUI
mutation reached the board. The bounded WebSocket read used the protocol's
ephemeral live-session lifecycle and cleaned it; it did not change hardware,
persistent configuration, or tasks.

## Visual Evidence

- Eight sessions produced 36 fresh original PNG/text/ANSI frame sets.
- Every 80x24 PNG is the full 1120x840 `.xterm-screen`; every 120x32 PNG is
  1680x1120.
- Every frame contains every terminal row and a nonblank final keybar.
- All 36 `tui-check` results report no overflow and
  `borderMisaligned=false`; no wide-character drift was found.
- PNG signature/dimensions, ANSI prefixes, opaque composition, and edge
  coverage all passed.
- Two independent read-only reviews checked all originals and returned PASS;
  the visual review confidence was 0.98.

Confirmation modals intentionally occlude the controls beneath them. The modal
border, title, body, buttons, and replacement keybar were intact in every
original frame; the occlusion is not an overlap defect.

## HTTP, WebSocket, CDC, And Final Equality

- `curl -fsS GET /api/v1/status`: HTTP 200, 5,201 bytes, 88.85 ms,
  `radxa-linkr-debugger.v1`, `ok=true`, `command=status`.
- Current release `--json status`: valid success envelope.
- Bounded release `adc record`: one telemetry record, four readings,
  `sequence=6`, and equal `device_t_mono_us`/`uptime_us=55012073800`.
- CDC by-id at 115200 baud: read-only `vin get` returned `vin=3.3v`.
- Final hardware/config/task projection SHA-256 remained
  `4b1aa048...8bcd`; source manifest, release, UF2, and zero-byte task blob were
  also exactly equal to pretest.
- Watchdog remained healthy and armed.

No restoration action was needed because no board mutation was forwarded.

## Harness Attempt Record

Three pre-final harness attempts are retained separately and did not produce a
product failure or board mutation:

1. The first looked up an old Power row after the modal had already obscured
   it: 9 GETs, 0 mutation.
2. The second repeated that evidence-coordinate mistake for a Saved Config row:
   15 GETs, 2 locally intercepted Power PUTs, 0 forwarded mutation.
3. The third encoded Esc immediately followed by Tab as `1b09`, which a real
   terminal parses as a combined sequence: all blocked-key pixel checks had
   already passed; 20 GETs, 4 locally intercepted requests, 0 forwarded
   mutation.

Each attempt closed its own Chromium, proxy, and PTYs. The final matrix was a
fresh complete rerun under a new proxy endpoint, not a partial reuse.

## Final Cleanup And Protected Process

- All 8 final PTYs exited with code 0; none used `pts/1`.
- Task-owned Chromium and loopback proxy closed; proxy port 32839 had no
  listener.
- The exact task npm/runtime directory and temporary compatibility symlink were
  removed.
- No broad cleanup command, tmux command, `/proc` inspection, BOOTSEL, flash,
  OTA, signal, input, resize, attach, or reuse operation targeted PID `3731563`
  or `pts/1`.
- The final `ps -p 3731563 -o pid=` returned `3731563` with exit status 0.

## Validation And Evidence Index

The repository doc-layout, repository-gate, and test-registration checkers
passed. Their focused Node contract tests passed 170/170. Evidence validation
passed all 8 sessions, 36 frames, 33 requests, 5 queued browser bursts, exact
blocked-screen equality, and final baseline equality.

- Source/artifacts and baseline: `pretest/summary.json`,
  `pretest/source-files.SHA256SUMS`, raw/normalized endpoints, source diff, and
  `pretest/hardware-baseline.normalized.json`
- Final matrix: `tui/harness-summary.json`, `tui/validation-summary.json`,
  `tui/all-requests.ndjson`, and the eight final case directories
- Safe harness history: `attempt-1-tui/`, `attempt-2-tui/`, `attempt-3-tui/`
- Transport and equality: `final/health-summary.json`,
  `final/cdc-vin-get.json`, `final/comparison.json`
- Cleanup and PID: `final/cleanup-summary.json`, `final/user-pid-check.json`
- Independent review: `reviews/functional.txt`, `reviews/visual.txt`
- Integrity: `SHA256SUMS`, `checksum-validation.txt`
