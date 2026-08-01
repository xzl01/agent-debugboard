# 2026-07-30 Persistent Configuration HIL

## Verdict

Todo 16 real-board HIL and the final frontend-artifact revalidation: **PASS**.
The final unshortened `all` run completed all six flows with exit code 0. No
flow was skipped and no business assertion was weakened. There are no blocked
items.

The superseding final-build run started at `2026-07-30T04:14:29Z` and completed
at `2026-07-30T04:16:41Z` (132 seconds).

## Target And Artifacts

- Board: Radxa Linkr Debugger, RP2350, USB NCM HTTP at
  `http://172.29.203.1`.
- CDC: `/dev/serial/by-id/usb-Radxa_Radxa_Linkr_Debugger_E6641C31A362C336-if00`
  (`/dev/ttyACM2` when initially enumerated).
- Combined ROM BOOTSEL image:
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`, 1,516,544
  bytes, SHA256
  `108b25e6d19d9904b03478613106ee9e170786f0ed55698d20ae9efaf3121605`.
- MCUboot OTA image:
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350-ota.bin`, 732,280
  bytes, SHA256
  `4bc4ce25895b8fe9a635d8b01fdd75f80deaa403d42c219f4cfa66383d847fec`.
- The app-only `zephyr.uf2` was never flashed.

## 2026-07-30 Final-Build Revalidation Addendum

The Web dialog fix changed the embedded frontend, so the canonical `shell.nix`
sysbuild was rerun in `build/radxa_linkr_debugger/`. The build used the lock
file's legacy-peer resolution mode (`npm_config_legacy_peer_deps=true`) without
changing `package.json` or `package-lock.json`. The resulting combined UF2 and
OTA identities are the artifacts recorded above and supersede the earlier
hashes in this report.

The new combined UF2 was first flashed through an independently discovered
`/dev/sdc1` ROM BOOTSEL device (vendor `RPI`, label `RP2350`). The unchanged
`config-persistence-hil.sh --execute ... all` runner then repeated safe reboot,
dangerous pending/confirmation, real capture busy, OTA preservation, HTTP
combined-UF2 recovery, and CDC config/BOOTSEL recovery against the final build.
Its 85-line assertion stream completed with exit code 0 and no
`cleanup_failed` record. The real capture received `STOP_RESP`; OTA returned to
`idle` and confirmed; both BOOTSEL paths flashed only the combined UF2.

Independent post-run HTTP and CDC checks again proved snapshot absent/pending
zero, USB/SD target, TF-WP writable, VIN 3.3 V, every power output off, every
enumerated GPIO input, watchdog healthy, and OTA idle/confirmed. Host cleanup
found no owned runner, capture holder, Playwright/Chrome process, port 5556
connection, tmux server, BOOTSEL block device, or RP2350 mount.

## Validation

- PASS: canonical sysbuild completed and generated the combined UF2 and OTA
  bin above.
- PASS: all Web tests, focused dialog RED/GREEN test, `tsc -b`, production Web
  build, and firmware-mode Web build completed for the portaled dialog.
- PASS: `sh -n` and ShellCheck completed without findings for the runner and
  its fixture suite.
- PASS: all runner fixtures completed, including three failed transport probes
  followed by recovery, an invalid reached response that fails without retry,
  and BOOTSEL copy/unmount cases.
- PASS: persistent-configuration documentation checker and all 39 Node tests.
- PASS: complete firmware host C unit suite.
- PASS: initial HTTP BOOTSEL and final runner HTTP/CDC BOOTSEL paths both used
  only the combined UF2 and returned to normal HTTP/CDC operation.

## Final All Run

The final command used `--execute`, the explicit board URL and CDC identity,
the evidence-local cold reboot and real raw-TCP Sigrok capture helpers, the
canonical combined UF2 and OTA bin, both dangerous confirmations, and flow
`all`.

| Flow | Result | Evidence |
| --- | --- | --- |
| safe-reboot | PASS | Safe SD and TF-WP values restored after cold reboot; readiness and full config GET both passed; snapshot cleared. |
| dangerous-pending | PASS | Unconfirmed save/apply returned HTTP 409 `confirmation_required`; confirmed save/apply passed after reboot. |
| capture-busy | PASS | Real Sigrok START acquired capture; save and clear returned HTTP 409 `busy` with capture ownership; STOP response received. |
| ota-preserve | PASS | Active upload was observed; save/clear returned OTA busy; upload returned HTTP 200; test boot recovered after transport retries; confirm passed. |
| bootsel-preserve | PASS | HTTP BOOTSEL enumerated `/dev/sdc1`; combined UF2 was mounted, copied, unmounted, and config survived recovery. |
| cdc-fallback | PASS | CDC show/save/apply/clear and CDC BOOTSEL passed; combined UF2 recovery returned HTTP and CDC. |

## Exact Assertion Matrix

The final runner parsed every reached response before emitting its assertion
line. The matrix below records the exact status and response fields validated by
that parser; it does not imply that every temporary response body was archived.

| Scenario | Exact validated assertion |
| --- | --- |
| Dangerous save, unconfirmed | HTTP 409, `schema=radxa-linkr-debugger.v1`, `ok=false`, `command=config`, `action=save`, `error.code=confirmation_required`, `dangerous_items=["switch/usb"]`. |
| Dangerous apply, unconfirmed | HTTP 409, `schema=radxa-linkr-debugger.v1`, `ok=false`, `command=config`, `action=apply`, `error.code=confirmation_required`, `dangerous_items=["switch/usb"]`. |
| Dangerous confirmed result | Confirmed save returned HTTP 200 with `action=save`; after reboot `switch/usb` was `pending`; confirmed apply returned HTTP 200 with `action=apply`, followed by `switch/usb` `apply_state=applied`. |
| Capture owns arbiter | Both config save and present-snapshot clear returned HTTP 409, `error.code=busy`, `activity=capture`; the bounded GET remained HTTP 200 and retained the prepared snapshot. |
| OTA owns flash | Both config save and present-snapshot clear returned HTTP 409, `error.code=busy`, `activity=ota`; GET remained HTTP 200 and confirm-false apply returned HTTP 200 with `noop=true`. |
| OTA lifecycle | Upload completed with HTTP 200 and `state=verified`; test returned HTTP 202 and `state=rebooting`; post-reboot config readiness and full GET passed; confirm returned HTTP 200 and `state=idle`. |
| Snapshot survival | Safe reboot restored `switch/sd=usb-reader` and `switch/tf_wp=protected` with snapshot present and both rows applied. OTA test boot and HTTP combined-UF2 recovery each retained the selected safe snapshot row as applied. |
| CDC fallback | `config show`, `config save power/12v_out`, `config apply --confirm`, `config clear`, and `bootloader` each matched the required primary response; clear reported `hardware_changed=false`. |
| Final config cleanup | Full final config response had `snapshot.present=false`, `snapshot.version=null`, numeric `pending=0`, `switch/usb=target`, `switch/sd=target`, and `switch/tf_wp=writable`. |
| Final hardware and OTA cleanup | Full final status response had `switch/vin=3.3v`, every controllable power output `off`, every enumerated GPIO `input`, and watchdog healthy/armed; full final OTA response had `state=idle` and `current_image_confirmed=true`. |

The OTA test boot took approximately 23 seconds from the fixed five-second
wait start to a successful readiness response. Transport failures were retried;
the subsequent full business response was still validated independently.

## Interrupted Attempts

Earlier logs remain under
`.omo/evidence/task-16-2026-07-30T061047+0800/raw/`. The parent harness timeout
was not treated as a product failure. A later resumed attempt exposed a real
runner sequencing bug: readiness polling ran after entering ROM BOOTSEL but
before copying the combined UF2. The runner was corrected so BOOTSEL entry only
waits for enumeration; HTTP readiness begins after combined-UF2 flashing. The
failed attempt was recovered with the same combined UF2 and its residual
snapshot was cleared before the final run.

## Independent Final State

Independent requests after the runner completed proved:

- HTTP config: snapshot absent and pending count 0.
- OTA: `idle`, current image confirmed.
- Power: `12v_out`, `5v_out`, `vdd_5v`, and `20v_out` all off.
- Switches: SD target, USB target, TF-WP writable, VIN 3.3 V.
- GPIO: every firmware-enumerated safe GPIO is input.
- Watchdog: supported, automatic, armed, and healthy.
- CDC: `config available=true reason=absent saved_count=0 pending_count=0`.
- Host: no runner, uploader, capture holder, flash process, port 5556 listener,
  RPI-RP2 block device, or RPI-RP2 mount remained.

## Evidence Files

- `2026-07-30-persistent-config-hil.raw.jsonl`: final runner assertion stream
  plus the three independent full final HTTP responses (config, status, and
  OTA). It is not an archive of every request body.
- `2026-07-30-persistent-config-hil.serial.log`: complete final runner CDC
  transcript plus independent final CDC query.
- `2026-07-30-persistent-config-hil.final-config.json`: independent config
  cleanup state.
- `2026-07-30-persistent-config-hil.final-status.json`: independent hardware
  state.
- `2026-07-30-persistent-config-hil.final-ota.json`: independent OTA state.
- `2026-07-30-persistent-config-hil.final-cdc.out`: independent CDC summary.
- `2026-07-30-persistent-config-hil.SHA256SUMS`: hashes for this report and all
  archived evidence files.

The final successful runner kept per-request bodies in its bounded temporary
directory and removed that directory during cleanup. Response bodies explicitly
captured during interrupted and diagnostic attempts remain under
`.omo/evidence/task-16-2026-07-30T061047+0800/raw/`; they are attempt evidence,
not substitutes for final-run bodies. In particular, stale malformed pre-fix
bodies were neither copied into the dated artifacts nor used to populate the
matrix above.
