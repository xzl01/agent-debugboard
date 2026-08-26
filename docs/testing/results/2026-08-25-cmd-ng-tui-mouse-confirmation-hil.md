# 2026-08-25 cmd-ng TUI Mouse And Confirmation HIL

## Verdict

**Overall result: FAIL.**

The current-release G3 board, TUI interaction, transport, recovery, visual,
state-restoration, and task-owned cleanup subchecks all passed. The mandatory
user-process postcondition did not: the single permitted final
`ps -p 957210 -o pid=` check returned exit status 1 with no output. This report
therefore does not grant an overall PASS or release-acceptance claim.

The HIL did not signal, attach to, resize, send input to, reuse a terminal or
tmux session from, inspect `/proc` for, or include PID 957210 in cleanup. It did
not attempt to restart or replace the missing process. The board was left in
the exact recorded hardware/config/task baseline and all task-owned resources
were removed before the PID check.

## Scope

This run exercised the current `cmd-ng` release build on a real G3 RP2350A
Linkr Debugger. It focused on:

- populated three-channel scope spacing at 80x24 and 120x32
- real xterm.js mouse selection of Controls, Saved Config, and Status
- full-width Saved Config and switch-row selection styling
- local Saved Config selection and confirmation cancellation with no
  persistent mutation
- universal three-second TUI confirmation for firmware-advertised `sd` and
  `tf_wp`, including cancel, timeout, fresh pointer confirm, and TUI restore
- HTTP, bounded ADC WebSocket, CDC `vin get`, HTTP BOOTSEL, and CDC BOOTSEL
- exact post-reboot and final restoration of every recorded restorable field

No Rust, Web, firmware, dependency, historical report, persistent snapshot,
task blob, USB/VIN route, power output, or unrelated GPIO state was modified.
The only repository source change made by this HIL is this dated report.

## Target And Artifacts

| Item | Value |
| --- | --- |
| Board | G3, RP2350A, HTTP `http://172.29.203.1` |
| Firmware CDC | `/dev/serial/by-id/usb-Radxa_Radxa_Linkr_Debugger_E6641C31A362C336-if00` -> `/dev/ttyACM2` |
| Release binary | `cmd-ng/target/release/radxa-linkr-debuggerctl` |
| Release version | `0.2.1` |
| Release SHA-256 | `64355ba902c86811f6ca4deeb6f4f602c5010e941afdd17976c652031f236885` |
| Release size | 12,967,128 bytes |
| Combined UF2 | `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2` |
| Combined UF2 SHA-256 | `f08c7580cc3aa634996fc702e5219a2e49826a572ee97193c1a5ae284320fb06` |
| Combined UF2 size | 1,594,368 bytes |
| Evidence | `.omo/evidence/20260825-tui-mouse-confirmation-release/` |

`cargo build --release --manifest-path cmd-ng/Cargo.toml` passed. The canonical
`make firmware` sysbuild passed with FLASH 771,256/847,832 bytes (90.97%) and
RAM 469,528/532,480 bytes (88.18%). The application-only `zephyr.uf2` was never
used. Both ROM recoveries copied only the combined UF2 identified above.

## Preserved Baseline

The baseline was captured before the first BOOTSEL or TUI hardware write:

- Power: `12v_out=off`, `5v_out=on`, `vdd_5v=on`, `20v_out=off`.
- Switches: `sd=usb-reader`, `usb=pc`, `tf_wp=protected`, `vin=3.3v`.
- GPIO: `GP10=output/0`; every other advertised GPIO was `input/0`.
- Watchdog: supported, automatic, healthy, armed, 5,000 ms timeout, no
  failing service.
- Persistent configuration: v1 snapshot present, six selected entries,
  `pending=0`. The live `tf_wp=protected` value intentionally differed from
  its saved `writable` value.
- Tasks: zero stored tasks, zero-byte blob; the firmware task catalog was
  captured separately.
- Host: the existing non-NCM default route remained through `enp130s0`;
  `/etc/resolv.conf` SHA-256 was
  `7f3caa1297ba19b23497086aa9bf37b6dcce5d9ab7f0b5bdb04da9121129c7a3`.

Stable baseline hashes:

| Projection | SHA-256 |
| --- | --- |
| Restorable hardware/config/task catalog | `38745c92f947d19b14d0d673805df9d652b7245e4b691f91952f703277a11ec0` |
| Normalized full config GET | `84a7e9ac83705cde82b15991dfd44382f30006a61581e43b10fbd0ac9e29e7d7` |
| Normalized stored tasks GET | `320524b11c7ecac0929fb8d60bd186ee0c8999f1769ce0789bcfd7e3bd7c1e2b` |
| Normalized task catalog GET | `7f56c75dfe3f585a2437058974cc94fd939c3197b4a48304dc709fc127a922c8` |
| Decoded task blob | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |

## Real xterm.js Pointer HIL

The release binary ran through a task-owned node-pty into xterm.js 6.0.0 in
Playwright 1.59.1-controlled Chromium 151.0.7922.173. Browser pointer events
were converted by xterm.js into SGR 1006 reports and then delivered to the
real PTY. No tmux capture, ANSI-to-HTML substitute, mock board, direct model
call, or pasted screenshot was used.

A deny-by-default logging proxy at `http://127.0.0.1:35351` forwarded GETs to
the real board. It allowed only the exact stage-specific `sd` or `tf_wp` PUT
body after the corresponding modal had already been captured with zero PUTs.
The proxy recorded zero unexpected mutations and was closed after the run.

The harness completed 13 PTY cases with 22 PNG/text states:

- Controls 80x24 and 120x32 had populated ADC scope rows. Every header/graph
  gutter cell was blank at columns 26/53 and 40/80 respectively.
- Real pointer clicks selected Saved Config at SGR `(15,10)` and Status at
  `(29,10)`. Both pages populated from the real firmware responses.
- Clicking the first visible Saved Config row emitted SGR `(3,12)`, toggled
  local `power/12v_out` selection to `[x]`, and painted the selected row across
  all 80 columns. It emitted no config mutation.
- The Saved Config modal opened locally only after confirming the current
  draft included firmware-classified dangerous entries. A real pointer clicked
  `[ Cancel ]`; the complete four-sided modal cleared and the proxy recorded
  zero `PUT /api/v1/config`, Save, Clear, or other persistent mutation.

The Saved Config case proves modal presentation and cancellation only. It is
**not** persistent Config Confirm HIL and makes no claim that a real Save or
Clear request was confirmed.

## Universal Switch Confirmation

Both switch cases were discovered from firmware at runtime and had
`requires_confirm=false`. Their advertised two-route sets were reversible, so
the TUI's universal confirmation policy was exercised without touching USB or
VIN.

| Switch flow | First click | Cancel/timeout | Fresh Confirm | Authoritative result |
| --- | ---: | ---: | ---: | --- |
| `sd` cancel | 0 PUT | 0 PUT | n/a | remained `usb-reader` |
| `sd` timeout | 0 PUT | 0 PUT | n/a | remained `usb-reader` |
| `sd` confirm | 0 PUT before modal | n/a | 1 PUT at 462.52 ms | `target` |
| `sd` TUI restore | 0 PUT before modal | n/a | 1 PUT at 465.29 ms | `usb-reader` |
| `tf_wp` cancel | 0 PUT | 0 PUT | n/a | remained `protected` |
| `tf_wp` timeout | 0 PUT | 0 PUT | n/a | remained `protected` |
| `tf_wp` confirm | 0 PUT before modal | n/a | 1 PUT at 448.33 ms | `writable` |
| `tf_wp` TUI restore | 0 PUT before modal | n/a | 1 PUT at 463.61 ms | `protected` |

Every first click selected and full-width-reversed the switch row before
opening the modal. Every modal had a complete border and visible
`[ Confirm ]`/`[ Cancel ]` targets. The four accepted Confirm clicks arrived
strictly before three seconds and each forwarded exactly one expected route
body. Firmware-authoritative GET readback and the settled TUI row agreed.

## Visual Evidence

- 22/22 PNGs had the correct PNG signature and metadata dimensions.
- 22/22 `tui-check` results had no overflow and
  `borderMisaligned=false`; CJK rows had no wide-cell drift.
- Three fresh contact sheets cover the core, `sd`, and `tf_wp` state sets.
  Direct image inspection found no blank compositor regions, overlap, broken
  modal border, missing button, or stale selected-row artifact.
- Two independent read-only reviews returned PASS with high confidence: one
  traced the real component/event/request path, and one inspected all 22
  visual/text states for fidelity and CJK precision.

## Transport And Recovery

| Path | Result |
| --- | --- |
| HTTP BOOTSEL | PASS: `POST /api/v1/bootloader` returned `ok=true`; ROM enumerated as strict `VENDOR=RPI`, model `RP2350`, `/dev/sdd1`. |
| HTTP combined-UF2 restore | PASS: the first immediate udisks mount hit an enumeration race (`object not found`); one bounded retry mounted `/run/media/chen/RP2350`, then copied only the combined UF2. |
| CDC BOOTSEL | PASS: CDC `bootloader` produced the expected serial disconnect and the same strict RPI ROM enumeration. |
| CDC combined-UF2 restore | PASS: udisks mounted on the first attempt and copied the same combined UF2 hash. |

After each flash, the saved snapshot restored its own values. The run then
restored the two baseline-only live differences: `tf_wp` from saved
`writable` to baseline `protected`, and `GP10` from boot `input/0` to baseline
`output/0`. No USB/VIN route or power output was used for restoration.

Final transport checks passed:

- release `doctor` and HTTP status/config/task/watchdog envelopes: PASS
- bounded one-sample ADC WebSocket record: four readings, device sequence and
  monotonic timing present
- CDC `vin get`: `vin=3.3v`
- watchdog: healthy and armed

## Final State And Cleanup

The final cleanup audit required no additional board write. The complete
power, switch, GPIO, watchdog, config, stored-task, and task-catalog projections
were field-equal to the baseline. Only uptime/timestamps and live ADC/runtime
measurement values were excluded from value equality. The host route entries
had the same fields and values; only JSON array ordering changed after USB
re-enumeration. The resolver file hash was unchanged, external traffic did not
use NCM, and external DNS did not resolve to the board.

- 13 exact task-owned PTY PIDs had exited with code 0.
- Chromium close was awaited before the harness summary was written.
- Proxy port 35351 had no listener.
- No RPI ROM disk or `/run/media/chen/RP2350` mount remained.
- No task temporary artifact remained outside the retained evidence tree.
- No emergency restoration was needed after the TUI matrix.

## User Process Postcondition Failure

The single permitted final check was:

```text
ps -p 957210 -o pid=
```

It returned exit status 1 with empty stdout/stderr. Because no earlier PID
inspection was permitted, this run cannot determine when or why the process
ended. It only establishes that the required PID was absent at the final
postcondition check. No attempt was made to inspect, control, clean up, or
replace it.

This missing mandatory postcondition is the sole reason the overall verdict is
FAIL despite the complete G3 product HIL submatrix passing. The hardware itself
was left safe and byte/field-equivalent to the recorded restorable baseline.

After this verdict was recorded, the user explicitly authorized a replacement
TUI in a new independent terminal session. The current debug binary is running
as PID `3205091` on `pts/1` in tmux session `linkr-debug-tui-restored`. This does
not recreate PID 957210 or change the historical HIL verdict above.

## Evidence Index

- Baseline and hashes: `pretest/`, `artifacts.json`, `run-manifest.json`
- HTTP recovery: `http-bootsel/`
- Real TUI pointer matrix: `tui/harness-summary.json`, `tui/all-requests.ndjson`,
  and the 13 case directories under `tui/`
- Visual checks: `tui/png-hygiene.json`, `tui/tui-check-summary.json`, and
  `tui/contact-sheet-*.png`
- CDC recovery: `cdc-bootsel/`
- Final state/cleanup/PID: `final/comparison.json`,
  `final/cleanup-audit.json`, `final/health-summary.json`, and
  `final/user-pid-check.json`
