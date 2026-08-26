# 2026-08-25 cmd-ng TUI Concurrency Final HIL

## Verdict

**Functional HIL matrix: PASS. Release acceptance: FAIL due to the protected-process postcondition.**

The current `cmd-ng` release passed the focused real-board concurrency matrix
through task-owned node-pty, xterm.js, Chromium, and a deny-by-default loopback
proxy. GPIO keyboard and mouse pending states, Saved Config clear/save busy
states, existing hardware modals, and stale-refresh boundaries all passed. The
proxy forwarded 47 real read-only GETs and zero mutations; all seven TUI
mutation requests were intercepted locally.

The mandatory final command `ps -p 3731563 -o pid=` returned exit status 1 with
empty output. No input, signal, resize, attach, `/proc` inspection, tmux
operation, or terminal reuse targeted that PID or `pts/1`. Because the required
protected process was absent, this report does not claim release-level PASS even
though the product and board matrix passed.

## Scope

This run validates the current Rust release after the TUI concurrency fixes:

1. GPIO keyboard `l`, followed by one queued Power and one queued Switch SGR
   Down. The first `l` redraw showed GPIO pending; both queued controls stayed
   inert and emitted no Power/Switch mutation.
2. GPIO keyboard `l`, followed by queued Saved Config row and tab SGR Downs.
   The first redraw remained Controls with the GPIO pending state; no local row
   selection or page hit occurred before the fresh render boundary.
3. Keyboard Saved Config clear and save workers, each followed by queued row and
   Status-tab Downs. The first worker redraw showed `[busy:clear]` or
   `[busy:save]`; each operation emitted exactly one local config request and
   preserved row/page state at that frame.
4. Mouse GPIO pending, followed by a queued Power and Switch SGR Down burst.
   The pending `[HIGH...]` state remained visible and neither control opened a
   modal or emitted a mutation.
5. Existing Power/Switch modal-before-request and stale-refresh/stale-hit
   boundaries remained PASS.

No product source, firmware, Web UI, persistent configuration, task blob,
historical report, BOOTSEL, flash, or OTA artifact was changed by the HIL.
The only requested product-document addition is this report.

## Target And Artifacts

| Item | Value |
| --- | --- |
| Board | G3 RP2350A, HTTP `http://172.29.203.1` |
| Firmware CDC | `/dev/serial/by-id/usb-Radxa_Radxa_Linkr_Debugger_E6641C31A362C336-if00` -> `/dev/ttyACM2` |
| Git HEAD | `a03ef6206d38fe797d3e874c37d7431f263a9ca5` |
| cmd-ng source files | 152 Cargo/Rust inputs |
| Source manifest SHA-256 | `4a2d53899dc6cbf22f46c55d103b536e0bbeaeef84caac8fd2174e58142c5eea` |
| Current tracked cmd-ng diff SHA-256 | `c7bd5a65a12231515737590dde7a9c2bb14368c4576cbbfad7750f45d8c081b9` |
| Current release | `cmd-ng/target/release/radxa-linkr-debuggerctl`, version `0.2.1` |
| Release SHA-256 | `fcfefdae0b008b9d3a7793140029d396a387a5f024848ba7bd47803c304f7322` |
| Release size | 12,969,592 bytes |
| Combined UF2 SHA-256 | `f08c7580cc3aa634996fc702e5219a2e49826a572ee97193c1a5ae284320fb06` |
| Combined UF2 size | 1,594,368 bytes |
| Evidence | `.omo/evidence/20260825-tui-concurrency-final-release/` |

`cargo build --release --locked --manifest-path cmd-ng/Cargo.toml` completed
successfully. The release hash above is the freshly rebuilt current artifact;
the combined UF2 is byte-identical to the earlier reports.

## Preserved Baseline

The pretest authoritative projection reported:

- Power: `12v_out=off`, `5v_out=on`, `vdd_5v=on`, `20v_out=off`.
- Switches: `sd=usb-reader`, `usb=pc`, `tf_wp=protected`, `vin=3.3v`.
- GPIO: `GP10=output/0`; every other advertised GPIO was `input/0`.
- Watchdog: supported, automatic, healthy, armed, 5,000 ms timeout, no failing
  service.
- Persistent configuration: v1 snapshot present, six selected entries,
  `pending=0`.
- Tasks: zero stored tasks and the zero-byte task blob; the firmware task
  catalog was captured separately.

The direct GP10 restoration record shows the authoritative `/api/v1/gpio/GP10`
GET was already `output/0` before and after the pretest guard, so no GP10 PUT
was needed (`allowedMutationCount=0`). No persistent configuration or other
hardware output was touched.

| Projection | Pretest SHA-256 | Final SHA-256 |
| --- | --- | --- |
| Hardware/config/task projection | `4b1aa04843207d1399575e9043fd71fa5d84dabcd46fab18ad26f4c906f48bcd` | same |
| Normalized config GET | `84a7e9ac83705cde82b15991dfd44382f30006a61581e43b10fbd0ac9e29e7d7` | same |
| Normalized stored tasks GET | `320524b11c7ecac0929fb8d60bd186ee0c8999f1769ce0789bcfd7e3bd7c1e2b` | same |
| Normalized task catalog GET | `7f56c75dfe3f585a2437058974cc94fd939c3197b4a48304dc709fc127a922c8` | same |
| Decoded task blob | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` | same |

`final/comparison.json` reports `baselineEqual=true`, `taskBlobEqual=true`,
`sourceEqual=true`, `sourceDiffEqual=true`, `releaseEqual=true`,
`combinedUf2Equal=true`, `watchdogHealthy=true`, and `watchdogArmed=true`.

## Real xterm.js Concurrency Matrix

The release binary ran through task-owned node-pty `1.1.0`, xterm.js `6.0.0`,
Playwright Core `1.55.0`, and Chromium `151.0.7922.173`. Chromium keyboard
events and CDP pointer events were converted by xterm.js into real terminal
bytes. Each requested queued burst was delivered as one PTY write. No tmux,
ANSI-to-HTML substitute, pasted screen, direct model call, or mock board data
was used.

| Case | Frames | Result | Key proof |
| --- | ---: | --- | --- |
| GPIO `l` + queued Power/Switch Down | 3 | PASS | `[LOW...]` first redraw; GPIO only, Power/Switch zero |
| GPIO `l` + queued Saved Config row/tab | 3 | PASS | Controls page and selection unchanged at first redraw |
| Config clear + queued row/tab | 3 | PASS | `[busy:clear]`, one intercepted DELETE, no early row/page mutation |
| Config save + queued row/tab | 3 | PASS | `[busy:save]`, one intercepted PUT, no early row/page mutation |
| Mouse GPIO pending + queued Power/Switch Down | 4 | PASS | `[HIGH...]` persisted; no hardware modal |
| Power Down + Enter modal boundary | 3 | PASS | Modal frame preceded local PUT |
| Switch Down + Enter modal boundary | 3 | PASS | Modal frame preceded local PUT |
| Refresh + stale Power Down | 3 | PASS | Fresh transformed GET cleared old coordinate before stale Down |
| Refresh + stale Switch Down | 3 | PASS | Fresh transformed GET cleared old coordinate before stale Down |

All nine PTYs were `/dev/pts/18` and exited with code zero. Since the required
protected process was absent in this environment, the harness reserved
`/dev/pts/1` with a task-owned dummy PTY before spawning cases; it sent no input
to that reservation and closed it during cleanup. No TUI PTY used `pts/1`.

## Request Safety Audit

The final deny-by-default proxy recorded 54 exact TUI requests:

| Class | Count | Board effect |
| --- | ---: | --- |
| Forwarded GET | 47 | Read-only status, ADC, config, and switch authority |
| Intercepted GPIO PUT | 3 | Local synthetic success; not forwarded |
| Intercepted Config DELETE | 1 | Local synthetic success; not forwarded |
| Intercepted Config PUT | 1 | Local synthetic success; not forwarded |
| Intercepted Power PUT | 1 | Local synthetic success; not forwarded |
| Intercepted Switch PUT | 1 | Local synthetic success; not forwarded |
| Forwarded mutation | 0 | None |
| Unexpected/denied request | 0 | None |

The exact ledger is `tui/all-requests.ndjson`; each request has method, path,
body hash, timestamps, forwarding decision, response status/hash, and duration.
No Power, Switch, GPIO, Config Save/Clear, task, OTA, BOOTSEL, or other TUI
mutation reached the board.

## Visual Evidence

- Nine sessions produced 28 fresh original PNG/text/ANSI frame sets.
- Every 80x24 frame is a full 1120x840 `.xterm-screen`; every 80x30 frame is
  a full 1120x1050 `.xterm-screen`.
- Every frame contains all terminal rows and a nonblank final keybar.
- All 28 `tui-check` results report `maxWidth` equal to the expected width,
  no overflow, `borderMisaligned=false`, and no wide-character drift.
- PNG signatures, dimensions, ANSI prefixes, and full-height coverage passed.
- Two independent read-only reviews returned PASS with HIGH confidence.

The busy marker occupies the first Saved Config table cell while the worker is
active; this is the current renderer's intentional status placement, not a
capture defect. The 80-column context keybar is visually tight but remains
within the exact measured width and is fully present in every frame.

## HTTP, WebSocket, CDC, And Final Equality

- Pretest curl status: HTTP 200, 5,201 bytes, valid
  `radxa-linkr-debugger.v1` success envelope.
- Final curl status: HTTP 200, 5,201 bytes, valid success envelope.
- Current release CLI status: valid success envelope before and after the matrix.
- Final bounded release WS read: one telemetry record with four readings,
  `sample_sequence=10`, `device_t_mono_us=69381985300`.
- Final CDC at 115200 baud: read-only `vin get` returned `vin=3.3v`.
- Watchdog remained healthy and armed; final projection and task blob are exact
  equals of pretest.

## Protected Process Postcondition

The required final command was run exactly once at the final postcondition:

```text
ps -p 3731563 -o pid=
```

It returned exit status 1 with empty stdout/stderr. The machine had no process
with that PID at the check. No attempt was made to inspect, restart, replace,
signal, attach to, resize, or send input to the missing process. This is the
sole reason the overall release acceptance remains FAIL.

## Historical Evidence Boundary

The earlier full mouse/confirmation report remains a historical overall FAIL
because its then-required PID was absent; this run does not rewrite it:

- [Historical full mouse and confirmation HIL](2026-08-25-cmd-ng-tui-mouse-confirmation-hil.md)
- [Mouse post-review HIL](2026-08-25-cmd-ng-tui-mouse-post-review-hil.md)
- [Keyboard modal HIL](2026-08-25-cmd-ng-tui-keyboard-modal-hil.md)
- [Final event-boundary HIL](2026-08-25-cmd-ng-tui-final-event-boundary-hil.md)

Those reports use earlier release hashes where applicable. The current release
hash and current source manifest are recorded above and in `pretest/summary.json`.

## Evidence Index

- Current artifact/source/baseline: `pretest/summary.json`,
  `pretest/source-files.SHA256SUMS`, `pretest/cmd-ng-source.diff`, endpoint
  captures, and `pretest/hardware-config-task-baseline.normalized.json`
- GP10 guard: `pretest/gp10-restoration.json` and raw before/after GETs
- Real TUI matrix: `tui/harness-summary.json`, `tui/validation-summary.json`,
  `tui/all-requests.ndjson`, and the nine case directories
- Visual checks: per-frame `*-tui-check.json`, original PNG/text/ANSI files,
  and `reviews/visual.txt`
- Transport/equality: `final/health-summary.json`, CDC captures,
  `final/comparison.json`, and final endpoint captures
- Cleanup/protected process: `final/cleanup-summary.json` and
  `final/user-pid-check.json`
- Independent reviews: `reviews/functional.txt` and `reviews/visual.txt`
- Integrity: `SHA256SUMS` and `checksum-validation.txt`
