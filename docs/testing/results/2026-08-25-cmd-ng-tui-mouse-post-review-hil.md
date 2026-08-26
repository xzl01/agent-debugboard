# 2026-08-25 cmd-ng TUI Mouse Post-review HIL

## Verdict

**PASS.** The current `cmd-ng` release build passed the focused post-review
real-board delta matrix. All board-visible state, persistent configuration, and
task data are field-equal to the captured baseline. The user-authorized
replacement TUI PID `3731563` remained alive at the final exact process check.

## Scope

This run validates only the host-side changes made after the earlier full TUI
mouse HIL:

- display-column-safe CJK/path rendering and three-channel scope gutters
- mouse tab selection and local Saved Config row selection/cancellation
- forced redraw after hardware or Saved Config modal close
- GPIO Down/Drag/Moved/Up cancellation over the tab row
- asynchronous Saved Config result precedence over a hardware confirmation
- universal `sd` and `tf_wp` switch confirmation, timeout, authoritative
  convergence, and TUI-driven restoration
- HTTP status, bounded ADC WebSocket recording, and CDC `vin get`

The run did not reboot, enter BOOTSEL, flash, upload OTA, save or clear persistent
configuration, mutate USB/VIN/power/GPIO state, inspect `/proc`, use tmux, or
signal/input/resize/reuse the replacement TUI or `pts/1`.

## Target And Artifacts

| Item | Value |
| --- | --- |
| Board | G3 RP2350A, HTTP `http://172.29.203.1` |
| Firmware CDC | `/dev/serial/by-id/usb-Radxa_Radxa_Linkr_Debugger_E6641C31A362C336-if00` -> `/dev/ttyACM2` |
| Release binary | `cmd-ng/target/release/radxa-linkr-debuggerctl` |
| Release version | `0.2.1` |
| Release SHA-256 | `18120637dd95607b48420eaf546174b544a4d295a00cb85c5d0a5a7826ec735c` |
| Release size | 12,969,664 bytes |
| Combined UF2 SHA-256 | `f08c7580cc3aa634996fc702e5219a2e49826a572ee97193c1a5ae284320fb06` |
| Combined UF2 size | 1,594,368 bytes |
| Evidence | `.omo/evidence/20260825-tui-mouse-post-review-release/` |

`cargo build --release --manifest-path cmd-ng/Cargo.toml` passed against the
current on-disk sources and reproduced the release hash above.

## Recovery Evidence Boundary

The earlier
[full TUI Mouse And Confirmation HIL](2026-08-25-cmd-ng-tui-mouse-confirmation-hil.md)
already passed the real HTTP BOOTSEL, CDC BOOTSEL, combined-UF2 recovery, HTTP,
WebSocket, CDC, control, and board-restoration product matrix. Its overall
historical verdict remains FAIL because its then-required user PID was absent at
the final check; this delta does not rewrite that result.

HTTP/CDC BOOTSEL and combined-UF2 recovery were deliberately **not rerun** here.
The firmware artifact is byte-identical (`f08c7580...fb06`), no firmware behavior
changed, and preserving the mandatory replacement PID `3731563` without USB
disconnect or terminal interference was an explicit constraint. The
application-only `zephyr.uf2` was not used.

## Preserved Baseline

Before the first TUI case, the real board reported:

- Power: `12v_out=off`, `5v_out=on`, `vdd_5v=on`, `20v_out=off`.
- Switches: `sd=usb-reader`, `usb=pc`, `tf_wp=protected`, `vin=3.3v`.
- GPIO: `GP10=output/0`; every other advertised GPIO was `input/0`.
- Watchdog: supported, automatic, healthy, armed, 5,000 ms timeout, no failing
  service.
- Persistent configuration: v1 snapshot present, six selected entries,
  `pending=0`; live `tf_wp=protected` intentionally differed from saved
  `writable`.
- Tasks: zero stored tasks and a zero-byte task blob; the firmware task catalog
  was captured separately.

| Projection | SHA-256 |
| --- | --- |
| Hardware/config/task projection | `4b1aa04843207d1399575e9043fd71fa5d84dabcd46fab18ad26f4c906f48bcd` |
| Normalized config GET | `84a7e9ac83705cde82b15991dfd44382f30006a61581e43b10fbd0ac9e29e7d7` |
| Normalized stored tasks GET | `320524b11c7ecac0929fb8d60bd186ee0c8999f1769ce0789bcfd7e3bd7c1e2b` |
| Normalized task catalog GET | `7f56c75dfe3f585a2437058974cc94fd939c3197b4a48304dc709fc127a922c8` |
| Decoded task blob | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |

## Real xterm.js Matrix

The release binary ran through task-owned node-pty 1.1.0 into xterm.js 6.0.0
under Playwright Core 1.55.0 and Chromium 151.0.7922.173. Chromium pointer input
produced real SGR 1006 reports delivered to each independent PTY. No tmux
capture, ANSI-to-HTML substitute, or direct model call was used.

| Case group | Result |
| --- | --- |
| Controls 80x24 and 120x32 | PASS: populated three-channel scope; gutters blank at zero-based columns 26/53 and 40/80; all firmware-discovered switch rows present |
| Saved Config local toggle/Cancel | PASS: pointer selected `power/12v_out`, local row changed to `[x]`, confirmation opened and Cancel closed it, zero config mutation |
| Status pointer tab | PASS: pointer selected Status and all dynamic switch rows rendered |
| Compact hardware queued Down | PASS: proxy-held safe response closed the first modal, same-stream redraw completed, queued Down resolved against fresh geometry to `5v_out`; zero board power PUT |
| Compact Saved Config queued Down | PASS: modal close produced a clean `[busy:save]` redraw before queued Down; proxy-crafted response never reached board persistence |
| GPIO release over tab | PASS: real Moved/Down/Drag/Up/Moved sequence waited 1,100 ms, left Controls active, removed pending markers, emitted zero GPIO PUT |
| Config `confirmation_required` precedence | PASS: async config result cleared an already-open hardware modal and installed the Saved Config confirmation; only real GETs were forwarded |
| Config `storage_error` precedence | PASS: async error cleared the hardware modal and surfaced Saved Config failure; only real GETs were forwarded |

The two compact `after-first-redraw` PNGs are same-stream prefix replays from
the live PTY ANSI chunks, before the queued second Down first appears. They are
not independently generated mock screens.

## Switch Confirmation And Restoration

Switch names, routes, and firmware `requires_confirm` values came from the live
status response. The discovered set was `sd`, `tf_wp`, `usb`, and `vin`; the
host harness contained no board switch catalog. `sd` and `tf_wp` each advertised
two reversible routes and `requires_confirm=false`, while the TUI still applied
its universal three-second confirmation gate.

| Flow | First click | Cancel/timeout | Confirm PUT | Deadline | Authoritative result |
| --- | ---: | ---: | ---: | ---: | --- |
| `sd` cancel | 0 | 0 | n/a | n/a | `usb-reader` |
| `sd` timeout | 0 | 0 | n/a | 3,300 ms wait | `usb-reader` |
| `sd` confirm | 0 | n/a | 1, `{"route":"target"}` | 148.653 ms | `target` |
| `sd` TUI restore | 0 | n/a | 1, `{"route":"usb-reader"}` | 178.383 ms | `usb-reader` |
| `tf_wp` cancel | 0 | 0 | n/a | n/a | `protected` |
| `tf_wp` timeout | 0 | 0 | n/a | 3,300 ms wait | `protected` |
| `tf_wp` confirm | 0 | n/a | 1, `{"route":"writable"}` | 132.411 ms | `writable` |
| `tf_wp` TUI restore | 0 | n/a | 1, `{"route":"protected"}` | 163.180 ms | `protected` |

Every accepted Confirm was strictly earlier than three seconds, generated one
exact route PUT, converged through firmware-authoritative readback, and settled
in the TUI. Restoration used the same fresh pointer-confirm flow, not a direct
cleanup setter.

## Request Safety Audit

The deny-by-default proxy recorded 83 exact requests:

| Class | Count | Board effect |
| --- | ---: | --- |
| Forwarded GET | 75 | Read-only status/ADC/config/switch authority |
| Forwarded approved switch PUT | 4 | `sd`/`tf_wp` alternate plus exact restoration |
| Intercepted power PUT | 1 | Local safe response; not forwarded |
| Intercepted config PUT | 3 | Crafted confirmation/error responses; not forwarded |
| Unexpected/denied request | 0 | None |

No GPIO PUT, Config Save/Clear, DELETE, power mutation, USB/VIN route, OTA, or
BOOTSEL request reached the board. `all-requests.ndjson` contains the timestamp,
method, path, body, body hash, response status/hash, forwarding decision, and
duration for every request.

## Visual Evidence

- 17 cases produced 33 fresh original PNGs.
- Every PNG captures the measured full `.xterm-screen`: 80x24 is 1120x840,
  120x32 is 1680x1120, and 80x8 is 1120x280.
- Every frame contains every terminal row, including the final keybar/status
  row.
- All 33 `tui-check` results have no overflow and
  `borderMisaligned=false`.
- Two independent read-only reviews directly inspected the originals and
  returned PASS at 0.96 confidence: one traced functional/request integrity,
  and one checked every PNG against its terminal text and `tui-check` result.

Thumbnail contact sheets are convenience indexes only. The original PNGs,
terminal text, metadata, and request logs are the authoritative evidence.

## HTTP, WebSocket, And CDC

- `curl` GET `/api/v1/status`: HTTP 200, 5,201 bytes, 34.896 ms.
- Node HTTP and release `--json status`: valid
  `radxa-linkr-debugger.v1`, `ok=true`, `command=status`.
- Bounded release `adc record`: one telemetry record, four readings,
  `sequence=5`, `sample_sequence=5`, and device monotonic/uptime aliases equal
  to `50086517400` us.
- CDC by-id `vin get` at 115200 baud: `vin=3.3v`.

## Final Equality And Cleanup

The final capture required no emergency restoration. Power outputs, switches,
GPIOs, watchdog, complete config GET, stored tasks, task catalog, and task blob
are field/byte-equal to the pretest snapshot. Final and baseline projection
hashes are both `4b1aa048...8bcd`; watchdog remained healthy and armed.

- All 17 task-owned PTYs exited with code 0 after `q`.
- Task-owned Chromium and the loopback proxy closed cleanly.
- The exact task-owned temporary npm/runtime directory was removed.
- No task-owned process, listener, or temporary resource was included in broad
  cleanup; no broad cleanup command was used.
- No command inspected `/proc` or operated a tmux client/server/session.

The single permitted final process check was `ps -p 3731563 -o pid=`. It
returned `3731563` with exit status 0. The run never signaled, attached to,
resized, sent input to, or reused the replacement TUI or `pts/1`.

## Evidence Index

- Artifacts and baseline: `pretest/summary.json`, normalized/raw endpoint
  captures, `pretest/hardware-baseline.normalized.json`, and `task-blob.bin`
- Real TUI matrix: `tui/harness-summary.json`, `tui/all-requests.ndjson`, and
  the 17 case directories
- Visual validation: `tui/validation-summary.json`, per-frame
  `*-tui-check.json`, original PNG/text/ANSI files, and contact sheets
- Transport: `final/http-status.curl.json`, `final/health-summary.json`,
  `final/ws-one-sample.ndjson`, and `final/cdc-vin-get.*`
- Final state/cleanup: `final/comparison.json`, `final/cleanup-summary.json`,
  `final/user-pid-check.json`, normalized/raw endpoint captures, and
  `final/hardware-baseline.normalized.json`
- Independent reviews: `reviews/functional.txt` and `reviews/visual.txt`
- Integrity: `SHA256SUMS` and `checksum-validation.txt`
