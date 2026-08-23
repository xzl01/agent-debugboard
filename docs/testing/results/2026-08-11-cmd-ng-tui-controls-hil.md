# 2026-08-11 cmd-ng Adaptive Scope And GPIO Controls Write HIL

## Verdict

The current-source Rust CLI/TUI write path on a real RP2350 Linkr Debugger:
**PASS**.

This run validates the adaptive three-channel scope and firmware-metadata GPIO
projection together with their real control surface. It covers keyboard and
browser-pointer GPIO writes, Power confirmation and execution, Switch writes,
terminal cleanup, HTTP/WebSocket transport, CDC fallback, both BOOTSEL entry
paths, and recovery using the canonical combined UF2.

No persistent configuration Save/Clear, task mutation, VIN 1.8 V change, USB
route change, 12 V enable, or 20 V enable was issued.

## Target And Artifacts

- Board: Radxa Linkr Debugger G3, RP2350, HTTP at
  `http://172.29.203.1`.
- USB identity: `2fe3:db01`, serial `E6641C31A362C336`.
- NCM interface: `eth0`, `172.29.203.10/24`.
- CDC ACM: `usb-Radxa_Radxa_Linkr_Debugger_E6641C31A362C336-if00`,
  resolved to `/dev/ttyACM2`.
- Combined MCUboot and application UF2:
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`,
  1,634,304 bytes, SHA256
  `da6cd49efbd15516f6f2390614dbc71718f9bc45ab8368ba6de9e5102e7031c6`.
- Application-only UF2:
  `build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.uf2`,
  1,582,592 bytes, SHA256
  `c20bdd1834422d38207625fea04d609158c2cff0e9b939698e897669669f0fbf`.
  This artifact was recorded only as a negative guard and was never flashed.
- MCUboot OTA image:
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350-ota.bin`,
  791,116 bytes, SHA256
  `c6278b9ce5487ef0f7a9bfe6dc2450437983d1e9a6bb1292921b4ae49d5ed62a`.
  OTA was outside this run and the image was not uploaded.
- Current Rust release binary:
  `cmd-ng/target/release/radxa-linkr-debuggerctl`, 12,348,984 bytes,
  SHA256
  `c5a15069069167b5edfd430fd1338bfbb2a9aac8857f252ec5d215d740cd25a2`.

## Preserved Baseline

The baseline was captured before the first hardware write:

- Power: `12v_out=off`, `5v_out=on`, `vdd_5v=off`, `20v_out=off`.
- Switches: `sd=usb-reader`, `usb=target`, `tf_wp=writable`,
  `vin=3.3v`.
- GPIO: all 15 advertised GPIOs were `direction=input`, `value=0`.
- Watchdog: supported, automatic, healthy, armed, timeout 5,000 ms, no
  failing service.
- Persistent config: v1 snapshot present, `saved_count=1`, `pending_count=0`.
  The selected saved item was `power/5v_out=on`.
- Tasks: `task_count=0`, empty blob.
- Host default route: `192.168.2.1` through `enp130s0`.
- `/etc/resolv.conf` SHA256:
  `4984fdde89decfaaef0be7e02ff08fea8dd88a9807447226fa92812d5a8cff84`.

Stable baseline hashes:

| Projection | SHA256 |
| --- | --- |
| Normalized status | `163c416a8ef0ec0cf8d712485d22b94eda85111789bbd473e5d8739e5aa5800b` |
| Sorted config response | `825a3c4fb60cd336729e9c8e45c2a6f47af8e0a893dacea6e4b2bbfd3f490b1a` |
| Sorted task response | `92d416a8334c4f1c514467df46c5b5cd030e8ec0f9be17aa59c75357992824a6` |
| Decoded task blob | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |

Preflight found the live `5v_out` load at approximately 1.315 A. Interrupting
that target would have been unnecessarily disruptive, so the Power execution
case used board-internal `vdd_5v` (`off -> on -> off`). This exercises the same
TUI confirmation and Power PUT path while keeping target 5 V uninterrupted.
The TF reader exposed a zero-byte `/dev/sdb` with no media, partition, mount, or
writer, so the reversible `tf_wp` case was safe to execute.

## Local Gates And Canonical Build

| Gate | Result |
| --- | --- |
| Rust CLI/TUI | PASS: formatting, Clippy with warnings denied, no-excuse scan, release build, and 346 tests. |
| Firmware/offline models | PASS: all registered C models plus 87 Python tests. The argparse errors in the log were expected negative fixtures. |
| Web tests | PASS: 286 Node tests, one declared skip, and 183 Vitest tests. |
| Web production build | PASS: React/Vite assets and release decoder WASM generated. |
| Repository contracts | PASS: 61 contract tests and test-registration checks. |
| Canonical firmware | PASS: `make firmware` completed the RP2350 sysbuild and generated MCUboot, application, OTA, and combined-UF2 artifacts. |

The first canonical Web/firmware attempts exposed a host toolchain environment
issue: the nested CMake -> npm process could not find Cargo, and a standalone
WASM retry initially lacked `lld`. The successful canonical build inherited the
verified Nix Cargo/Rust target, Nix `lld`, and Nix `wasm-bindgen`. No source or
upstream checkout was modified to work around the environment.

## HTTP, WebSocket, And Firmware Recovery

All status, Power, Switch, GPIO, ADC, watchdog, config, and task REST endpoints
returned `schema=radxa-linkr-debugger.v1` and `ok=true`. The current release
`doctor` command passed.

The release recorder received exactly three 10 Hz WebSocket telemetry records;
each contained the four expected readings and device timing metadata. A second
one-record WebSocket smoke test passed after CDC BOOTSEL recovery. Recorder
sessions were closed, and the one explicitly created diagnostic session was
deleted by its own session ID.

| Recovery path | Result | Evidence |
| --- | --- | --- |
| HTTP BOOTSEL | PASS | `POST /api/v1/bootloader` returned `ok=true`; USB enumerated as `2e8a:000f` and `/dev/sdc1` on an RPI RP2350 disk. |
| HTTP combined-UF2 recovery | PASS | Only the combined UF2 was copied through the `udisksctl` mount. HTTP, NCM, CDC, and the healthy watchdog returned. |
| CDC BOOTSEL | PASS | CDC shell `bootloader` enumerated the same ROM target on bounded attempt 5. |
| CDC combined-UF2 recovery | PASS | The same UF2 hash was copied again; HTTP, NCM, CDC, watchdog, and WebSocket telemetry returned. |

`picotool` identified the RP2350 but lacked permission to open it. Both flashes
therefore used the unprivileged `udisksctl` mount-and-copy path. The repository
runner's first HTTP BOOTSEL discovery also missed the disk because `lsblk`
returned a space-padded `VENDOR="RPI     "`; direct JSON `lsblk` discovery
confirmed the sole RPI partition before either copy.

After each fresh firmware boot, unsaved SD routing correctly returned to the
firmware default `target`. The test restored the preserved run baseline
`sd=usb-reader` through the ordinary API before continuing. Config and tasks
remained unchanged across both recoveries.

## TUI GPIO Write HIL

The current release binary ran in real 120x32 PTYs. Keyboard actions used a
tmux-backed interactive terminal; mouse actions used real Chromium pointer
events delivered through node-pty and xterm.js.

| Step | Input surface | HTTP readback | Result |
| --- | --- | --- | --- |
| Initial GP13 | Read-only | `input/0` | PASS |
| GP13 primary | Keyboard Enter | `output/1` | PASS; red bold `OUT HIGH`. |
| GP13 primary | Keyboard Enter | `output/0` | PASS; gray `OUT LOW`. |
| GP13 primary | Browser left pointer | `output/1` | PASS |
| GP13 restore | Browser right pointer | `input/0` | PASS |
| Non-GPIO right click | Browser right pointer on Power row | Power unchanged, GP13 `input/0` | PASS; inert. |

The GPIO selection remained confined to the left GP13 cell; its GP19 sibling
stayed independent. Rendering, selection, navigation, mouse hit testing, and
HTTP readback all agreed on the firmware-provided J16 row/column metadata.

Fresh pointer evidence:

- `.omo/evidence/20260811-cmd-ng-tui-write-hil/gpio-left/terminal.png`
- `.omo/evidence/20260811-cmd-ng-tui-write-hil/gpio-right/terminal.png`
- `.omo/evidence/20260811-cmd-ng-tui-write-hil/non-gpio-right/terminal.png`

## TUI Power And Switch Write HIL

Power confirmation ran in a real 80x24 TUI:

| Flow | Result |
| --- | --- |
| Keyboard cancel | PASS: modal opened for `vdd_5v off -> on`; Esc left HTTP at off. |
| Pointer cancel | PASS: a real left pointer opened the Power modal and a real pointer hit its Cancel target; HTTP remained off. |
| Timeout | PASS: the status changed to `Power confirmation timed out` after three seconds; HTTP remained off. |
| Confirmed execution | PASS: one Enter opened and one Enter confirmed `vdd_5v off -> on`; HTTP and green TUI state reported on. |
| Confirmed restoration | PASS: the same sequence restored `vdd_5v on -> off`; all four rails matched baseline. |

Pointer evidence:

- `.omo/evidence/20260811-cmd-ng-tui-write-hil/power-mouse-cancel/terminal.png`

Switch HIL results:

- `tf_wp` changed `writable -> protected -> writable` through the real TUI.
  Both HTTP readbacks matched, and each yellow pending presentation converged
  to cyan ready after the two-second observation window.
- USB opened `target -> pc` confirmation and was cancelled. HTTP remained
  `target`; no USB route write was issued.
- VIN opened `3.3v -> 1.8v` confirmation and was cancelled. HTTP remained
  `3.3v`; no VIN route write was issued.
- SD remained `usb-reader` throughout the interactive control cases.

## Terminal And CDC Fallback

Three independent terminal cleanup paths passed:

| Exit path | Exit code | Cleanup evidence |
| --- | --- | --- |
| `q` | 0 | Before/after `stty -g` byte-identical; returned to primary screen. |
| Ctrl+C | 0 | Before/after `stty -g` byte-identical; `AFTER_TUI_CTRLC` visible on primary screen. |
| Unreachable `127.0.0.1:9` | 1 | Connection error returned; before/after `stty -g` byte-identical; primary-screen marker visible. |

The runs released raw mode, canonical input, echo, cursor ownership, alternate
screen, and mouse capture. No TUI process or tmux session remained at final
audit.

CDC shell readback before NCM isolation returned `vin=3.3v`,
`saved_count=1 pending_count=0`, and `task_count=0`. The host then disconnected
only `eth0`: HTTP became unreachable as expected while the same CDC returned
`task_count=0`. Reconnecting `eth0` restored the same NetworkManager connection
name and `172.29.203.10/24`; HTTP recovered on the first bounded attempt. The
non-NCM default route and DNS hash never changed.

## Final State And Comparison

Final audit found no state requiring an additional rollback write:

- Power: `12v_out=off`, `5v_out=on`, `vdd_5v=off`, `20v_out=off`.
- Switches: `sd=usb-reader`, `usb=target`, `tf_wp=writable`, `vin=3.3v`.
- GPIO: every advertised GPIO `input/0`, including GP13 and ADC-owned GP29.
- Watchdog: healthy and armed, no failing service.
- Persistent config: v1 snapshot present, `saved_count=1`, `pending_count=0`.
- Tasks: zero tasks, empty blob.
- Host: `eth0` connected as `Wired connection 2`; default route still through
  `enp130s0`; DNS file unchanged.
- Handles: no TUI process, tmux session, CDC owner, or RPI BOOTSEL disk remained.

The final normalized status, full sorted config, full sorted task response,
decoded task blob, and DNS SHA256 values were byte-identical to the preserved
baseline values listed above.

## Scope Boundary

This run did not switch VIN to 1.8 V, route USB to PC, interrupt the active
1.315 A `5v_out` target, enable 12 V/20 V, mutate config/tasks, upload OTA, or
inject a watchdog failure. Those operations are not implied by this PASS.
The tested `vdd_5v` transition covers the shared TUI Power confirmation and
write path without risking the active target.
