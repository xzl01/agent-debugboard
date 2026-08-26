# 2026-08-25 cmd-ng TUI Final Event-boundary HIL

## Verdict

**PASS.** The current `cmd-ng` release build passed the final focused
real-board and real-terminal mouse-open-modal and refresh-stale-hit boundary
matrix. Browser-generated SGR mouse and keyboard bytes were delivered as one
PTY burst in every case. The first redraw visibly preceded each queued
confirmation or stale hit, every TUI mutation was intercepted locally, and
zero mutations were forwarded to the board.

The complete source, release, combined UF2, hardware, persistent configuration,
and task baselines remained equal. The required PID `3731563` was alive at the
single final process check.

## Scope

This run validates the final host-side delta for:

- Power left-button Down followed by queued Enter
- dynamic Switch left-button Down followed by queued Enter
- `r` refresh followed by a stale Power coordinate Down
- `r` refresh followed by a stale Switch coordinate Down
- deny-by-default request interception and request/redraw timestamp ordering
- full-height xterm.js PNG, text, and ANSI evidence
- HTTP, bounded WebSocket, CDC `vin get`, watchdog, and exact final equality

No product source, firmware, Web UI, dependency, historical report, persistent
snapshot, task blob, or hardware state was changed by this HIL. The only
product-document addition is this dated report.

## Target And Artifacts

| Item | Value |
| --- | --- |
| Board | G3 RP2350A, HTTP `http://172.29.203.1` |
| Firmware CDC | `/dev/serial/by-id/usb-Radxa_Radxa_Linkr_Debugger_E6641C31A362C336-if00` -> `/dev/ttyACM2` |
| Git HEAD | `a03ef6206d38fe797d3e874c37d7431f263a9ca5` |
| cmd-ng source manifest SHA-256 | `91754a7f9c2a9a099f154ff01b28ef08d5cd09bd43de86071dd29f8ac20ae5d6` |
| cmd-ng tracked diff SHA-256 | `4f1b1cfe4be687df3150e83debb55b086b387520d644eba13c2c25b212947fe5` |
| Source files | 152 Cargo/Rust inputs |
| Release binary | `cmd-ng/target/release/radxa-linkr-debuggerctl` |
| Release version | `0.2.1` |
| Release SHA-256 | `7eb4350ff59fd00cc148d13f0c15f49d99447068a64eaa64a7f19a280a383f24` |
| Release size | 12,969,888 bytes |
| Combined UF2 SHA-256 | `f08c7580cc3aa634996fc702e5219a2e49826a572ee97193c1a5ae284320fb06` |
| Combined UF2 size | 1,594,368 bytes |
| Evidence | `.omo/evidence/20260825-tui-final-event-boundary-release/` |

`cargo build --release --locked --manifest-path cmd-ng/Cargo.toml` completed
successfully and produced the release hash above.

## Evidence And Supersession Boundary

The earlier
[full TUI mouse and confirmation HIL](2026-08-25-cmd-ng-tui-mouse-confirmation-hil.md)
passed its product matrix but remains a historical overall **FAIL** because its
required PID `957210` was absent at the final check. This run does not rewrite
that result.

The later
[mouse post-review HIL](2026-08-25-cmd-ng-tui-mouse-post-review-hil.md)
remains the broader mouse, switch, visual, and transport **PASS**. The
[keyboard modal HIL](2026-08-25-cmd-ng-tui-keyboard-modal-hil.md) remains the
keyboard modal/error-boundary **PASS** for release `5ef7c9...`.

The current `7eb4350f...` release adds the final mouse input-safety host delta
after that keyboard run. This report supersedes only the event-boundary
acceptance claim for the current host release. It does not replace either
broader report or their historical artifacts.

BOOTSEL, flashing, and OTA were deliberately not run. The combined UF2 is
byte-identical to the prior reports, no firmware behavior changed, and
preserving PID `3731563` and `pts/1` was mandatory. Neither the combined UF2
nor the application-only `zephyr.uf2` was used.

## Preserved Baseline

Before the focused matrix, the real board reported:

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

| Projection | Pre SHA-256 | Final SHA-256 |
| --- | --- | --- |
| Hardware/config/task projection | `4b1aa04843207d1399575e9043fd71fa5d84dabcd46fab18ad26f4c906f48bcd` | same |
| Normalized config GET | `84a7e9ac83705cde82b15991dfd44382f30006a61581e43b10fbd0ac9e29e7d7` | same |
| Normalized stored tasks GET | `320524b11c7ecac0929fb8d60bd186ee0c8999f1769ce0789bcfd7e3bd7c1e2b` | same |
| Normalized task catalog GET | `7f56c75dfe3f585a2437058974cc94fd939c3197b4a48304dc709fc127a922c8` | same |
| Decoded task blob | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` | same |

The post-CDC final capture also reproduced the source manifest, tracked diff,
release, and combined-UF2 hashes exactly.

## Real xterm.js Event Matrix

The release binary ran through task-owned node-pty 1.1.0 into xterm.js 6.0.0
under Playwright Core 1.55.0 and Chromium 151.0.7922.173. Chromium pointer and
keyboard events were converted by xterm.js into real terminal bytes. Each
boundary sequence was delivered as one PTY write, so the later input was
already ready when the first event was processed.

No tmux, direct model call, pasted screen, ANSI-to-HTML substitute, or mock
board baseline was used.

| Case | Single PTY payload | Ordering proof | Result |
| --- | --- | --- | --- |
| Power Down + Enter | `1b5b3c303b353b31324d0d` | modal stream chunk `768.556 ms`; intercepted PUT began `771.719 ms` | PASS, modal first by 3.163 ms |
| Switch Down + Enter | `1b5b3c303b353b31364d0d` | modal stream chunk `1678.715 ms`; intercepted PUT began `1684.576 ms` | PASS, modal first by 5.861 ms |
| Refresh + stale Power Down | `721b5b3c303b353b31324d` | transformed real GET completed `2695.706 ms`; empty-controls redraw `2751.606 ms` | PASS, old row 12 coordinate blank |
| Refresh + stale Switch Down | `721b5b3c303b353b31364d` | transformed real GET completed `3816.228 ms`; empty-controls redraw `3870.425 ms` | PASS, old row 16 coordinate blank |

The Power and Switch first-redraw frames visibly contain the complete red
modal border, title, warning, target, `[ Confirm ]` and `[ Cancel ]` buttons,
and modal-specific keybar before Enter is admitted. Enter then generated one
locally intercepted request in each case.

For each refresh case, the proxy first forwarded an unmodified real status to
populate the TUI. On `r`, it fetched another fresh real status and returned a
transformed copy with all four Power rows, four Switch rows, and fifteen GPIOs
removed. Clearing all controls made the old coordinate provably target-free
instead of allowing a different row to shift into that coordinate. The real
board response and transformed response are both retained with independent
hashes.

The queued stale Down remained behind the redraw boundary. After the redraw,
the old coordinate was blank; the settled frame had no modal, no worker or
pending marker, and no mutation request.

## Focused Unit Coverage

The current source's six matching Rust regressions passed in 0.03 seconds;
521 other tests were filtered, matching the full 527-test suite size:

- `power_mouse_down_defers_queued_enter_until_modal_redraw`
- `dynamic_switch_mouse_down_defers_queued_enter_until_modal_redraw`
- `stale_power_hit_target_is_inert`
- `stale_switch_hit_target_is_inert`
- `stale_gpio_hit_target_is_inert`
- `refresh_removing_power_defers_stale_coordinate_until_fresh_render`

These deterministic tests retain direct coverage for stale GPIO target
inertness alongside the focused real-board Power and Switch refresh cases.

## Request Safety Audit

The final deny-by-default loopback proxy recorded 20 exact TUI requests:

| Class | Count | Board effect |
| --- | ---: | --- |
| Forwarded GET | 18 | Read-only status, ADC, and config authority |
| Intercepted Power PUT | 1 | Local synthetic success; not forwarded |
| Intercepted Switch PUT | 1 | Local synthetic success; not forwarded |
| Forwarded mutation | 0 | None |
| Unexpected/denied request | 0 | None |

The two transformed status responses still came from freshly forwarded real
GETs. No Power, Switch, GPIO, Config Save/Clear, task, OTA, BOOTSEL, or other
TUI mutation reached the board.

The bounded WebSocket reads used the protocol's ephemeral live-session
lifecycle and deleted only their own sessions. They did not change hardware,
persistent configuration, or tasks.

## Visual Evidence

- Four final sessions produced 12 fresh original PNG/text/ANSI frame sets.
- Every PNG captures the full 80x24 `.xterm-screen` at 1120x840 pixels.
- Every text capture contains exactly 24 rows and a nonblank final keybar.
- All 12 `tui-check` results report `maxWidth=80`, no overflow,
  `borderMisaligned=false`, and no wide-character drift.
- PNG signatures, dimensions, ANSI prefixes, and full-height edge coverage
  passed.
- Two independent read-only reviews inspected the complete final set, traced
  the source and request timeline, opened all 12 PNGs, and returned PASS with
  HIGH confidence and no blocking finding.

Confirmation modals intentionally occlude controls beneath them. The modal
border and text remain complete; this is not an overlap defect. The refresh
frames intentionally retain status, scope, tabs, table header, and keybar while
the controls data area is empty.

## HTTP, WebSocket, CDC, And Final Equality

- Pretest curl status: HTTP 200, 5,201 bytes, 35.605 ms,
  `radxa-linkr-debugger.v1`, `ok=true`, `command=status`.
- Pretest bounded WebSocket: one record, four readings, sequence 7,
  `device_t_mono_us=59094626600`.
- Pretest CDC at 115200 baud: read-only `vin get` returned `vin=3.3v`.
- Final curl status: HTTP 200, 5,201 bytes, 31.054 ms, valid success envelope.
- Final bounded WebSocket: one record, four readings, sequence 8,
  `device_t_mono_us=59836240200`.
- Final CDC at 115200 baud: read-only `vin get` returned `vin=3.3v`.
- A fresh full baseline capture after CDC remained byte/field-equal to pretest.
- Watchdog remained healthy and armed.

No restoration action was needed because no board mutation was forwarded.

## Harness Attempt Record

One pre-final harness attempt is retained under `attempt-1-tui/`. Its Power and
Switch cases passed, but the first refresh case's replay predicate selected an
earlier startup empty frame instead of the post-burst frame. The attempt stopped
as an evidence-pipeline failure, not a product failure.

That attempt recorded 14 requests: 12 forwarded GETs, two locally intercepted
PUTs, and zero forwarded mutations. Its three PTYs exited with code zero, its
browser and proxy closed, and it did not change the board. The final `tui/`
matrix was a fresh complete four-case rerun with a lower ANSI chunk boundary;
no attempt artifact is counted in the final 4-case/12-frame totals.

## Final Cleanup And Protected Process

- All four final PTYs and all three attempt PTYs exited with code zero; every
  one was `/dev/pts/20`, never `pts/1`.
- Both task-owned Chromium instances closed.
- Final proxy port 33761 and attempt proxy port 36515 both returned
  `ECONNREFUSED` after close.
- The exact task-owned npm runtime
  `/tmp/opencode/20260825-tui-final-event-boundary-release-harness` was removed.
- An unintended root `package.json` created during temporary npm setup was
  immediately removed before HIL execution; no root `package.json` or lockfile
  remains.
- No broad cleanup command, tmux command, `/proc` inspection, signal, input,
  resize, attach, or reuse operation targeted PID `3731563` or `pts/1`.
- No BOOTSEL, flash, OTA, or emergency board restoration action occurred.
- The single final `ps -p 3731563 -o pid=` check returned `3731563` with exit
  status zero.

## Evidence Index

- Source, artifacts, and baseline: `pretest/summary.json`,
  `pretest/source-files.SHA256SUMS`, raw/normalized endpoint captures, source
  diff, and `pretest/hardware-config-task-baseline.normalized.json`
- Final matrix: `tui/harness-summary.json`, `tui/validation-summary.json`,
  `tui/all-requests.ndjson`, and the four final case directories
- Fresh status transformation: each refresh case's `fresh-real-status.json`,
  `transformed-status.json`, and `transformation.json`
- Safe harness history: `attempt-1-tui/`
- Transport and equality: `pretest/health-summary.json`,
  `final/health-summary.json`, both CDC captures, and `final/comparison.json`
- Cleanup and protected PID: `final/cleanup-summary.json` and
  `final/user-pid-check.json`
- Independent reviews: `reviews/functional.txt` and `reviews/visual.txt`
- Integrity: `SHA256SUMS` and `checksum-validation.txt`
