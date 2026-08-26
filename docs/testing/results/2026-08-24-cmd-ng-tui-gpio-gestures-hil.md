# 2026-08-24 cmd-ng TUI GPIO Gestures HIL

This is the historical gesture-evidence report for the predecessor
`4276…` 17-case matrix and its post-review `bd6a5d…` current-release
delta. It supersedes the historical
[GPIO Direct Actions HIL](2026-08-24-cmd-ng-tui-gpio-direct-actions-hil.md).

> **Supersession note**: the "Post-review Current-release Delta" section
> below was built on the `bd6a5d…` release binary and predates the three
> TUI event-boundary fixes. Current evidence for the confirmation deadline
> independence, GPIO direct-key modifier filter, and terminal `Event::Resize`
> redraw boundary lives in the newer
> [TUI Event Boundaries HIL](2026-08-24-cmd-ng-tui-event-boundaries-hil.md).
> The earlier `4276…` 17-case gesture matrix above remains historical
> gesture evidence and is not superseded.

## Scope

This run validated the then-current `cmd-ng` TUI GPIO keyboard and mouse contract on
a real G3 RP2350A Linkr Debugger. It covers the `l`/`o`/`i` direct keys, retired
key and mouse-button inertness, short/hold/double left-button gestures,
cell-level movement tolerance, cancellation, pending/HOLD presentation,
authoritative HTTP readback, HTTP/WebSocket/CDC transport, and both ROM BOOTSEL
recovery paths.

## Artifacts

- Release binary: `cmd-ng/target/release/radxa-linkr-debuggerctl`
- Release binary SHA-256:
  `bd6a5d440f1ce13318c5044cf958181ccd98d984ede100c5eabc0b22ef1b9389`
- Release binary size: 12,956,080 bytes
- Full 17-case matrix predecessor SHA-256:
  `4276f780dfdf82c66253b07c8abe4d93bda3865743330bd2f83b78bc074eb21e`
- Combined UF2:
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`
- Combined UF2 SHA-256:
  `37d48dd7ecac90834bbf3ef28bf66e7ce6a852f3e62543e1acb2d8e43ee23e71`
- Combined UF2 size: 1,710,080 bytes
- Firmware CDC by-id:
  `/dev/serial/by-id/usb-Radxa_Radxa_Linkr_Debugger_E6641C31A362C336-if00`

The canonical firmware build passed with FLASH 828,948/847,832 bytes (97.77%)
and RAM 508,744/532,480 bytes (95.54%). Only the combined UF2 was used for ROM
BOOTSEL. The application-only `zephyr.uf2` was never used.

## Local Gates

- Rust fmt and Clippy with `-D warnings`: PASS
- Rust tests: 452/452 PASS
- Nix flake checks, repository gates, test registration, document layout, and
  skill boundary: PASS
- Web Node tests 392 passed / 1 skipped, Vitest 427/427, production build: PASS
- Fresh xterm.js visual QA at 47/48/80/120 columns: PASS
- Dual visual/CJK review: PASS

The exact 599/600/601 ms Up and 219/220/221 ms second-Down boundaries are
covered by deterministic explicit-`Instant` tests. Real browser HIL confirms
the same behavior with bounded waits but does not claim sub-millisecond host
scheduler precision.

## Post-review Current-release Delta

Code review found one event-order boundary after the full matrix: when an
await-second LOW deadline has expired, a non-GPIO left Down dequeued before the
runtime tick must settle the original GPIO LOW and consume that click. The fix
does not change rendering, keyboard actions, ordinary short/hold/double timing,
transport code, firmware, or recovery behavior.

The current release binary above was rebuilt after that fix and validated as
follows:

- The exact event-before-tick boundary is covered by a deterministic
  explicit-`Instant` integration test; the complete Rust suite is 452/452.
- In a real 80x24 PTY through the logging proxy, a GP13 short release followed
  by a later Power click produced exactly one GP13 `output/0` PUT and
  authoritative `output/0` readback. In that host-scheduled run the normal tick
  settled LOW before the Power event, so the Power confirmation opened as
  expected; this run does not claim exact-deadline scheduler precision.
- A second current-release PTY run sent `i` on GP13 and produced exactly one
  `direction=input` PUT with authoritative `input/0` readback.
- The current release recorded one valid 1 Hz ADC WebSocket telemetry sample.
- Final status preserved Power, switches, watchdog health, persistent-config
  counts, GP10 `output/0`, and all other GPIOs `input/0`.

Current-release delta evidence is under
`.omo/evidence/20260824-tui-gpio-gestures-release/current-release-delta/`.
The unchanged 17-case visual and interaction matrix below remains evidence from
the explicitly identified predecessor release hash.

## Baseline

Before HIL:

- GP13: `input/0`
- GP8 and GP10: `output/0`; every other advertised GPIO: input
- Power: `12v_out=off`, `5v_out=on`, `vdd_5v=on`, `20v_out=off`
- Switches: `sd=usb-reader`, `usb=pc`, `tf_wp=writable`, `vin=3.3v`
- Watchdog: healthy and armed
- Persistent configuration: available, `saved_count=6`, `pending_count=0`

An independent user-owned debug TUI was present. It was not launched,
controlled, or terminated by this HIL run.

## Keyboard Matrix

Every full-matrix case used the predecessor release binary identified above
through a real PTY rendered by
xterm.js in Chromium. A loopback logging proxy forwarded requests to the real
board and recorded every request body. HTTP readback was checked after each
action.

| Case | GP13 PUT log | Authoritative result | Result |
| --- | --- | --- | --- |
| `l` | one `output/0` | `output/0` | PASS |
| `o` | one `output/1` | `output/1` | PASS |
| `i` | one `input` | `input` | PASS |
| Enter, Space, `0`, `1` | none | unchanged `input/0` | PASS |
| 120x32 `l` | one `output/0` | `output/0` | PASS |

The TUI selection remained on GP13. `l` did not move to its right-hand sibling;
Right-arrow navigation remains covered by deterministic integration tests.

## Mouse Gesture Matrix

| Case | Request evidence | Authoritative result | Result |
| --- | --- | --- | --- |
| Middle and right buttons | no GP13 PUT | unchanged `input/0` | PASS |
| Short click | one `output/0` after await-second expiry | `output/0` | PASS |
| Held before 600 ms | no GP13 PUT; `[HOLD…]` visible with `IN LOW` | unchanged `input/0` | PASS |
| Hold through 600 ms and release | one `output/1`; no second PUT | `output/1` | PASS |
| Same-pin double click | exactly one `input`; no output PUT | `input` | PASS |
| Second Down after await deadline | exactly one expired `output/0`; no Input | `output/0` | PASS |
| Same-cell pointer movement while held | one eventual `output/1` | `output/1` | PASS |
| Cross-cell Drag/release | no GP13 PUT | unchanged `input/0` | PASS |
| Escape during hold | no GP13 PUT | unchanged `input/0` | PASS |
| Tab during hold | no GP13 PUT; Saved Config page shown | unchanged `input/0` | PASS |
| Pause during hold | no GP13 PUT | unchanged `input/0` | PASS |
| HOLD followed by keyboard `o` | one delayed `output/1`; `[HIGH…]` replaces HOLD | board remained `input/0` before delayed forwarding | PASS |

The proxy logs show that the double-click path produced no transient LOW and
that all cancellation cases emitted no GP13 mutation. Same-cell browser motion
kept the gesture active; crossing to the sibling cell cancelled it.

Evidence is under `.omo/evidence/20260824-tui-gpio-gestures-release/` and
contains PNG, terminal text, raw ANSI, and cleanup metadata for every case.
Request logs are under `/tmp/opencode/hil-*.ndjson` for this run.

## Transport And Recovery

- HTTP status and GPIO endpoints returned valid
  `radxa-linkr-debugger.v1` envelopes.
- The release CLI recorded a one-sample 1 Hz ADC WebSocket stream before and
  after recovery.
- CDC `vin get` returned `vin=3.3v` through the identified firmware CDC path.
- HTTP `POST /api/v1/bootloader` independently enumerated the strict
  `VENDOR=RPI`, model `RP2350` ROM disk; the combined UF2 restored HTTP and CDC.
- CDC shell `bootloader` independently enumerated the same ROM target; the
  same combined UF2 hash restored HTTP, WebSocket, CDC, and watchdog health.

## Final State And Cleanup

After the second recovery, the persisted snapshot had changed GP8 and GP10 to
input. The HIL cleanup restored the recorded pre-test baseline explicitly:

- GP8 and GP10: `output/0`
- GP13: `input/0`
- Every other advertised GPIO: input
- Power and Switch states: byte-for-field equal to the baseline above
- VIN: `3.3v`
- Watchdog: healthy and armed
- Persistent configuration: `saved_count=6`, `pending_count=0`

No RPI ROM disk, HIL-owned release TUI, logging proxy, xterm capture process,
listener on ports 22701-22717, or HIL PTY remained. The independent user-owned
debug TUI was deliberately left untouched.

## Verdict

PASS. The full matrix, deterministic boundary coverage, and focused
current-release board regression together validate the requested `i`/`o`/`l`
GPIO keys and Web-parity left-button gesture UX. Firmware-authoritative state,
transport and recovery checks passed, and each run restored its recorded board
baseline.
