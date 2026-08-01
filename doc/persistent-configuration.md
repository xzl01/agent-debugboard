# Persistent Configuration

This document is the canonical reference for the persistent-configuration
feature on `radxa-linkr-debugger`. The persistent-configuration service lets
the firmware remember selected power, switch, and safe-GPIO values across
boots, restore them on startup, and re-apply them on demand through the HTTP,
WebSocket, CDC ACM, and embedded Web surfaces.

The default device URL is `http://172.29.203.1`. The feature is owned by the
firmware: the host CLI/TUI, Web UI, and CDC ACM shell only render and forward
state. They do not derive confirmation policy, hold profiles, or keep their own
copy of saved values.

## Scope And Non-Goals

The persistent-configuration feature is in scope:

- One explicit snapshot of selected power, switch, and safe-GPIO values
  stored on the board under `linkr/config/snapshot`.
- A four-endpoint HTTP API on the same composite USB NCM interface that
  already carries `/api/v1/status` and `/api/v1/power`.
- A compact status summary carried inside the existing WebSocket
  `snapshot/status` message.
- A Rust CLI subcommand (`config show|save|apply|clear`) on the released
  `radxa-linkr-debuggerctl` and on the in-tree `cmd-ng/` source build.
- A TUI control surface that mirrors the Rust CLI.
- An embedded Web UI panel that mirrors the Rust CLI.
- A CDC ACM shell fallback path with the same `config show|save|apply|clear`
  verbs.

The persistent-configuration feature is intentionally out of scope:

- No named profiles are stored, listed, switched between, or exposed by the
  HTTP API, CLI, TUI, Web UI, or CDC shell.
- No encryption or secure storage is provided. The snapshot is stored in
  clear text on the board's existing NVS `storage_partition`; no
  confidentiality, integrity proof beyond the NVS CRC, or tamper resistance is
  claimed.
- No authentication, authorization, signature, or anti-rollback is provided
  for the persistent-configuration service or any of its transports.
- No automatic rollback of saved values is performed. Failure of one entry
  during apply stops the apply; earlier entries are not undone.
- No host-side persistence is performed. The Rust CLI, TUI, Web UI, and CDC
  shell do not remember the saved snapshot across their own restarts.

## User Model

Ordinary setters are volatile. A `power set 12v_out on`, `switch route sd
usb-reader`, or `gpio set GP13 1` command changes the live hardware state for
the current boot only. Closing and reopening the connection, or rebooting the
board, does not bring those values back.

A persistent value is created by an explicit `config save` request. The
firmware snapshots the live values of the items listed in the request into
the on-board storage under `linkr/config/snapshot`. The save is atomic: every
listed item is captured and written together or the request fails and the
existing snapshot is left untouched.

A saved snapshot is restored in two phases. At boot, the firmware loads the
snapshot and re-applies the values that do not require confirmation. Values
that do require confirmation stay pending until the user runs `config apply`.
After boot, `config save` always writes a fresh snapshot; it does not apply
the saved values to live hardware. `config clear` deletes the snapshot; it
does not alter live hardware.

The confirmation rule is firmware-owned. Firmware confirmation is required
whenever the catalog marks an entry as `requires_confirm`. The CLI, TUI,
Web UI, and CDC shell do not decide which items are dangerous. They only
forward the firmware's `requires_confirm` flag and let the user pass
`--confirm` when dangerous items are present.

The user-visible verbs are:

- `config show`: read the catalog, the snapshot status, and the per-item
  state.
- `config save [--confirm] <firmware-item-id>...`: capture the live values
  of the listed items into the on-board snapshot. `--confirm` is required
  when any listed item is dangerous.
- `config apply --confirm`: apply the saved snapshot to live hardware. The
  `--confirm` is mandatory.
- `config clear`: delete the saved snapshot. Live hardware is unchanged.

## Storage Model

The snapshot reuses the existing on-board NVS `storage_partition`. This feature
does not introduce a `storage_partition` Device Tree node, partition overlay,
or `chosen` override. Board Kconfig enables `CONFIG_SETTINGS`,
`CONFIG_SETTINGS_NVS`, `CONFIG_NVS`, and `CONFIG_NVS_DATA_CRC`.
`CONFIG_SETTINGS_NVS_SECTOR_COUNT=8` reserves eight NVS sectors for Settings
under that existing partition.

The Zephyr Settings+NVS backend sits on top of `storage_partition`. The
persistent-configuration service calls `settings_subsys_init()` at boot,
loads the value at the Settings key `linkr/config/snapshot`, and stores a
single binary snapshot blob under that key. There is no other key, no
sharded record, and no per-item entry.

The snapshot is a single bounded binary record. The header carries the magic
`LRCF`, the version byte `1`, the entry size, the entry count, and the total
encoded size. Each entry is four bytes: domain, item id, value, and a padding
byte. The maximum encoded size is 104 bytes for a 12-byte header and 23
entries. The catalog is exactly 23 items: 4 power outputs, 4 switches, and
15 safe GPIOs (`gpio/GP7` through `gpio/GP20` plus `gpio/GP29`).

The snapshot is version-bounded. The codec rejects:

- A missing or wrong magic: returned as `invalid_snapshot` with the
  `invalid_snapshot` reason.
- A version that is not `1`: returned as `unsupported_version` with the
  `unsupported_version` reason.
- An entry count of zero: returned as `empty_selection` to the policy layer
  and surfaced as a save validation error.
- An entry whose domain or item id is not in the firmware catalog, or whose
  value is out of the per-domain range: returned as `invalid_snapshot`.

CRC is enforced by `CONFIG_NVS_DATA_CRC` at the NVS layer; the persistent
configuration service does not add a second CRC. Corrupt or unsupported storage
falls back safely without formatting; missing, corrupt, and unsupported
snapshots leave the boot defaults in place.
An encoded snapshot `entry_count=0` is rejected as `empty_selection`. Missing
key means absent and defaults; corrupt or unsupported storage surfaces its
reason and defaults. The firmware does not auto-format, erase, or delete the
snapshot, and storage read and write failures remain errors.
`apply` is unavailable until its status reason permits it. `save` does not gate
on `service_status.reason`: after ownership acquisition, control capture, and
policy checks, it calls the backend store. A successful backend write can
replace a corrupt or unsupported snapshot and sets the service reason to
`ready`. `clear` is the explicit deletion/recovery operation: when the backend
is available it deletes the single key and re-establishes `absent`, but it is
not the only possible replacement path. A backend-unavailable or
storage-read/write failure can still make the relevant operation fail. The
firmware does not auto-format, auto-erase, or auto-delete the snapshot.

## Firmware-Owned Catalog And Risk

The catalog is owned by the firmware. The 23 items are exactly:

- Power: `power/12v_out`, `power/5v_out`, `power/vdd_5v`, `power/20v_out`.
- Switch: `switch/sd` (target | usb-reader), `switch/usb` (pc | target),
  `switch/tf_wp` (writable | protected), `switch/vin` (1.8v | 3.3v).
- GPIO: `gpio/GP7` through `gpio/GP20` and `gpio/GP29`.

The firmware classifies each saved entry as safe or dangerous through
`linkr_debugger_config_classify_entry`:

- Dangerous: any power `on` value; `switch/usb` (both routes); `switch/vin`
  with value `1.8v`; any GPIO output value (the GPIO entry's value byte has
  the output bit set).
- Safe: power `off`; `switch/sd`; `switch/tf_wp`; `switch/vin` with value
  `3.3v`; any GPIO input value.

The `requires_confirm` flag on each catalog row is computed by the firmware
from the catalog row's current value or saved value. The host never overrides
the flag.

## Boot Restore And Apply

The boot flow has four phases: defaults first, then snapshot load, then
safe auto-restore, then dangerous pending.

1. The firmware boots with the Device Tree defaults. Power outputs are
   off, `switch sd` is `target`, `switch usb` is `target`, `switch tf_wp`
   is `writable`, `switch vin` is `3.3v`, and the allowlisted GPIOs are in
   input mode.
2. The persistent-configuration service loads the snapshot from
   `linkr/config/snapshot`. A corrupt, unsupported, missing, or
   backend-unavailable snapshot does not block startup. The boot path
   reports the reason and continues with the defaults.
3. If the snapshot is valid, the service re-applies every entry whose
   `requires_confirm` flag is false. These are the safe values, and the
   safe values auto-restore after defaults on every boot.
4. Entries whose `requires_confirm` flag is true remain pending after
   boot, and the dangerous values remain pending until the user runs
   `config apply`. `pending` is non-zero, `apply_state` is `pending`, and
   `current` still reflects the Device Tree default.

An all-dangerous snapshot may have a zero-entry boot-safe subset. That valid
filtered boot restore is a successful no-op, leaves dangerous rows pending, and
is not corruption.

Boot restore and `config apply` share the same ten-stage order, enforced by
`linkr_debugger_config_apply_order_snapshot`. Boot-safe restore skips dangerous
entries, including GPIO outputs, while preserving this common order for the
entries it does apply:

1. Safe GPIO inputs (GPIOs in input mode).
2. `switch sd`.
3. `switch tf_wp`.
4. `switch usb`.
5. `power vdd_5v`.
6. `power 12v_out`.
7. `power 5v_out`.
8. `power 20v_out`.
9. `switch vin`.
10. Saved GPIO outputs (GPIOs in output mode).

The service mutex is held for the entire apply sequence. It acquires the
capture owner and then the flash owner once before calling the full
`linkr_debugger_config_apply_execute` sequence, then releases flash and capture
once after that call returns. Owners are not reacquired per entry.

GET never acquires owners. The HTTP facade returns absent or non-ready apply
before service owners. A ready snapshot with `pending_count==0` and
`failed_count==0` returns HTTP 200 `noop:true` from the facade before service
owner acquisition regardless of confirm. For a retryable pending or failed
snapshot, the service checks dangerous confirmation before owner acquisition;
an unconfirmed dangerous retry returns HTTP 409 owner-free. A safe retry or
confirmed dangerous retry acquires capture, then flash, then runs the full
apply.

During the apply, the service uses stop-first-failure. The first setter that
returns a non-zero errno stops the apply, sets `apply_state` to `failed`
for the failing entry, and leaves every earlier entry applied. There is no
rollback: the firmware does not undo earlier entries, does not retry, and
does not fall back to a previous snapshot. The host sees `noop` as false,
`applied_items` listing every entry that succeeded, `failed_item` pointing
to the entry that stopped the apply, and `pending_items` listing every
remaining entry.

The boot apply skips any entry whose `requires_confirm` flag is true. The
host can run `config apply --confirm` after boot to push the pending entries
through the same ten-stage sequence with stop-first-failure.
Failed-only snapshots remain retryable even when numeric `pending` excludes
failed rows. A confirmed full apply confirms every saved dangerous row,
including already-applied dangerous siblings.

## HTTP API

The HTTP API uses the same JSON envelope as the rest of the board:
`schema`, `ok`, `command`, and either command-specific fields or
`error.code` plus `error.message`. The command is always `config`. Every
response includes `Cache-Control: no-store`. The default device URL is
`http://172.29.203.1`.

The four endpoints are:

- `GET /api/v1/config` returns the catalog, the snapshot status, and the
  per-item state. The response carries `action` set to `get`, `backend` with
  `available` and `reason`, `snapshot` with `present` and `version`,
  `pending` as the count of items that still need to be applied, and `items`
  with one row per catalog item. Each row carries `id`, `kind`, `current`,
  `saved`, `selected`, `requires_confirm`, and `apply_state`. The `items`
  array is required for a successful `get` response.
- `PUT /api/v1/config` captures the live values of the listed items into
  the snapshot. The body is a JSON object with `items` (an array of
  firmware item ids) and `confirm` (a boolean). The response carries
  `action` set to `save`, `saved_items`, `confirmation_items`,
  `snapshot.present` set to `true`, `snapshot.version` set to `1`, and
  `pending` set to `0`. The request body is capped at 1024 bytes; an
  oversize body returns `body_too_large` with HTTP 413.
- `POST /api/v1/config/apply` runs the saved snapshot through the ten-stage
  apply. The body is `{"confirm": true}`. The response carries `action`
  set to `apply`, `noop` (true only when `pending_count==0` and
  `failed_count==0`),
  `applied_items`, `failed_item`, and `pending_items`.
- `DELETE /api/v1/config` deletes the snapshot. The body is empty. The
  response carries `action` set to `clear`, `noop`, `snapshot.present` set
  to `false`, and `pending` set to `0`.

The success and failure fields on each envelope are:

- Success top-level: `schema`, `ok`, `command`, `action`.
- Success `get`: `backend`, `snapshot`, `pending`, `items`.
- Success `save`: `saved_items`, `confirmation_items`, `snapshot`,
  `pending`.
- Success `apply`: `noop`, `applied_items`, `failed_item`, `pending_items`.
- Success `clear`: `noop`, `snapshot`, `pending`.
- Failure top-level: `schema`, `ok`, `command`, `action`, `error.code`,
  `error.message`.

The machine error codes defined by the persistent-configuration service are:

`invalid_json`, `empty_selection`, `unknown_item`, `duplicate_item`,
`confirmation_required`, `item_unavailable`, `no_snapshot`, `busy`,
`body_too_large`, `backend_unavailable`, `invalid_snapshot`,
`unsupported_version`, `control_capture_failed`, `storage_error`,
`storage_write_failed`, `apply_failed`, and `internal_error`. The `busy`
error carries an `activity` field whose value is `capture` or `ota`.
`confirmation_required` carries a `dangerous_items` array.

The HTTP status mapping for each error code is:

- 400 Bad Request: `invalid_json`, `empty_selection`, `unknown_item`,
  `duplicate_item`.
- 409 Conflict: `confirmation_required`, `item_unavailable`, `busy`,
  `no_snapshot`.
- 413 Payload Too Large: `body_too_large`.
- 500 Internal Server Error: `backend_unavailable`, `invalid_snapshot`,
  `unsupported_version`, `storage_error` (save path), `storage_write_failed`,
  `control_capture_failed`, `apply_failed`, `internal_error`.

### Examples

<!-- persistent-config-example: curl-config-get -->
```sh
curl -fsS http://172.29.203.1/api/v1/config
```

<!-- persistent-config-example: curl-config-save-safe -->
```sh
curl -fsS -X PUT -H 'Content-Type: application/json' --data '{"items":["switch/sd"],"confirm":false}' http://172.29.203.1/api/v1/config
```

<!-- persistent-config-example: curl-config-save-dangerous -->
```sh
curl -fsS -X PUT -H 'Content-Type: application/json' --data '{"items":["switch/usb"],"confirm":true}' http://172.29.203.1/api/v1/config
```

<!-- persistent-config-example: curl-config-apply-dangerous -->
```sh
curl -fsS -X POST -H 'Content-Type: application/json' --data '{"confirm":true}' http://172.29.203.1/api/v1/config/apply
```

<!-- persistent-config-example: curl-config-clear -->
```sh
curl -fsS -X DELETE http://172.29.203.1/api/v1/config
```

## Status And WebSocket Summary

The persistent-configuration status is reported through two compact surfaces.

`GET /api/v1/status` does not return the catalog. It carries the same
`board_monitoring` shape as before. WebSocket `snapshot/status` messages
carry the same `board_monitoring` object. The status summary fragment is the
only persistent-configuration field on these surfaces and contains exactly
four fields: `available`, `reason`, `saved_count`, and `pending_count`. The
summary is appended through `linkr_debugger_config_summary_append`; firmware
may omit the config summary fragment on encode failure. The exact wire `reason` values are:
`ready`, `absent`, `storage_error`, `invalid_snapshot`, and
`unsupported_version`. The compact summary maps service
`backend_unavailable` to wire `storage_error`; it does not emit
`backend_unavailable` as a summary reason.

Web preserves the last valid summary when the next WS value is absent or
malformed; Rust TUI clears support, focus, and confirmation when
`snapshot.config` is `None`.

The catalog, `snapshot.version`, the `items` array, the `dangerous_items`
list, and the per-row `apply_state` only appear on the four dedicated HTTP
endpoints above. They are not carried on the status WebSocket.

## Rust CLI

The Rust host CLI lives under `cmd-ng/` and exposes the four verbs as a
single `config` subcommand. The CLI is a thin renderer: it does not decide
which items are dangerous, does not retry, and does not keep its own copy
of the saved snapshot.

The exact grammar is:

- `radxa-linkr-debuggerctl config show`
- `radxa-linkr-debuggerctl config save [--confirm] <firmware-item-id>...`
- `radxa-linkr-debuggerctl config apply --confirm`
- `radxa-linkr-debuggerctl config clear`

A `config save` with no items is a usage error and does not reach the
HTTP API. A `config apply` without `--confirm` is a usage error and does not
reach the HTTP API. `--confirm` may appear before or after the items on
`config save`.

The Rust parser preserves the user-supplied item order, sends the request
verbatim, and lets the firmware validate the snapshot. The `--json` flag
passes the raw firmware JSON through unchanged; without `--json`, the
renderer prints a human-readable summary based on the envelope.

The default base URL is `http://172.29.203.1`. The CLI sends
`Content-Type: application/json` and `Accept: application/json`. Before
rendering it strictly binds the expected `action`, schema, command, and the
action-specific required fields. It also rejects a response when its HTTP
status does not agree with the `ok` envelope: 2xx requires `ok: true`, and a
non-2xx response requires `ok: false`. Failure envelopes require
`error.code` and `error.message`; `confirmation_required` requires
`dangerous_items`, `busy` requires `activity` `capture` or `ota`, and
`apply_failed` requires the partial-apply fields. The CLI therefore does not
ignore HTTP status or invent a fallback rendering.

### Examples

<!-- persistent-config-example: cli-config-show -->
```sh
radxa-linkr-debuggerctl config show
```

<!-- persistent-config-example: cli-config-save-safe -->
```sh
radxa-linkr-debuggerctl config save switch/sd
```

<!-- persistent-config-example: cli-config-save-dangerous -->
```sh
radxa-linkr-debuggerctl config save --confirm switch/usb
```

<!-- persistent-config-example: cli-config-apply -->
```sh
radxa-linkr-debuggerctl config apply --confirm
```

<!-- persistent-config-example: cli-config-clear -->
```sh
radxa-linkr-debuggerctl config clear
```

## Interactive TUI

The TUI control surface in `cmd-ng/src/tui/` mirrors the four verbs. It keeps
only the current firmware-derived view and runs `ConfigWorker` as a bounded
background worker: one active refresh/save/apply/clear job at a time.

The TUI worker (`ConfigWorker` in `cmd-ng/src/tui/config_io.rs`) issues one
mutation request and one authoritative refresh in the same job:

- A `Save`, `Apply`, or `Clear` job sends the mutation first and then issues
  one authoritative `GET /api/v1/config`, including after a mutation failure.
- The TUI never silently retries or injects confirmation. A
  `confirmation_required` response opens a separate saved-config confirmation
  modal from its firmware `dangerous_items`; `Enter` sends the next request
  with `confirm: true`, while `Esc` cancels it.
- `r` refreshes HTTP status and requests an authoritative config GET.
- `c` focuses Saved Config when it is supported and loaded. While focused,
  `c` or `Esc` blurs it; Up/Down or `j`/`k` moves the item cursor, and
  Enter/Space toggles the current selection. `s`, `a`, and `x` request save,
  apply, and clear. `q` and Ctrl-C remain global quit keys even while focus or
  a confirmation modal is active.
- A `busy` result retains its firmware activity and a storage failure remains
  an error; neither path falls back to client defaults or auto-issues another
  mutation.

The TUI does not own confirmation policy. The confirmation flag is read
from the firmware's `requires_confirm` field on each catalog row and from
the `dangerous_items` field on save errors.
When a saved row is `pending` or `failed`, the TUI offers a retry; an apply
confirmation lists every saved dangerous row, including an already-applied
dangerous sibling.

## Embedded Web UI

The embedded Web UI talks to the same four HTTP endpoints and the same
WebSocket summary. Its source is under repository `web/`. The Web UI is
HTTP-driven and does not keep its own copy of the snapshot.

The card/hook pair lists catalog rows from `GET /api/v1/config`, serializes
save/apply/clear mutations, and only presents successful mutation state after
an authoritative follow-up GET. It has separate confirmation dialogs for
dangerous save, dangerous apply, and clear. The card disables mutation while
loading, busy, or disconnected; it presents old firmware as unsupported and
has distinct busy, storage, partial-apply, and disconnect states. The dialog
starts on Cancel, traps Tab/Shift-Tab, cancels on Escape when idle, and restores
focus to its opener on close.

The Web UI localizes busy, confirmation, and partial-apply feedback from the
structured error model. It preserves the firmware message for generic errors,
but does not claim every displayed error string is verbatim or automatically
retry a failed mutation. An operator can explicitly retry a failed-only saved
snapshot; apply confirmation covers every saved dangerous row, including an
already-applied dangerous sibling.

## Automatic Current Synchronization

The Web Saved Config panel keeps its `Current` column aligned with the board
without a manual reload. Current is firmware-authoritative data retrieved
from `/api/v1/config`; the underlying status and WebSocket live-control
observations trigger the bounded refresh but are not a client-side overlay.
The browser does not invent Current from status or WS fields, and it does
not own the firmware catalog, the risk policy, or the saved snapshot.

Observed changes to power `state`, switch `route`, or allowlisted GPIO
`direction/value` auto-refresh the `Current` column. Names and identifiers
are supplied by firmware, not a host-side catalog. Identical, reordered, or
unrelated telemetry frames do not trigger additional `/api/v1/config`
GETs; one actual relevant value transition causes exactly one Current
refresh, and the bounded refresh does not flood the firmware even during
high-rate status or WS polling.

Display synchronization does not write flash, change the saved snapshot,
apply pending values, or auto-persist ordinary power, switch, or GPIO
setters. `config save` remains the only path that persists; ordinary
volatile setters stay volatile, and no live transition by itself becomes a
saved snapshot. Local unsaved item-selection drafts (checkbox state) on the
Saved Config panel survive ordinary Current synchronization, so operator
draft intent is preserved when the upstream value changes.

Mutation truthfulness follows the same authority model. Save, apply, and
clear mutations remain pending until the latest authoritative config
response commits to the hook state. A lifecycle change (disconnect,
unsupported firmware, unmount) rejects the pending mutation rather than
letting it settle against a stale response; a superseding transition wins
the race and only then does the mutation resolve.

`Refresh` is a manual recovery or retry action after a transient failed
request or suspected stale UI. It is not a required normal step, and
ordinary live transitions do not need it. The manual refresh issues exactly
one authoritative `GET /api/v1/config` and applies the same authority rules
as a transition-triggered refresh.

Local validation is not real-hardware HIL. The deterministic Hook and
Hook+Card tests plus an independent production loopback mock cover the new
behaviour. Todo 6 post-fix real-hardware HIL remains required for this
code change until executed under the dated combined-UF2 build.

<!-- persistent-config-current-sync:
current-source:Current-from-/api/v1/config
current-trigger:live-power/switch/GPIO-transitions-auto-refresh
current-scope:power-state|switch-route|GPIO-direction-value
current-no-write:display-sync-no-auto-save-no-flash-no-apply
current-no-flood:one-transition-one-refresh;identical-frames-zero-GETs
current-draft-survives:local-checkbox-draft-survives-refresh
current-refresh-recovery:Refresh-manual-recovery-not-required
current-mutation-truthful:save-apply-clear-pending-until-authority
current-hil-boundary:Todo-6-post-fix-HIL-still-required
-->

## CDC ACM Fallback

The same verbs are exposed through the Zephyr shell on the USB CDC ACM port.
The CDC ACM shell is a fallback path for Zephyr cmdline access and BOOTSEL
fallback; the HTTP path is the primary control plane. The CDC ACM shell is
available when the firmware is running even if the USB NCM network has not
acquired an address yet.

The exact CDC ACM grammar is:

- `config show`
- `config save [--confirm] <firmware-item-id>...`
- `config apply --confirm`
- `config clear`

A `config save` with no items is a syntax error and does not reach the
service. A `config apply` without `--confirm` is a syntax error and does
not reach the service. The CDC ACM shell prints the same error codes as
the HTTP API: `empty_selection`, `unknown_item`, `duplicate_item`,
`confirmation_required`, `busy activity=capture`, `busy activity=ota`,
`backend_unavailable`, `no_snapshot`, `invalid_snapshot`,
`unsupported_version`, `storage_error`, `control_capture_failed`, and
`apply_failed`. The CDC ACM shell owns no policy and provides no retry.
The primary result or error line precedes `confirmation_id`, `applied_id`,
`failed_id`, and `pending_id` detail lines; prompt or echo is not a result.

## Capture And OTA Exclusion

The persistent-configuration service refuses to run when another owner is
holding the global resources it needs. The service uses two arbiters:
`linkr_debugger_capture_arbiter` and `linkr_debugger_flash_arbiter`. The
capture arbiter has four owners: `none`, the logic analyzer, the sigrok
linkr sink, and the persistent-configuration service. The flash arbiter
has three owners: `none`, OTA, and the persistent-configuration service.

The `owners_acquire` helper inside `linkr_debugger_config_service` acquires
capture first and flash second. If flash is held by OTA, the service
releases capture and returns `LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_FLASH`.
If capture is held by the logic analyzer or the sigrok sink, the service
returns `LINKR_DEBUGGER_CONFIG_SERVICE_BUSY_CAPTURE`. The release order is
the reverse: flash first, then capture.

The HTTP layer maps both busy codes to the JSON `error.code` `busy`. The
`activity` field is set to `capture` for `BUSY_CAPTURE` and `ota` for
`BUSY_FLASH`. The service does not retry, does not steal the owner, and
does not queue the request. A second busy request after the first release
will succeed normally.

For a successful save, those owners cover the complete control snapshot,
projection, confirmation, and store operation. For clear, they cover the
complete store deletion and status reset. Boot restoration and explicit apply
each acquire once around their full `linkr_debugger_config_apply_execute` call;
the service releases flash first and capture second only after that operation.

## Clear, Recovery, And Firmware Update

`config clear` calls `settings_delete("linkr/config/snapshot")`. The
firmware service status is updated to `snapshot.present = false`, the saved
snapshot is zeroed, and `pending_count` becomes zero. The clear does not alter live hardware: the running board keeps its current power, switch, and GPIO state. The clear response carries `snapshot.present = false`,
`pending = 0`, and `noop` set to true when there was no snapshot to delete.

When the snapshot is missing and the firmware enters `clear`, the response
carries `noop = true` and the same `snapshot.present = false` and
`pending = 0` fields. Clear explicitly removes a corrupt or unsupported
snapshot. A later successful save can instead replace that stored snapshot and
set the service reason to `ready`.

Persistent configuration survives OTA updates. The OTA path uses MCUboot
with an unsigned application payload. The persistent configuration lives on
`storage_partition`, which is separate from the application slots, so an
OTA swap or rollback does not touch the saved snapshot. The retained
recovery marker is independent of the persistent-configuration snapshot.
BOOTSEL scratch index 0 marker `0xadb00751` and OTA-test scratch index 2 marker
`0x07a7e571` are independent of Settings snapshot.

For ROM BOOTSEL flashing, the canonical combined MCUboot plus application
artifact is `radxa-linkr-debugger-rp2350.uf2`. For OTA updates, the
canonical MCUboot-format application payload is
`radxa-linkr-debugger-rp2350-ota.bin`. The app-only `zephyr.uf2` is invalid for ROM BOOTSEL flashing: it does not contain MCUboot, and flashing it for ROM BOOTSEL leaves the board without a bootloader.

## Security Boundaries

The persistent-configuration service provides no security guarantee. The
following are explicit non-features:

- No named profiles are stored or supported. The CLI, TUI, Web UI, and
  CDC ACM shell never expose profile names, profile lists, or profile
  switching. The service has exactly one snapshot and no profile metadata.
- No encryption is used. The snapshot is plain bytes on the NVS partition.
  No key material, no at-rest cipher, no host-side decryption is
  performed. Storage is not described as encrypted storage and not as secure storage either.
- No authentication or authorization is enforced. The HTTP API, the
  WebSocket summary, and the CDC ACM shell are reachable by any host that
  can talk to the board over USB NCM or USB CDC ACM. The service does not
  require, provide, or guarantee any form of authentication or
  authorization.
- No tamper resistance is provided. Any host with USB NCM access can read
  the snapshot, replace it, or delete it. Any host with USB CDC ACM access
  can issue the same verbs through the shell.
- No automatic rollback of saved values is performed. A failed apply stops
  at the first failing entry and leaves earlier entries applied; the
  service does not undo the apply, does not restore a previous snapshot,
  and does not auto-rollback.
- No config-rollback path exists. The OTA retained marker is not a config
  rollback path; the snapshot is independent of the firmware image.

The persistent-configuration snapshot is therefore convenience storage,
not protected storage. Operators must not use it to store credentials or
secrets.

## Validation Boundaries

Local validation is not real-hardware HIL. The Rust CLI and the firmware
service are exercised by:

- `cmd-ng/src/persistent_config_tests.rs` for the HTTP contract.
- `cmd-ng/src/persistent_config_model_tests.rs` for envelope validation
  rules.
- `cmd-ng/src/tui/config_io_tests.rs` for the TUI mutation and refresh
  pattern.
- `apps/radxa_linkr_debugger/tests/model_host` for the firmware service
  logic on a host-side test binary.

These tests run against a loopback mock or a host-side test binary and do
not require the board. They are not real-hardware HIL.

The 2026-07-30 real-hardware HIL passed all six runner flows. See the
[dated persistent-configuration HIL report](testing/results/2026-07-30-persistent-config-hil.md).
The board-level procedure remains defined by
[doc/testing/hil-functional-test-spec.md](testing/hil-functional-test-spec.md)
and validated:

- Boot defaults first, then safe auto-restore, then dangerous pending.
- Live hardware unchanged after `config clear`.
- Capture and OTA busy exclusion with `activity` set to `capture` or `ota`.
- OTA preservation of the snapshot across a swap and rollback.
- Combined-UF2 ROM BOOTSEL recovery does not alter the saved snapshot.
- CDC ACM `config show|save|apply|clear` parity with the HTTP API.

This dated board evidence is separate from the local suites above. Future local
tests remain distinct from real-hardware HIL and cannot replace a board run when
hardware behavior changes.

## Source Of Truth

The behavior described in this document is sourced from these files in the
repository. Future firmware or host CLI changes must update the affected
files and this document together.

Firmware implementation:

- [apps/radxa_linkr_debugger/src/linkr_debugger_config_service.c](../apps/radxa_linkr_debugger/src/linkr_debugger_config_service.c):
  service mutex, capture/flash owner acquisition and reverse release, save,
  apply, and clear entry points.
- [apps/radxa_linkr_debugger/src/linkr_debugger_config_apply.c](../apps/radxa_linkr_debugger/src/linkr_debugger_config_apply.c):
  ten-stage apply order, boot-safe mode that skips dangerous entries, and
  stop-first-failure apply.
- [apps/radxa_linkr_debugger/src/linkr_debugger_config_codec.c](../apps/radxa_linkr_debugger/src/linkr_debugger_config_codec.c):
  catalog, magic `LRCF`, version `1`, entry format, encode and decode
  validation, dangerous classification.
- [apps/radxa_linkr_debugger/src/linkr_debugger_config_store.c](../apps/radxa_linkr_debugger/src/linkr_debugger_config_store.c):
  Zephyr Settings backend, `linkr/config/snapshot` key, `storage_partition`
  mount, save and clear semantics.
- [apps/radxa_linkr_debugger/src/linkr_debugger_config_policy.c](../apps/radxa_linkr_debugger/src/linkr_debugger_config_policy.c):
  request resolution, snapshot projection, canonicalization, and
  confirmation report population.
- [apps/radxa_linkr_debugger/src/linkr_debugger_config_http.c](../apps/radxa_linkr_debugger/src/linkr_debugger_config_http.c):
  HTTP routing, body accumulation, response dispatch, `Cache-Control:
  no-store`.
- [apps/radxa_linkr_debugger/src/linkr_debugger_config_http_encode.c](../apps/radxa_linkr_debugger/src/linkr_debugger_config_http_encode.c):
  envelope serialization for `get`, `save`, `apply`, `clear`, and the error
  envelope with `dangerous_items` and `activity`.
- [apps/radxa_linkr_debugger/src/linkr_debugger_config_http_result.c](../apps/radxa_linkr_debugger/src/linkr_debugger_config_http_result.c):
  HTTP status mapping and error code names.
- [apps/radxa_linkr_debugger/src/linkr_debugger_config_shell.c](../apps/radxa_linkr_debugger/src/linkr_debugger_config_shell.c):
  CDC ACM `config show|save|apply|clear` grammar and error text.
- [apps/radxa_linkr_debugger/src/linkr_debugger_config_summary.c](../apps/radxa_linkr_debugger/src/linkr_debugger_config_summary.c):
  compact status WebSocket summary fields `available`, `reason`,
  `saved_count`, `pending_count`.
- [apps/radxa_linkr_debugger/src/linkr_debugger_capture_arbiter.c](../apps/radxa_linkr_debugger/src/linkr_debugger_capture_arbiter.c):
  capture owner arbitration with the logic analyzer, the sigrok linkr sink,
  and the persistent-configuration service.
- [apps/radxa_linkr_debugger/src/linkr_debugger_flash_arbiter.c](../apps/radxa_linkr_debugger/src/linkr_debugger_flash_arbiter.c):
  flash owner arbitration between OTA and the persistent-configuration
  service.
- [apps/radxa_linkr_debugger/boards/rpi_pico2_rp2350a_m33_mcuboot.conf](../apps/radxa_linkr_debugger/boards/rpi_pico2_rp2350a_m33_mcuboot.conf):
  `CONFIG_NVS`, `CONFIG_SETTINGS`, `CONFIG_SETTINGS_NVS`,
  `CONFIG_NVS_DATA_CRC`, and `CONFIG_SETTINGS_NVS_SECTOR_COUNT=8`.

Host CLI and TUI implementation:

- [cmd-ng/src/persistent_config.rs](../cmd-ng/src/persistent_config.rs):
  envelope, item, snapshot, and busy types shared by CLI and TUI.
- [cmd-ng/src/persistent_config_validate.rs](../cmd-ng/src/persistent_config_validate.rs):
  envelope field validation rules for `get`, `save`, `apply`, `clear`, and
  the known error codes.
- [cmd-ng/src/persistent_config_render.rs](../cmd-ng/src/persistent_config_render.rs):
  human-readable renderer for the four verbs.
- [cmd-ng/src/persistent_config_value.rs](../cmd-ng/src/persistent_config_value.rs):
  per-kind value parsing and kind-mismatch rejection.
- [cmd-ng/src/config_command.rs](../cmd-ng/src/config_command.rs):
  `config show|save|apply|clear` grammar, item-order preservation, and
  `--confirm` handling.
- [cmd-ng/src/client.rs](../cmd-ng/src/client.rs):
  HTTP client for `GET`, `PUT /api/v1/config`, `POST /api/v1/config/apply`,
  `DELETE /api/v1/config`, default base URL, and envelope validation.
- [cmd-ng/src/tui/config_io.rs](../cmd-ng/src/tui/config_io.rs): TUI mutation
  plus authoritative refresh.

Tests:

- [cmd-ng/src/persistent_config_tests.rs](../cmd-ng/src/persistent_config_tests.rs):
  HTTP contract, parser, and renderer behavior.
- [cmd-ng/src/persistent_config_model_tests.rs](../cmd-ng/src/persistent_config_model_tests.rs):
  envelope validation cases.
- [cmd-ng/src/tui/config_io_tests.rs](../cmd-ng/src/tui/config_io_tests.rs):
  mutation-plus-refresh, busy, storage error, and confirmation behavior.
