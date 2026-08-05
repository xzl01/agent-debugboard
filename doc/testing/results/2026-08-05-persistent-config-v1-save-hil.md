# 2026-08-05 Persistent Configuration v1 Save-And-Apply HIL

## Verdict

v1 save-and-apply real-hardware flow: **PASS**.

This dated report is the board evidence for the v1-only persistent
configuration contract: one wire version, Save as the sole persist-and-apply
operation, full boot restore of every structurally valid v1 snapshot, no
separate Apply operation anywhere. The 2026-07-30 and 2026-08-04 historical
reports are not rewritten; they continue to describe the superseded v1/v2
dual-format contract that this change removed.

## Scope

This run covers the complete runner flow set on the new v1-only firmware plus
the direct API/CLI/Web verifications that the runner does not cover:

- `config-persistence-hil.sh --execute ... all` six flows: `safe-reboot`,
  `dangerous-auto-restore`, `capture-busy`, `ota-preserve`,
  `bootsel-preserve`, `cdc-fallback`, plus the enumerated final cleanup.
- Direct checks: removed HTTP Apply route, removed CLI Apply subcommand,
  dangerous GPIO output persistence across a cold reboot, and the board-hosted
  Web UI with the Apply control removed.
- Partial-failure and retry semantics are covered by the host-model suites
  (see "Partial-Failure Coverage"), not by a board trigger; see that section
  for why a board trigger is not practical on this hardware.

## Target And Artifacts

- Board: Radxa Linkr Debugger, RP2350, USB NCM HTTP at `http://172.29.203.1`.
- CDC ACM device: identified by `usb-Radxa_Radxa_Linkr_Debugger_*`-if00 path,
  passed as `--serial <identified-cdc-device>` to the runner. CH347 target
  UART is not a persistent-configuration prerequisite.
- Canonical sysbuild PASS: FLASH 781,800 / 847,832 B (92.21 %), RAM
  494,672 B (92.90 %).
- Combined ROM BOOTSEL image:
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`,
  1,615,872 bytes,
  SHA256 `b59fa4c18e0235485d6421675553a61b80541526b15e768f8ae3b3e1f63727db`.
- MCUboot OTA image:
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350-ota.bin`,
  SHA256 `3772c9600f97d170e800d62aaa0694b8990e711d9507b4b83c5a9183a03efe66`.
- ELF:
  `build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.elf`,
  SHA256 `51d87430ba5f89012c06da50d0b6b2cd6aa57f4f68ee5c6f5dd6df3fab9197d6`.
- Only the combined UF2 was flashed, through HTTP and CDC ROM BOOTSEL. The
  app-only `zephyr.uf2` was never flashed; flashing it through ROM BOOTSEL
  would brick the board.

## Pre-Existing Snapshot Rejection (v2 Blob)

Before flashing, the board held a v2 snapshot written by the previous
firmware (three power outputs on plus `switch/usb=pc`, fully replayed under
the old v2 semantics). After the first boot of the new v1-only firmware:

- `GET /api/v1/config` reported `backend.reason=unsupported_version`,
  `snapshot.present=false`, `snapshot.version=null`, numeric `pending=0`.
- The stored v2 blob was not replayed: live hardware stayed at Device Tree
  defaults (all four power outputs off, `switch/usb=target`).
- The stored v2 blob was not erased or migrated; the first explicit Save
  afterwards overwrote it with a fresh v1 header.

This is the board-level proof that a version byte other than 1 is never
replayed, migrated, or auto-cleared.

## `dangerous-auto-restore` Run (v1)

The flow used the non-boot default `switch/usb=pc`, exactly one unconfirmed
Save attempt, exactly one confirmed Save, and two consecutive CDC cold
reboots. There is no Apply operation in the API; none was issued or needed.

| Step | Result | Evidence |
| --- | --- | --- |
| Unconfirmed `switch/usb` Save | PASS | HTTP 409, `error.code=confirmation_required`, `dangerous_items=["switch/usb"]`, no snapshot write. |
| Confirmed `switch/usb` Save | PASS | HTTP 200, `action=save`, `saved_items=["switch/usb"]`, `confirmation_items=["switch/usb"]`, `applied_items=["switch/usb"]`, `snapshot.version=1`, `snapshot.present=true`, numeric `pending=0`; live `switch/usb` route became `pc` immediately. |
| First CDC cold reboot | PASS | Readiness, then `GET /api/v1/config`: `switch/usb` row reports `current={route:"pc"}`, `saved={route:"pc"}`, `apply_state="applied"`, `selected=true`, `requires_confirm=true`, numeric `pending=0`, `snapshot.version=1`. |
| Second CDC cold reboot | PASS | Same readiness and `GET /api/v1/config` result as the first reboot. |
| Test snapshot | PASS | Cleared at the end of the run via `config clear`, restoring snapshot-absent state with `pending=0`. |

## Six-Flow Runner Result

`config-persistence-hil.sh --execute --url http://172.29.203.1 --serial
<identified-cdc-device> --reboot-command <cdc-reboot> --capture-start
<sigrok-start> --capture-stop <sigrok-stop> --confirm-dangerous-save
--combined-uf2 build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2
--ota-image build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350-ota.bin
all` completed with exit status 0.

| Flow | Result | Evidence |
| --- | --- | --- |
| `safe-reboot` | PASS | Safe `switch/sd=usb-reader` + `switch/tf_wp=protected` saved (v1), two reboots restored both, cleanup returned `target`/`writable`. |
| `dangerous-auto-restore` | PASS | See the table above. |
| `capture-busy` | PASS | Real sigrok TCP START (100 kHz continuous, GP10) held the capture arbiter; save and clear both returned HTTP 409 `busy` with `activity=capture`; STOP released ownership. |
| `ota-preserve` | PASS | During an active paced (`--limit-rate 64K`) OTA upload, save and clear returned HTTP 409 `busy` with `activity=ota`; the prepared v1 snapshot survived the OTA test boot and manual confirm. |
| `bootsel-preserve` | PASS | HTTP BOOTSEL entry, RPI-RP2 partition discovery, canonical combined-UF2 flash; the v1 snapshot survived ROM BOOTSEL recovery. |
| `cdc-fallback` | PASS | CDC `config show`, `config save <firmware-item-id>`, `config clear`, and CDC `bootloader` into ROM BOOTSEL plus combined-UF2 recovery. |
| Final enumerated cleanup | PASS | All controllable power outputs off, output GPIOs back to input, GET readback verified both, snapshot absent. |

The capture holder is a small sigrok binary client: HELLO/CAPS handshake,
CONFIG (FAST8, GP10 mask, 100 kHz, pre=0, post=0 continuous), START_REQ,
then CAPS_REQ keepalive frames every 1.5 s to defeat the 2 s streaming idle
timeout. 1 MHz continuous overran the arena in this setup; 100 kHz was
stable for the full busy window.

## Removed Apply Operation

| Check | Result | Evidence |
| --- | --- | --- |
| HTTP Apply route | PASS | `POST /api/v1/config/apply` with `{"confirm":true}` returns HTTP 405 with an empty body; the route is not registered. |
| CLI Apply subcommand | PASS | `radxa-linkr-debuggerctl config apply --confirm` exits with status 2 and a usage error before any I/O. |
| CDC Apply subcommand | PASS | The CDC `config` subcommand set is `show`, `save [--confirm] <firmware-item-id>...`, `clear`; the `cdc-fallback` flow exercises exactly those verbs. |
| Web Apply control | PASS | Board-hosted page has no Apply control; the Saved Config card offers only save-selected, refresh, and clear (see Web section). |

## Dangerous GPIO Output Persistence

`gpio/GP13` was set to `output=1`, then saved with confirmation:

- Save response: `saved_items=["gpio/GP13"]`,
  `confirmation_items=["gpio/GP13"]`, `applied_items=["gpio/GP13"]`,
  `snapshot.version=1`, numeric `pending=0`.
- After one CDC cold reboot: `GET /api/v1/gpio/GP13` reports
  `direction=output, value=1`; `GET /api/v1/config` reports the GP13 row
  `current` and `saved` both `output/1`, `apply_state=applied`,
  `snapshot.version=1`, numeric `pending=0`.

GP13 was returned to `input` and the snapshot cleared afterwards. The same
save-and-cold-reboot restore was re-verified on the final flashed build.

## GPIO Input Level Restore

Legacy v1 blobs can carry GPIO entries whose value byte is
`LINKR_DEBUGGER_CONFIG_GPIO_LEVEL` without the output bit (an input row
captured while its pin read high). The codec accepts that byte as
structurally valid, and the contract requires every structurally valid v1
snapshot to restore fully. The replay adapter previously rejected that byte
with `-EINVAL`, which would have stopped the restore; it now configures the
pin as an input regardless of the captured level, and the HTTP status
encoder reports the saved row as `{"direction":"input","value":1}`. The
current Save path never produces this byte (the capture projects GPIO input
rows to value zero), so the reachable trigger on this hardware is a legacy
blob; the codec golden vector, the host-model encoder case, and the
production adapter change are the coverage.

## Partial-Failure Coverage

Partial-failure and retry semantics are covered by the host-model suites, not
by a board trigger, because no board-side failing setter can be armed through
the public API on this hardware: `gpio/GP29` is ADC3-owned and input-only, so
a live `output` value for it can never be captured into a snapshot, and the
NVS partition is not writable through any API. The offline proof points are:

- `test_linkr_debugger_config_replay.c`: first-failure stop at every one of
  the 23 replay positions, applied/pending/failed reporting, and a later
  full success clearing the failure.
- `test_linkr_debugger_config_service.c`
  `test_save_partial_failure_reports_pending_and_retry`: a failed Save still
  persists the snapshot, reports the applied subset, the failed row, and the
  pending rows, and a repeated confirmed Save retries and clears the state.
- `test_linkr_debugger_config_http.c` / `test_linkr_debugger_config_shell.c`:
  HTTP 500 `apply_failed` carries `applied_items`, `failed_item`, and
  `pending_items`; CDC prints the matching detail lines under the `save`
  verb.

## Web UI Verification (Playwright, Board-Hosted Page)

Against `http://172.29.203.1/` served by this firmware:

- No Apply control exists anywhere on the page (`hasApplyButton=false`).
- The Saved Config card renders all 23 firmware-enumerated rows and offers
  exactly three operations: save selected, refresh, clear saved.
- UI save flow: selecting `switch/sd` and clicking save produced a v1
  snapshot (`snapshot.version=1`, `pending=0`), the row showed the applied
  state, the selection cleared, and the success notice appeared.
- UI clear flow: the clear confirmation dialog cleared the snapshot
  (`reason=absent`, `snapshot.present=false`) and closed.
- Screenshot: `.omo/evidence/v1-save-hil/saved-config-1280.png`.
- A page-level horizontal overflow exists at 375/768 px viewports and traces
  to the pre-existing sticky header toolbar (`max-w-[1600px]` flex row,
  about 770 px). The web diff in this change touches only
  persistent-configuration components, so this is recorded as a pre-existing
  page-chrome observation, not a regression of this change.

## Intermittent Watchdog Event (Investigated)

One earlier `all` attempt ended with the board entering ROM BOOTSEL after a
save request exceeded the runner's 5 s transport timeout during the
`safe-reboot` flow. The identical save sequence was then reproduced three
times without failure, the two subsequent complete `all` runs were clean,
and no firmware fault was captured. It is recorded here as an investigated
intermittent event rather than a reproducible defect.

## Local Validation Boundary

Local validation is not real-hardware HIL. The firmware host-model suites
(`apps/radxa_linkr_debugger/tests/run_unit_tests.sh`), the Rust CLI/TUI
suites (`cargo test --all-targets`), the Web suites (`npm test` plus
production build), the offline shell fixture suite
(`config-persistence-hil.test.sh`), and the frozen documentation checker all
pass locally and are distinct from this board run. This dated board evidence
stands as the real-hardware validation of the v1 save-and-apply contract.

## Related Reports

- Historical six-flow report:
  [2026-07-30-persistent-config-hil.md](2026-07-30-persistent-config-hil.md)
- Superseded v2 report:
  [2026-08-04-persistent-config-v2-hil.md](2026-08-04-persistent-config-v2-hil.md)
