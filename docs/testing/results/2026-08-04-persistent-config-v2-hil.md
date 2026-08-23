# 2026-08-04 Persistent Configuration v2 HIL

## Verdict

v2 `AUTO_RESTORE_ALL` real-hardware flow: **PASS**.

The dated 2026-07-30 report
(`doc/testing/results/2026-07-30-persistent-config-hil.md`) already recorded the
historical all-six-flow board HIL pass. This 2026-08-04 dated report is the
narrow v2 `AUTO_RESTORE_ALL` evidence: it is the first board run that
exercises the renamed `dangerous-auto-restore` flow, which the 2026-07-30
report explicitly excluded. The 2026-07-30 historical report is not rewritten
and continues to describe its original six-flow scope.

## Scope

This run covers only the v2 `AUTO_RESTORE_ALL` board-HIL gap. The historical
six flows from 2026-07-30 were not rerun on 2026-08-04. Only the
`dangerous-auto-restore` v2 cold-reboot pair plus the supporting Web-UI
verification on the same board were executed.

## Target And Artifacts

- Board: Radxa Linkr Debugger, RP2350, USB NCM HTTP at `http://172.29.203.1`.
- CDC ACM device: identified by `usb-Radxa_Radxa_Linkr_Debugger_*`-if00 path,
  passed as `--serial <identified-cdc-device>` to the runner. CH347 target
  UART is not a persistent-configuration prerequisite.
- Canonical sysbuild PASS: FLASH 783,440 / 847,832 B (92.41 %), RAM
  498,840 / 532,480 B (93.68 %).
- Combined ROM BOOTSEL image:
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`,
  1,618,944 bytes,
  SHA256 `8102c88c29f54013f66b5d0c2298d0f22ce68e6d784d720a90785acc7d3aec35`.
- MCUboot OTA image:
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350-ota.bin`,
  783,480 bytes,
  SHA256 `885395493ee6637d535257d533e3df8ba8b83fa6f9f159722f6f1d73df6537b1`.
- ELF:
  `build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.elf`,
  SHA256 `fb59c6f45b8ee8a9702df80a2111265d94357db097a466f8e67f7591e5f9422a`.
- Only the combined UF2 was flashed, through HTTP ROM BOOTSEL. The app-only
  `zephyr.uf2` was never flashed; flashing it through ROM BOOTSEL would
  brick the board.

## v2 `AUTO_RESTORE_ALL` Run

The `dangerous-auto-restore` flow started at `2026-08-04T07:31:53Z` and
completed at `2026-08-04T07:32:13Z` (20 seconds). It used the non-boot
default `switch/usb=pc`, exactly one unconfirmed Save attempt, exactly one
confirmed Save, and two consecutive CDC cold reboots with no Apply.

| Step | Result | Evidence |
| --- | --- | --- |
| Unconfirmed `switch/usb` Save | PASS | HTTP 409, `error.code=confirmation_required`, `dangerous_items=["switch/usb"]`, no snapshot write. |
| Confirmed `switch/usb` Save | PASS | HTTP 200, `action=save`, `saved_items=["switch/usb"]`, `confirmation_items=["switch/usb"]`, `snapshot.version=2`, `snapshot.present=true`, numeric `pending=0`. |
| First CDC cold reboot | PASS | Readiness, then `GET /api/v1/config`: `switch/usb` row reports `current={route:"pc"}`, `saved={route:"pc"}`, `apply_state="applied"`, `selected=true`, `requires_confirm=true`, numeric `pending=0`, `snapshot.version=2`. |
| Second CDC cold reboot | PASS | Same readiness and `GET /api/v1/config` result as the first reboot. |
| Apply requests during the two reboots | PASS | Zero `POST /api/v1/config/apply` requests issued between the confirmed Save and the final post-second-reboot `GET`. |
| Test snapshot | PASS | The v2 test snapshot used for the cold-reboot pair was cleared at the end of the run via `config clear`, restoring snapshot-absent state with `pending=0`. |

`switch/usb=pc` is the non-boot default because the boot default is
`switch/usb=target`; the v2 confirmation requirement only fires for values
that diverge from the boot default, and `pc` is the canonical non-default
the `dangerous-auto-restore` flow must use.

## Web UI Verification

After the second reboot, the same board served the embedded production Web
UI to a real browser session in dark mode. The browser-side evidence below
was captured against the running board, not against a mock.

- The selected row's computed style carried a `15%` brand fill and a `30%`
  inset ring consistent with the frozen dark-mode selection treatment.
- The confirmed Save `PUT /api/v1/config` and the authoritative
  `GET /api/v1/config` both returned HTTP 200.
- After the confirmed Save and the next authoritative `GET`, all 23 rendered
  row-selection controls were false. Snapshot membership remained
  firmware-authoritative; the Web model's explicit false overrides prevented
  saved rows from visually reselecting themselves.
- 375 px, 768 px, and 1280 px viewport widths all rendered with zero
  horizontal overflow.
- The browser console emitted zero errors.

The dated browser evidence images live under
`.omo/evidence/v2-persistent-final/`. They are attempt-local evidence and
are not committed as tracked paths.

## Final Preservation And Live-Hardware Safety

After the Web UI verification finished, the user's original v2 snapshot
values were recreated on the board so the running configuration matches the
state the user started with before the v2 run. The recreated v2 snapshot
contains exactly:

- `power/12v_out=on`
- `power/5v_out=on`
- `power/vdd_5v=on`
- `switch/usb=pc`

After recreating that snapshot, the live hardware itself was then manually
set safe rather than left as `pc`/`on`:

- All four controllable power outputs are `off`
  (`12v_out`, `5v_out`, `vdd_5v`, `20v_out`).
- `switch/usb=target`, `switch/sd=target`, `switch/tf_wp=writable`.
- `switch/vin=3.3v`.
- Every enumerated GPIO is `input`.
- The preserved v2 snapshot still lives in `linkr/config/snapshot`. A future
  normal reboot will auto-restore the preserved v2 values
  (`12v_out=on`, `5v_out=on`, `vdd_5v=on`, `switch/usb=pc`) because v2
  `AUTO_RESTORE_ALL` replays every saved entry on every normal boot. Live
  state and preserved snapshot state are intentionally different on purpose
  right now: the board was left safe so the operator can repower or reroute
  the target before the next normal boot, while the preserved snapshot keeps
  the user's saved intent intact.

Snapshot absence was rejected as the final state: the user explicitly chose
preserved v2 snapshot plus safe live hardware. The final live hardware
matches the same safe baseline that 2026-07-30 ended on
(all power outputs `off`, USB/SD `target`, TF-WP `writable`, VIN 3.3 V,
GPIO `input`); the preserved snapshot differs on purpose.

## Evidence Files

- Per-attempt screenshots, console logs, and per-request responses are
  archived under `.omo/evidence/v2-persistent-final/`. They are attempt
  evidence and are not tracked in the repository.
- Tracked evidence for this dated report consists of the SHA256 sums and
  sizes listed above plus the board-side observation records captured by
  the `dangerous-auto-restore` runner.

## Relation To The 2026-07-30 Historical Report

The 2026-07-30 report
(`doc/testing/results/2026-07-30-persistent-config-hil.md`) covered six
runner flows (`safe-reboot`, `dangerous-pending`, `capture-busy`,
`ota-preserve`, `bootsel-preserve`, `cdc-fallback`) and explicitly excluded
the v2 `AUTO_RESTORE_ALL` flow because the runner rename to
`dangerous-auto-restore` happened after that report. This 2026-08-04 report
is the dated evidence for that gap. The historical 2026-07-30 report was
not rewritten and continues to describe its original six-flow scope.
