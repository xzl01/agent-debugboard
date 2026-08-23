# 2026-08-09 cmd-ng TUI Controls And `rdb` HIL

## Verdict

The real-hardware CLI/TUI control flow and `rdb` distribution alias: **PASS**.

This run validates the current Rust release binary against an RP2350 Linkr
Debugger. It covers keyboard and mouse control, section navigation, GPIO level
presentation, hardware confirmation, terminal cleanup, the `rdb` alias, both
BOOTSEL paths, and recovery with the canonical combined UF2.

## Target And Artifacts

- Board: Radxa Linkr Debugger, RP2350, USB NCM HTTP at
  `http://172.29.203.1`.
- CDC ACM: `usb-Radxa_Radxa_Linkr_Debugger_E6641C31A362C336-if00`, resolved
  to `/dev/ttyACM2` for this run.
- Combined ROM BOOTSEL image:
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`, 1,632,768
  bytes, SHA256
  `498f1e3d43bbfeb87253c1c0b85137a0a6815e330af9f900f70f060a16c6f535`.
- MCUboot OTA image:
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350-ota.bin`, 790,352
  bytes, SHA256
  `b5aadc041acd69a613e587220042691c97dd8901c626958b3368371feb59dd90`.
- Current Rust release binary: `cmd-ng/target/release/radxa-linkr-debuggerctl`,
  12,403,344 bytes, SHA256
  `e1966236f159d00b23a2ed1bca10e06357c2c80d440f5a1d9140dbbde3d55f63`.
- Only `radxa-linkr-debugger-rp2350.uf2` was used for ROM BOOTSEL recovery.
  The app-only `zephyr.uf2` was never flashed.

## Preserved Board Baseline

The board started with an existing v1 snapshot containing eight selected
items. It was deliberately preserved rather than cleared:

- `12v_out`, `5v_out`, `vdd_5v`, and `20v_out`: on.
- SD route: `usb-reader`; USB route: `pc`; TF write protect: `writable`;
  VIN: `3.3v`.
- GPIOs were returned to input at the end of each GPIO test.

The snapshot remained present and fully applied after both combined-UF2
recoveries. No persistent configuration Save or Clear operation was issued.

## TUI Keyboard And Confirmation

The current release binary was run in a real 140x48 PTY against the board.

| Check | Result | Board evidence |
| --- | --- | --- |
| Fixed footer | PASS | General controls appeared only on the bottom row; confirmation replaced the footer with confirmation-specific guidance. |
| Section navigation | PASS | Repeated Tab selected Power, Switch, Target recovery, GPIO, then wrapped to Power; Shift+Tab returned to the previous section. |
| Power first activation | PASS | Enter on `12v_out` displayed the centered red `Confirm Power Toggle` dialog without changing the board; HTTP still reported `state=on`. |
| Power cancellation | PASS | Esc and the modal Cancel mouse target both dismissed the dialog; `12v_out` remained on. |
| Power confirmation | PASS | Enter followed by Enter changed `12v_out` from on to off; the same confirmed sequence restored it to on. |
| VIN cancellation | PASS | Selecting VIN at `3.3v` displayed `Confirm Switch Route` for `switch vin -> 1.8v`; Esc left HTTP readback at `3.3v`. No 1.8 V operation was issued. |
| Paused timeout | PASS | A Power confirmation opened while polling was paused and timed out after three seconds without executing; the status read `Power confirmation timed out`. |
| Post-fix safe Switch control | PASS | The rebuilt release routed `tf_wp` from `writable` to `protected`, showed the converged cyan state, and restored `writable`; HTTP readback matched both operations. |

The first TUI attempt used an older release artifact and did not show the
modal. An identical current-source debug build did show it, isolating the
problem to the stale executable rather than the event handler. Rebuilding with
`cargo build --release --manifest-path cmd-ng/Cargo.toml` made the release
artifact pass the same real-PTY input sequence. No source workaround was
required.

## GPIO Keyboard, Mouse, And Color

| Flow | Result | HTTP readback |
| --- | --- | --- |
| Keyboard primary action on GP8 | PASS | `input/0 -> output/1 -> output/0`. |
| Keyboard `i` on GP8 | PASS | Returned to `direction=input`. |
| Mouse left click on GP13 | PASS | `input/0 -> output/1 -> output/0`. |
| Mouse right click on GP13 | PASS | Returned to `direction=input`. |
| LOW presentation | PASS | xterm.js rendered LOW with the black GPIO badge style. |
| HIGH presentation | PASS | xterm.js rendered GP8 `out=1` with a red, bold value while retaining the selected background. |

During manual coordinate testing, GP15 was observed as `output/1` even though
it was not the intended target. It was immediately restored through the HTTP
input operation. Final GPIO enumeration after the CDC BOOTSEL recovery reported
every allowlisted GPIO, including GP8, GP13, GP15, and ADC-owned GP29, as
`direction=input`.

## Mouse Hit Testing

- A left-button SGR mouse press on the GP13 chip used the same state-changing
  path as keyboard activation.
- A right-button press on the same chip restored input mode.
- A left-button press on `12v_out` opened the confirmation without changing
  hardware.
- Clicking the modal Cancel target dismissed the dialog and did not activate
  the underlying GPIO row.

## `rdb` Alias

The repository-local installer rebuilt the current Rust release and created
the relative Unix link `rdb -> radxa-linkr-debuggerctl`.

| Check | Result |
| --- | --- |
| Resolved inode | PASS: `rdb` and the primary command resolved to inode `236432418`. |
| Version | PASS: both printed `radxa-linkr-debuggerctl 0.2.1`. |
| Stable HTTP JSON | PASS: both returned byte-identical `watchdog status` JSON with `healthy=true` and `armed=true`. |
| Live status | PASS: both commands reached the board; separately sampled runtime fields were allowed to differ by sampling time. |

The host environment does not expose Nix's `libudev.so.1` on the default
runtime search path. HIL therefore supplied the Nix `systemd-minimal-libs`
path through `LD_LIBRARY_PATH`; this is an environment/runtime packaging issue,
not a CLI response failure.

## HTTP And CDC Recovery

| Recovery path | Result | Evidence |
| --- | --- | --- |
| HTTP BOOTSEL | PASS | `POST /api/v1/bootloader` returned `ok=true`; RP2350 enumerated as a 128 MiB ROM BOOTSEL volume; the canonical combined UF2 was copied through the mounted `RP2350` volume. |
| HTTP recovery boot | PASS | NCM HTTP and `/dev/ttyACM2` returned; watchdog was healthy and armed; the eight-item snapshot remained applied. |
| CDC shell | PASS | `vin get` returned `vin=3.3v`. |
| CDC BOOTSEL | PASS | CDC `bootloader` enumerated the same RP2350 ROM volume; the same combined UF2 was copied again. |
| CDC recovery boot | PASS | NCM HTTP and CDC returned; watchdog remained healthy and armed; all GPIOs were input. |

`picotool` detected the RP2350 but could not open it under the current USB
permissions, and passwordless sudo was unavailable. The unprivileged
`udisksctl` mount-and-copy path was therefore used for both recoveries.

## Terminal Cleanup

The TUI was launched from an interactive shell, exited with `q`, and followed
by `stty -a`. The shell returned normally with `isig icanon iexten echo` and
normal output processing enabled. The alternate screen and mouse capture were
released; no raw-mode terminal state leaked into the shell.

## Visual Evidence

Fresh evidence was rendered through a real node-pty and xterm.js in headless
Chromium, not through tmux:

- `.omo/evidence/20260809-cmd-ng-tui-visual-final/main-140x48/terminal.png`
- `.omo/evidence/20260809-cmd-ng-tui-visual-final/modal-140x48/terminal.png`
- `.omo/evidence/20260809-cmd-ng-tui-visual-final/gpio-high-140x48/terminal.png`
- `.omo/evidence/20260809-cmd-ng-tui-visual-final/main-80x24/terminal.png`
- `.omo/evidence/20260809-cmd-ng-tui-visual-final/switch-mismatch-140x48/terminal.png`

For all four 140x48 captures, the width checker reported 48 lines,
`maxWidth=140`, no overflow lines, aligned borders, and no wide-character
column drift. The 80x24 capture reported 24 lines, `maxWidth=80`, the same clean
overflow/border/width results, and usable two-row control reflow. Every capture
metadata file records the xterm.js true-color path and a killed PTY cleanup
receipt.

The first independent integrity review found that an expired Switch request
discarded its desired route, making the red mismatch state unreachable through
the real snapshot transition. A failing regression test reproduced the reset
(`target` observed instead of expected `usb-reader`). The model now latches a
local route intent, clears only its yellow pending window at the deadline,
keeps the desired/actual mismatch red, and releases the intent when readback
converges. The deterministic final screenshot shows red
`switch sd [usb-reader]`, `sd desired = usb-reader`, and
`sd actual = target`; it also captures gray `power 20v_out [off]`.

Two independent read-only visual reviews inspected the complete final 5/5
evidence set and returned high-confidence **PASS** verdicts with no blockers.
They confirmed the reachable red Switch mismatch, gray Power OFF, green Power
ON, cyan ready Switch, GPIO black/red badges, centered red modal, fixed footer,
and 80x24 reflow. The only non-blocking observations were the intentionally
small GPIO value badge and truncation of lower-priority footer shortcuts at 80
columns; core help remains visible.

## Final Board State

- Watchdog: supported, automatic, healthy, armed; no failing service.
- Power: all four user-visible outputs on, matching the preserved snapshot.
- Switches: SD `usb-reader`, USB `pc`, TF write protect `writable`, VIN `3.3v`.
- GPIO: every enumerated GPIO input; GP29 remained input-only under ADC3
  ownership.
- Persistent configuration: v1 snapshot present, eight selected items,
  `pending=0`; no snapshot was cleared or rewritten.

## Local Validation Boundary

The real-hardware results above are distinct from local validation. The
affected Rust gates passed in this validation run: formatting, Clippy with
warnings denied, and 275 tests. The `rdb` alias fixture suite, workflow
contract checks,
PowerShell parse/dry-run, ShellCheck, Actionlint, Nix flake check, and canonical
`make firmware` build also passed. The current-source release binary was rebuilt
again before the final TUI run.

The complete repository is not described as fully green: the current worktree's
`check-persistent-configuration-docs.mjs` still rejects unrelated
persistent-configuration documentation drift in
`skills/radxa-linkr-debugger/SKILL.md` and `doc/persistent-configuration.md`
(missing required headings, policy/current-sync markers, HIL link/boundary
text, and one dangerous-save example). Its fixture tests pass, and this TUI/rdb
run did not change those contracts.
