# 2026-08-24 cmd-ng TUI Event Boundaries HIL

This is the authoritative dated report for the current `cmd-ng` TUI
event-boundary fixes that landed after the post-review current-release delta
in the older
[GPIO Gestures HIL](2026-08-24-cmd-ng-tui-gpio-gestures-hil.md). The older
gesture report's earlier `4276…` 17-case matrix remains historical gesture
evidence and is not replaced by this report.

## Scope

This run validates the three event boundaries the post-review delta and the
follow-up fix introduced:

- confirmation deadline evaluated on the confirmation event itself
  (`now < started + CONFIRM_TIMEOUT`), independent of any later render or
  poll tick
- GPIO direct-key decoder modifier filter that accepts only lowercase keys
  without modifiers or matching uppercase keys with Shift, and preserves any
  in-flight gesture for every rejected combination
- terminal `Event::Resize` as a canonical redraw boundary that cancels the
  current GPIO gesture, clears `hit_map`, and forces a redraw before the
  next queued input is consumed

The validation runs against a real G3 RP2350A Linkr Debugger through an 80x24
or resized real PTY, with a loopback logging proxy forwarding requests to the
board so every PUT is recorded. All three boundary checks use HTTP
authoritative readback. The deterministic Super key and exact-deadline cases
remain deterministic-test-only and are not claimed on real-board HIL.

## Artifacts

- Release binary: `cmd-ng/target/release/radxa-linkr-debuggerctl`
- Release binary SHA-256:
  `8d93c2708e39352937b31ab145360fd3dc2f2ad2e50505065ae0d2c782fef833`
- Release binary size: 12,956,352 bytes
- Combined UF2:
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`
- Combined UF2 SHA-256:
  `37d48dd7ecac90834bbf3ef28bf66e7ce6a852f3e62543e1acb2d8e43ee23e71`
- Combined UF2 size: 1,710,080 bytes
- Firmware CDC by-id:
  `/dev/serial/by-id/usb-Radxa_Radxa_Linkr_Debugger_E6641C31A362C336-if00`
- Loopback logging proxy: `http://127.0.0.1:22719`, PID terminated after run
- Evidence directory:
  `.omo/evidence/20260824-tui-event-boundaries-release/`

The canonical firmware build passed with FLASH 828,948/847,832 bytes (97.77%)
and RAM 508,744/532,480 bytes (95.54%). Only the combined UF2 was used for
ROM BOOTSEL. The application-only `zephyr.uf2` was never used.

## Local Gates

- Rust 461/461 PASS (3 confirmation, 4 modifier, 2 Resize regressions added,
  full suite green)
- Web Node tests 392 pass / 1 skipped, Vitest 427/427, production build: PASS
- `nix flake check -L`: PASS
- Repository governance and contract tests: 181/181 PASS
- Offline firmware model tests: PASS
- Every changed Rust module stays under the 250 pure-LOC ceiling: PASS
- Test registration, document layout, nightly workflow, and skill boundary:
  PASS

## Baseline

Before HIL, the board reported:

- GP13: `input/0`
- GP10: `output/0`; GP8 and every other advertised GPIO: `input/0`
- Power: `12v_out=off`, `5v_out=on`, `vdd_5v=on`, `20v_out=off`
- Switches: `sd=usb-reader`, `usb=pc`, `tf_wp=writable`, `vin=3.3v`
- Watchdog: healthy and armed
- Persistent configuration: available, `saved_count=6`, `pending_count=0`

The host IPv4 routes were captured before the HIL operations, including the
existing `172.29.203.0/24` NCM route. The system did not expose the
`systemd-resolved` service required by `resolvectl`, so no `resolvectl` DNS
claim is made. The HIL did not modify host routes or DNS configuration. No
user-owned TUI was present at the start; the loopback proxy on port 22719 was
the only persistent TUI-side test resource.

## Confirmation Deadline Independence

The confirmation dialog opens for any `requires_confirm` power row. The
release binary now compares the confirmation event's own timestamp to the
recorded start and only forwards when `now < started + CONFIRM_TIMEOUT`. A
late confirmation event reaches the timeout branch on the event itself and
returns the command's `timeout_message` without issuing any PUT.

| Case | PTY capture evidence | HTTP PUT log | Result |
| --- | --- | --- | --- |
| Late Enter after >3 s | red-border dialog opens, status line `Power confirmation timed out`, dialog clears, no power row state change | zero `power/*` PUT after the dialog open | PASS |
| Late Space after >3 s | same dialog, same timeout message, same clean exit | zero `power/*` PUT after the dialog open | PASS |
| Late left-click Confirm after >3 s | dialog opens for `power 12v_out off -> on`, the click reaches the timeout branch, and live state remains unchanged | zero `power/*` PUT | PASS |

The HTTP log shows only `GET /api/v1/status`, `/api/v1/adc/read`, and
`/api/v1/config` polls between the dialog open and the timeout message in
every case.

## Fresh Enter

A fresh Enter inside the 3 s window executes the hardware toggle exactly
once and the captured HTTP log records the single PUT. Cleanup restored the
row to the recorded baseline, so the final `status` reports `12v_out=off`.

| Case | PTY capture evidence | HTTP PUT log | Result |
| --- | --- | --- | --- |
| Fresh Enter on `12v_out` (off -> on) | dialog opens, Enter pressed well before 3 s, status line shows `power 12v_out=on`, the row reflects the green `on` styling | exactly one `PUT /api/v1/power/12v_out {"state":"on"}`, restored to `off` by cleanup | PASS |

The recorded proxy PUT timestamp is 2026-08-24T11:49:29.840Z, followed by
status and ADC readback GETs. The subsequent cleanup PUT that returns
`12v_out` to `off` bypassed the case-specific proxy log; the final
authoritative status records the restored `off` state.

## Modified-Key Inertness

The direct-key decoder in `cmd-ng/src/tui/direct_gpio_key.rs` only returns
`Some(ControlIntent::…)` for the six exact combinations
`('l', NONE)`, `('L', SHIFT)`, `('o', NONE)`, `('O', SHIFT)`,
`('i', NONE)`, `('I', SHIFT)`. Every other modifier combination returns
`None` and never cancels an in-flight gesture.

| Case | PTY capture evidence | HTTP PUT log | Result |
| --- | --- | --- | --- |
| `Ctrl+l` on the GPIO page | no action indicator appears, selection stays on the same cell, no `[LOW…]` tag | zero GPIO PUT | PASS |
| `Alt+o` on the GPIO page | no `[HIGH…]` tag, no state change | zero GPIO PUT | PASS |

The HTTP log between 2026-08-24T11:50:25 and 11:50:46 contains only status,
ADC, and config GETs. Super combinations and mismatched forms (lowercase with
Shift or uppercase without Shift) are verified through the deterministic
decoder tests because those representations cannot be pinned reliably in this
PTY setup.

## Valid Key Forms

The four sampled accepted direct-key forms each produce exactly one PUT and the
final GPIO settles back to `input/0`.

| Case | PTY capture evidence | HTTP PUT log | Final HTTP readback | Result |
| --- | --- | --- | --- | --- |
| `l` | `[LOW…]` then settled `○ OUT LOW J16_PIN7` | `PUT /api/v1/gpio/GP13 {"direction":"output","value":0}` | `output/0` | PASS |
| `I` (uppercase, Shift) | `[INPUT…]` then `◌ IN LOW J16_PIN7` | `PUT /api/v1/gpio/GP13 {"direction":"input"}` | `input/0` | PASS |
| `O` (uppercase, Shift) | `[HIGH…]` then `● OUT HIGH J16_PIN7` | `PUT /api/v1/gpio/GP13 {"direction":"output","value":1}` | `output/1` | PASS |
| `i` | `[INPUT…]` then `◌ IN HIGH J16_PIN7` | `PUT /api/v1/gpio/GP13 {"direction":"input"}` | `input/0` | PASS |

The four PUT timestamps are 2026-08-24T11:50:48.412Z, 11:50:49.419Z,
11:50:50.420Z, and 11:50:51.423Z. Each PUT is followed by the same
status/ADC GET pair.

## Resize Cancel Hold

The terminal `Event::Resize` cancels the in-flight GPIO gesture before the
next queued input is consumed. The hold frame is shown briefly, then the
cancelled gesture leaves no PUT on the wire.

| Case | PTY capture evidence | HTTP PUT log | Result |
| --- | --- | --- | --- |
| Left-down on GP13 followed by `Event::Resize` | `J16 GP13 ◌ IN LOW [HOLD…] J16_PIN7` rendered once, the new wide geometry repaints without `[HOLD…]`, GP13 settles back to `◌ IN LOW` | zero GPIO PUT in the resize window | PASS |

The resize first expanded the terminal to the wider geometry (the typebar
includes `PgUp/PgDn Move`), then the loopback proxy held the GP13 state
unchanged while the redraw finished. The `hit_map` was cleared on the
`Event::Resize` boundary, so any queued mouse event that arrived in the new
geometry was re-evaluated against the new rectangles rather than the old
ones.

## Resize New Geometry Click

A second `Resize` shrinks the terminal to 47 columns. The GPIO grid
collapses to a single-column layout and the row that previously held GP13
now holds GP7 (J13 MASKROM / CON_MAS). A click at the **old** row-22
coordinate must hit GP7 in the new geometry, not GP14 (the old right-column
neighbour), and that click produces exactly one LOW PUT for GP7. Cleanup
restored GP7 to `input/0`.

| Case | PTY capture evidence | HTTP PUT log | Result |
| --- | --- | --- | --- |
| 80 -> 47 redraw, click at old row-22 coordinate | the 80-column geometry maps that coordinate to GP14; after redraw the 47-column geometry maps it to `J13 MASKROM` / GP7; the click resolves to `[LOW…]` on GP7 | exactly one `PUT /api/v1/gpio/GP7 {"direction":"output","value":0}`; zero `GP14` PUT; GP7 restored to input by cleanup | PASS |

The recorded PUT timestamp is 2026-08-24T11:52:07.473Z, with status and ADC
GETs at 11:52:07.525Z and 11:52:07.551Z. The single GP7 PUT and the
absence of any GP14 PUT are the load-bearing evidence: the click hit the
new geometry rather than the stale rectangles.

## Deterministic Test-Only Items

The Super-key decoder test and the exact `CONFIRM_TIMEOUT` deadline
(`now < started + CONFIRM_TIMEOUT`) are verified through the deterministic
explicit-`Instant` unit and integration tests in the Rust suite, not
through real-board HIL. The Super case cannot be pinned to a fixed terminal
multiplexer, and the exact-deadline case is a sub-millisecond host
scheduler boundary that bounded PTY waits can only bracket. The full 461/461
Rust suite covers both.

## Transport And Recovery

- HTTP status and GPIO endpoints returned valid
  `radxa-linkr-debugger.v1` envelopes throughout the control cases.
- One 1 Hz ADC WebSocket sample was recorded through the release CLI before
  the recovery sequence. The recording carries
  `metadata.device_timing.device_t_mono_us = 626861200` with `sample_sequence = 1`
  and a single `telemetry-record` of `sequence = 1` covering the four
  channels (`5v_out`, `12v_out`, `20v_out`, `adc3`).
- CDC `vin get` returned `vin=3.3v` through the identified firmware CDC by-id
  device. Opening CDC triggered the expected USB bus re-enumeration; HTTP
  returned under a bounded recovery check.
- HTTP `POST /api/v1/bootloader` returned `ok=true` and the ROM enumerated
  as vendor `RPI`, model `RP2350`. `picotool` lacked the host permissions
  to write the combined UF2, so the recovery used `udisksctl` to mount the
  strict `VENDOR=RPI` partition and copy the combined UF2 to that mount.
- CDC shell `bootloader` independently enumerated the same ROM target. The
  same combined UF2 hash restored HTTP and watchdog health, after which a new
  one-sample WebSocket recording and CDC `vin get` both passed.

## Final State And Cleanup

After the second recovery, every HIL-owned resource was restored or
terminated:

- Only GP10 remains `output/0`; GP8 and every other advertised GPIO are
  `input/0`. GP7 returned to `input/0` after the resize new-geometry case.
- Power: `12v_out=off`, `5v_out=on`, `vdd_5v=on`, `20v_out=off`. The fresh
  Enter case turned `12v_out` on briefly and the cleanup PUT returned it to
  `off`.
- Switches: `sd=usb-reader`, `usb=pc`, `tf_wp=writable`, `vin=3.3v`. The
  HIL run did not mutate any switch route.
- Watchdog: healthy and armed.
- Persistent configuration: `saved_count=6`, `pending_count=0`.
- HTTP/NCM, one-sample WebSocket recording, and firmware CDC were available
  after the final CDC check.

A later read-only review found GP10 had drifted to `input/1` while board uptime
remained continuous and no HIL process was running. The cleanup path restored
GP10 to the recorded `output/0` baseline again, observed it for longer than one
5 s watchdog window, and saved the final authoritative readback as
`final-status-after-review.json`. No other baseline field changed.

The HIL did not issue any host route or DNS mutation. `resolvectl` could not
connect to `systemd-resolved`, so the report makes no stronger DNS-equivalence
claim; the recorded IPv4 routes still included the original default and NCM
routes after the run.

No RPI ROM disk, HIL-owned release TUI, logging proxy, PTY capture
process, listener on port 22719, or HIL PTY remained at the end of the run.
No user-owned TUI existed at the start of this HIL, so none had to be left
untouched.

## Verdict

PASS. The three event-boundary fixes (confirmation deadline independence,
GPIO direct-key modifier filter, terminal `Event::Resize` redraw boundary)
are validated on a real G3 RP2350A board through a logging proxy, against
firmware-authoritative HTTP readback. The Super-key decoder test and the
exact-deadline boundary remain deterministic-test-only and are covered by
the 461/461 Rust suite. Combined-UF2 BOOTSEL recovery worked through both
HTTP and CDC paths, with `picotool` permission fallback via `udisksctl`.
The HIL run left every recorded baseline field equal to its starting value.

## Cross-link

This report supersedes the "Post-review Current-release Delta" section of
the older
[GPIO Gestures HIL](2026-08-24-cmd-ng-tui-gpio-gestures-hil.md) because
that section's `bd6a5d…` release hash was built before the three
event-boundary fixes landed. The older report's earlier `4276…` 17-case
gesture matrix is not superseded by this report and remains historical
gesture evidence.
