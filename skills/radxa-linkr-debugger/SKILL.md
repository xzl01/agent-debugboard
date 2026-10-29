---
name: radxa-linkr-debugger
description: Operate a connected Radxa Linkr Debugger through MCP, curl, the optional CLI, or USB CDC ACM. Use it for target power, ADC, routes, safe GPIO, shared UART, persistent configuration, tasks, OTA, ROM BOOTSEL, watchdog recovery, and target OpenOCD sessions.
---

# Radxa Linkr Debugger Operations

Use this runbook to operate a connected G3 Radxa Linkr Debugger with RP2350A.
G2/RP2040 is retired and is not a fallback target. RP2354 is not supported by
this runbook.

This is an operations guide. It controls a target through Linkr; it does not
describe changing, constructing, validating, or publishing Linkr itself.

## Operating Model

The board exposes its control API through USB NCM at:

```sh
# The board is .1 of the host NCM /30; read it from the CDC shell `ip`
# command or set it explicitly.
: "${BOARD_URL:?set BOARD_URL to http://<board-ip> (host NCM /30 address minus one)}"
```

It also exposes an embedded dashboard at `$BOARD_URL/` and a USB CDC ACM shell.
The dashboard, curl, CLI, and MCP ultimately operate the same firmware-owned
catalogs and state. Do not impose host-side defaults for rails, routes, GPIOs,
or ADC channels.

Use this adapter order:

1. Use installed `linkr_*` MCP tools when they are available.
2. Use `curl` against `$BOARD_URL` when MCP is unavailable.
3. Use `radxa-linkr-debuggerctl` for an interactive TUI, `doctor`, or a
   supported batch operation.
4. Use the USB CDC ACM shell only when HTTP is unavailable or a shell-only
   recovery action is needed.

MCP v1 intentionally does not own OTA, ROM BOOTSEL, EDL/MASKROM, or arbitrary
GPIO mutation. Use the explicit workflow in this runbook for those actions.

## Universal Safety Rules

- Read firmware-advertised state before every mutation. Firmware is authoritative
  for catalog entries, accepted values, danger classification, and boot state.
- Confirm the connected target, selected rail, route, voltage, or pin before a
  side-effectful request. Ask for an explicit human decision when the result can
  power, reset, re-route, reflash, or otherwise affect a target.
- Treat `schema`, `ok`, `command`, and `error.code` as structured control flow.
  A successful HTTP transport does not make `ok: false` successful.
- Retry only read-only operations when the response marks them retryable. Never
  automatically replay power, route, GPIO, serial-write, configuration, task,
  OTA, or BOOTSEL mutations after an error.
- Do not infer a safe value from an identifier, previous board, or local map.
  Enumerate the current firmware response instead.
- Prefer target software reboot or OpenOCD reset before power-cycling. A power
  cycle is a destructive hard-restart fallback.
- `reboot` is a normal cold restart; `bootloader` is ROM BOOTSEL. Never
  substitute one for the other.
- `5V_FIN` is a source input, not a controllable power output.

## First Checks

Check the transport and compact board state before operating hardware:

```sh
curl --version
curl -fsS "$BOARD_URL/api/v1/status"
curl -fsS "$BOARD_URL/api/v1/adc/read"
```

For automation, require a response with the expected schema and `ok: true`.
When the board rejects a request, preserve the JSON body and inspect
`error.code` before choosing a new action.

Interpret failures by boundary:

- A valid response with `ok: false` means the board rejected the request.
- A timeout, refusal, or missing route means the USB NCM/HTTP path needs
  attention.
- If HTTP is down but the CDC ACM shell is present, use the limited shell
  fallback in the relevant section.

For browser, permission, captive-portal, or bridge operation, use the
[Web UI guide](../../docs/user/webui.md) and
[troubleshooting guide](../../docs/user/troubleshooting.md).

## MCP Fast Path

The resident endpoint is normally `http://127.0.0.1:8787/mcp`. Use it when its
tools are supplied to the Agent. Its full tool contract is in
[MCP Server](../../docs/reference/mcp-server.md).

### Read State

- `linkr_board_status` returns compact state by default. Request `detail: "full"`
  only when GPIO or complete monitoring data is needed.
- `linkr_adc_read` reads current or voltage telemetry without changing hardware.
- `linkr_serial_status`, `linkr_serial_read`, and `linkr_serial_expect` inspect
  a shared UART channel.

Read status before every mutation and after a user-directed mutation. Do not
poll or request full status repeatedly when compact state is sufficient.

### Confirmed Power And Routes

Use `linkr_power_set` and `linkr_switch_route` only after reading the relevant
firmware catalog and obtaining an explicit approval for that exact target state.
Pass `confirm: true` only for the operation the user has approved.

Do not treat a stale confirmation as permission for a changed rail, route, or
target. Re-read state if the transport disconnected, another user may have
operated the board, or the request returned an error.

### Shared UART Lifecycle

1. Call `linkr_serial_connect` for the selected target UART.
2. Read with `linkr_serial_read` and preserve the returned `next_cursor`.
3. Pass that cursor to the next read or `linkr_serial_expect` call.
4. Use `linkr_serial_login` to send a credential without returning it to the
   Agent transcript.
5. Use `linkr_serial_shell_command` when a POSIX exit result is required.
   Use `linkr_serial_command` for bootloaders and non-shell consoles.
6. Call `linkr_serial_disconnect` when this Agent no longer needs its
   subscription. Other subscribers remain connected.

Serial command and login calls acquire the channel only for their operation and
release it automatically. Do not turn a shared UART into a permanently exclusive
host resource.

Treat `serial_cursor_expired` and `serial_cursor_ahead` as reset boundaries.
Inspect the returned earliest or latest cursor and the surrounding target output
before resuming. Do not re-read an old cursor until its output happens to match
an expected string.
## Target Serial J4
J4 on GP10/GP11 is the product target console, separate from the legacy MCP `linkr_serial_*` CH347 `uart0`/`uart1` broker. Raw HTTP only reads `GET /api/v1/serial/j4` and changes line coding through `PUT /api/v1/serial/j4/config`: send the complete tuple plus the current `expected_generation`, then poll status; stale generation is 409, baud is 4800-115200, data bits 5-8, parity none/odd/even, stop bits 1/2, flow control none. UART bytes never use raw HTTP: subscribe a live session to `{"type":"subscribe","topic":"serial.j4"}` for binary WebSocket writes of at most 480 bytes, use raw TCP `5555` for up to two binary-transparent clients, or use CDC ACM4 and identify the interface rather than assuming `/dev/ttyACM*`. Every sink has its own cursor and drops; inspect `sink_count`, `queue`, and counters after use. J4 is multiwriter, `serial_write_accepted` is admission while `serial_write_complete` is wire completion or failure, and failed writes are not replayable. Keep 5555 for J4 and 5556 for logic analysis. See [HTTP API](../../docs/user/api.md#j4-target-serial) and [Serial Broker](../../docs/reference/serial-broker.md).
### MCP Recovery

For a read-only `host_temporarily_unavailable` result, honor
`error.details.retry_after_ms` only when `error.details.retryable` is true.
MCP reconnect can replace a closed cached broker connection while retaining
monotonic cursor checkpoints.

This recovery rule never authorizes a write replay. After a failed serial
command, login, power, or route operation, obtain current state and a new
operator decision.

## Curl And CLI Fallback

Use curl for portable, machine-readable board control:

```sh
curl -fsS "$BOARD_URL/api/v1/status"
curl -fsS "$BOARD_URL/api/v1/power"
curl -fsS "$BOARD_URL/api/v1/switch"
curl -fsS "$BOARD_URL/api/v1/gpio"
```

Use `--fail-with-body` for a mutating request when the error body is needed:

```sh
curl --fail-with-body -sS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"on"}' \
  "$BOARD_URL/api/v1/power/12v_out"
```

The optional CLI provides the same board operations plus `doctor` and a TUI:
Without an explicit URL or `LINKR_BOARD_URL`, it selects only a unique verified device (`discover` lists devices) and passes that URL to Tray; an existing Host on another URL is not switched automatically—check for other clients, then quit and relaunch it explicitly.

```sh
radxa-linkr-debuggerctl --json doctor
radxa-linkr-debuggerctl --json status
radxa-linkr-debuggerctl
```

In the TUI, the first keyboard or mouse activation of every advertised switch
opens the existing three-second hardware confirmation and sends no request.
The TUI applies this gate to every firmware-advertised switch regardless of
the firmware `requires_confirm` flag, and the policy itself never hardcodes
any production switch name or route. A switch click selects its full row
before the modal opens; that full-row selection stays visible on every cell
not covered by the modal until the confirmation resolves. A fresh
confirmation routes once; cancellation or timeout routes zero times. This
TUI gate does not replace firmware `requires_confirm`, which remains
authoritative for Saved Config and the API/CLI surfaces.

On the Saved Config page, left-button Down on a visible item row re-resolves
that item's stable firmware ID, focuses the page, moves the cursor, and
toggles the local checkbox selection exactly once. Mouse Up, Drag, Moved,
scrolling, middle or right-button events, and Down on a stale row whose ID
is no longer present are inert. The visible `Controls`, `Saved Config`, and
`Status` tab labels accept left-button Down to switch pages; the active
label, the two-space gaps between labels, every other mouse event, and tab
input blocked by a confirmation or Saved Config error are inert.

These TUI-only behaviors do not change the Web UI, the firmware, the HTTP
API, the persistent-configuration wire contract, or the non-TUI CLI
commands. For the full TUI interaction contract, see the
[TUI guide](../../docs/user/tui.md); the canonical design contract,
including the universal switch confirmation semantics and the scope gutter
geometry, lives in [cmd-ng/DESIGN.md](../../cmd-ng/DESIGN.md).

Use `--json` in unattended CLI flows and parse the same response envelope as
curl. For installation or a platform-specific binary, use the
[install guide](../../docs/user/install.md); do not substitute a locally
constructed executable for an installed operator tool.

## Power Outputs

Enumerate outputs before changing one:

```sh
curl -fsS "$BOARD_URL/api/v1/power"
curl -fsS "$BOARD_URL/api/v1/power/12v_out"
```

After confirmation, set only the advertised output and requested state:

```sh
curl --fail-with-body -sS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"on"}' \
  "$BOARD_URL/api/v1/power/12v_out"
```

For a hard restart, first confirm that the selected output powers the target.
Turn it off, allow the target to discharge, then turn it back on:

```sh
curl --fail-with-body -sS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"off"}' \
  "$BOARD_URL/api/v1/power/5v_out"
sleep 2
curl --fail-with-body -sS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"on"}' \
  "$BOARD_URL/api/v1/power/5v_out"
```

On current x1.2 firmware, `5v_usb_hub` and `5v_otg` are child rails of `vdd_5v`.
Enabling a child can enable the parent first; disabling the parent disables both
children and clears OTG desired state. Enabling the parent alone does not enable
either child. Both children boot off. The OTG port is backfeed-capable: off does
not prove externally supplied VBUS is absent. Inspect parent and both children
after a user-approved change.

Do not use a fixed rail list as a target contract. Firmware may advertise a
different catalog, state, or confirmation requirement.

## Switch Routes

Read the switch catalog and the selected switch before changing a route:

```sh
curl -fsS "$BOARD_URL/api/v1/switch"
curl -fsS "$BOARD_URL/api/v1/switch/sd"
curl -fsS "$BOARD_URL/api/v1/switch/tf_wp"
```

Examples use only values returned by current firmware. Do not reuse them until
the matching GET confirms that the route is currently advertised:

```sh
curl --fail-with-body -sS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"usb-reader"}' \
  "$BOARD_URL/api/v1/switch/sd"

curl --fail-with-body -sS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"protected"}' \
  "$BOARD_URL/api/v1/switch/tf_wp"
```

Current x1.2 firmware advertises the `sd` and `tf_wp` switches. `vdd_5v` is an
independent power output and does not follow switch-route changes. The firmware
owns the catalog, accepted routes, and boot state; inspect current status after
a user-approved route change rather than forcing a host-side default.

`switch tf_wp` affects write protection as interpreted by the reader. A route
change can require card reattachment before the reader observes it.

## Safe GPIO

Discover the current GPIO catalog before selecting a pin:

```sh
curl -fsS "$BOARD_URL/api/v1/gpio"
curl -fsS "$BOARD_URL/api/v1/gpio/GP13"
```

Only use output-capable entries advertised by firmware. The x1.2 safe
surface is `GP7`-`GP9`, `GP12`-`GP18`, and input-only `GP29`; `GP10`/`GP11`
are the J4 UART, `GP19` is the CH224A PD strap, and `GP20`/`GP21` are the
PIO I2C bus on J8 (GP20=SCL, GP21=SDA, not GPIO controls), so the live catalog remains
authoritative. `GP29` is owned by `adc3` on this firmware and is input-only;
do not retry an output request that firmware rejects.

After confirmation, request the intended direction and value:

```sh
curl --fail-with-body -sS -X PUT -H 'Content-Type: application/json' \
  --data '{"direction":"output","value":1}' \
  "$BOARD_URL/api/v1/gpio/GP13"

curl --fail-with-body -sS -X PUT -H 'Content-Type: application/json' \
  --data '{"direction":"input"}' \
  "$BOARD_URL/api/v1/gpio/GP13"
```

Do not expose schematic codenames as a substitute for the firmware catalog.
Use a catalog-provided name, raw pin number, or exact board note only when the
current API identifies that same pin.

In the TUI (`radxa-linkr-debuggerctl` with no arguments), a selected GPIO cell
takes direct keys: `l`/`L` drives LOW, `o`/`O` drives HIGH, `i`/`I` restores
input; Enter, Space, `0`, and `1` are inert on a GPIO. The direct-key decoder
accepts only lowercase `l`/`o`/`i` with no modifiers and uppercase `L`/`O`/`I`
with exactly Shift; any other modifier (Ctrl, Alt, Super) and any mismatched
case/Shift form returns no intent and is fully inert without disturbing the
current gesture state. A left click released before 600 ms opens a 220 ms
second-click window whose expiry drives LOW; a press held for 600 ms drives
HIGH once, and a second press on the same pin before that window expires
restores input. Middle and right buttons are always inert. A held press shows
`[HOLD…]` on the pin until the gesture resolves. Same-cell Moved or left
Drag reports remain active; only a report that crosses a terminal cell
cancels the gesture. A terminal `Resize` is the TUI redraw boundary: it
cancels any in-flight GPIO gesture, invalidates the existing hit rectangles,
and forces a redraw before the next queued input is consumed. The visible
`Controls`, `Saved Config`, and `Status` tab labels accept left-button Down to
switch pages; the active label, two-space gaps, other mouse events, and tab
input blocked by a confirmation or Saved Config error are inert. See the
[TUI guide](../../docs/user/tui.md) for the full interaction contract.

## ADC, Power Analysis, And Logic Capture

Read current and voltage telemetry without changing hardware:

```sh
curl -fsS "$BOARD_URL/api/v1/adc/read"
curl -fsS "$BOARD_URL/api/v1/adc/read?channel=5v_out"
curl -fsS "$BOARD_URL/api/v1/adc/read?channel=adc3"
```

Treat firmware values and channel metadata as authoritative. `adc0` has
`role:"pd_input"`; its canonical value is source-side DC20V reconstructed from
the Device Tree divider, while `raw`/`mv` remain pad diagnostics. ADC1-3 have
`role:"native"` and report pad voltage. Never multiply `raw`/`mv` by `115/15`
or apply a host calibration table. ADC0 accuracy is uncalibrated and about 2%.
`adc3` reports GP29 pad voltage; it is not a controllable current output.
`5v_usb_hub` has no INA226 channel, so do not substitute another rail's reading
for it or report a fabricated measurement. See the
[ADC telemetry contract](../../docs/reference/adc-telemetry.md).

Use the [Power Analyzer guide](../../docs/user/power-analyzer.md) for recorded
current workflows. Use the [Logic Analyzer guide](../../docs/user/logic-analyzer.md)
for target capture setup. Use the [logic analyzer reference](../../docs/reference/logic-analyzer.md)
only when a protocol limit or transport response needs interpretation.

Do not start concurrent capture transports without first checking the current
capture state. A capture or OTA activity can make other mutations busy.

Before capture, inspect GPIO state across the full physical sampling lane, not
only the displayed channels. Release conflicting outputs to input only after
the operator approves the exact pins; otherwise capture is refused. Do not
automatically switch pin directions to seize resources. Stop capture and wait
for cleanup before requesting either input or output changes on those GPIOs.

## Persistent Configuration

This is the operator-facing view of the canonical contract in
[Persistent Configuration](../../docs/reference/persistent-configuration.md).
Keep this section operational only.

### Read Saved Configuration

Read `GET /api/v1/config` before any save or clear. The firmware catalog,
snapshot status, and per-item `requires_confirm` flags are authoritative;
do not derive risk from local memory or a previous board.

### Save Selected Current Values

Only save IDs returned by the current GET. `requires_confirm` is the sole
danger gate. Safe items use `confirm: false`; dangerous items use
`confirm: true` only after a human reviews the exact current value. Save
persists the v1 snapshot and applies it immediately.

### Clear Without Changing Hardware

`DELETE /api/v1/config` deletes the stored snapshot only. It does not restore
defaults or alter live power, route, or GPIO state.

### Handle Confirmation And Busy Errors

Handle `confirmation_required`, `busy`, and `apply_failed` as structured
failures. For `busy`, inspect `activity` (`capture` or `ota`) and wait for the
owner to finish. Do not auto-confirm `confirmation_required`, and do not replay
writes automatically after any error.

Use `curl --fail-with-body` when you need the JSON body. Preserve the response
body and check `[ "$curl_status" -eq 22 ]` before parsing. Keep the body
available with `response="$(curl --fail-with-body ...)"` and print it with
`printf '%s\n' "$response"` if you need to inspect the error payload.

### CLI And CDC Fallback

The CLI verbs are `radxa-linkr-debuggerctl --json config show`,
`radxa-linkr-debuggerctl --json config save [--confirm] <firmware-item-id>...`,
and `radxa-linkr-debuggerctl --json config clear`. The CDC ACM fallback exposes
`config show`, `config save [--confirm] <firmware-item-id>...`, and
`config clear`. Parse `schema`, `ok`, `command`, and `error.code` from both.

### Automatic Current Synchronization

The `Current` column is firmware-authoritative data from `/api/v1/config`.
Observed changes to power `state`, switch `route`, or allowlisted GPIO
`direction/value` trigger one follow-up GET; identical or unrelated frames do
not. Display sync does not write flash, change the saved snapshot, or
auto-persist ordinary setters. Local unsaved selection drafts survive the
refresh. Save and clear remain pending until the authoritative response lands.

<!-- persistent-config-current-sync:
current-source:Current-from-/api/v1/config
current-trigger:live-power/switch/GPIO-transitions-auto-refresh
current-scope:power-state|switch-route|GPIO-direction-value
current-no-write:display-sync-no-auto-save-no-flash-no-apply
current-no-flood:one-transition-one-refresh;identical-frames-zero-GETs
current-draft-survives:local-checkbox-draft-survives-refresh
current-refresh-recovery:Refresh-manual-recovery-not-required
current-mutation-truthful:save-clear-pending-until-authority
-->

### Persistence Recovery Safety

Use the canonical contract for snapshot semantics. The snapshot lives under
`storage_partition` through Settings+NVS at `linkr/config/snapshot`;
`config clear` removes the snapshot only.

## Firmware Tasks
Firmware owns two task sources:

- an immutable built-in catalog at `GET /api/v1/tasks/catalog`;
- a four-slot task-v2 store described by `GET /api/v1/tasks` and read as exact
  IR from `GET /api/v1/tasks/ir`.

Read both sources before selecting a task:
```sh
curl -fsS "$BOARD_URL/api/v1/tasks/catalog"
curl -fsS "$BOARD_URL/api/v1/tasks"
curl -fsS "$BOARD_URL/api/v1/tasks/ir" -o /tmp/linkr-tasks.ir
radxa-linkr-debuggerctl task list
```
Built-ins and stored tasks remain separate. A catalog failure must fail closed
for a requested built-in; never fall back to a stored task with the same ID.
Stored tasks are inert until an operator explicitly runs one. Firmware does not
run, replay, or seed task sequences at boot.

Use the CLI to list, store, import/export, delete, clear, or run tasks:
```sh
radxa-linkr-debuggerctl task list
radxa-linkr-debuggerctl task store my-task.ndjson my-task-id
radxa-linkr-debuggerctl task import all-tasks.ndjson
radxa-linkr-debuggerctl task export all-tasks.ndjson
radxa-linkr-debuggerctl task source my-task-id my-task.source.ndjson
radxa-linkr-debuggerctl task delete my-task-id
radxa-linkr-debuggerctl task clear
radxa-linkr-debuggerctl task run builtin/maskrom/12v_out --confirm
radxa-linkr-debuggerctl task run my-task-id --confirm
```
`task store` writes the compiled one-task IR without a source payload, so the
board task is IR-only. Use the Web workflow editor when board-backed lossless
source is required. `task source` fails with `task_source_absent` for IR-only
tasks. Task IR import/export and workflow import/export are different
operations; do not treat IR as editable workflow source.

Task writes use only the binary `POST /api/v1/tasks/mutations` with expected
revision/catalog. There is no task blob upload or per-ID write/delete route.
The canonical wire, source/IR limits, and conflict/error rules are in the
[HTTP API](../../docs/user/api.md#tasks-firmware-catalog-and-task-v2-storage).

`task run` dispatches ordinary control requests in order. `wait_ms` is client
timing metadata and is not sent to the board. A failed built-in stops its main
sequence and attempts only catalog-declared cleanup. Inspect both diagnostics,
then inspect and restore the selected rail through ordinary APIs. Do not invent
cleanup or replay a failed task.

## OTA Firmware Update

OTA is an operation against a connected board with MCUboot already installed.
It accepts a MCUboot-format application binary such as
`radxa-linkr-debugger-rp2350-ota.bin`.

Do not upload `.uf2` or `.elf` files to OTA. They are not OTA payloads.
Initial installation and ROM recovery use the combined UF2 workflow below.

Before upload, confirm the target, the exact image, and the intended reboot.
Check current OTA state:

```sh
curl -fsS "$BOARD_URL/api/v1/ota"
radxa-linkr-debuggerctl --json ota status
```

Upload, request a test boot, then confirm only after the result is acceptable:

```sh
radxa-linkr-debuggerctl --json ota upload /path/to/radxa-linkr-debugger-rp2350-ota.bin
radxa-linkr-debuggerctl --json ota test
radxa-linkr-debuggerctl --json ota confirm
```

For raw HTTP, send the image size and SHA-256 with the binary:

```sh
IMAGE=/path/to/radxa-linkr-debugger-rp2350-ota.bin
SIZE=$(wc -c < "$IMAGE" | tr -d '[:space:]')
SHA256=$(sha256sum "$IMAGE" | awk '{print $1}')
curl --fail-with-body -sS -X POST \
  -H 'Content-Type: application/octet-stream' \
  -H "X-Linkr-Ota-Size: $SIZE" \
  -H "X-Linkr-Ota-Sha256: $SHA256" \
  --data-binary "@$IMAGE" \
  "$BOARD_URL/api/v1/ota/upload"
curl --fail-with-body -sS -X POST "$BOARD_URL/api/v1/ota/test"
curl --fail-with-body -sS -X POST "$BOARD_URL/api/v1/ota/confirm"
```

After `ota test`, firmware owns the watchdog health gate and can confirm a
healthy image. The dashboard never confirms automatically. Do not assume a
rollback is a substitute for retaining a physical ROM BOOTSEL recovery path.

The OTA endpoint has no signature, authentication, secure-boot, or
anti-rollback protection. USB NCM access is sufficient to submit an image.
Keep the board and its USB connection physically controlled during this action.

## Normal Restart And Physical BOOTSEL

`reboot` and `bootloader` are separate operations. Never treat them as aliases.

Request a normal cold restart when the board is reachable and the target does
not need ROM BOOTSEL:

```sh
curl --fail-with-body -sS -X POST "$BOARD_URL/api/v1/reboot"
```

Firmware restarts 250 ms after the response and returns to normal operation.
The reboot action itself never requests ROM BOOTSEL and never enumerates as
`RPI-RP2`; holding the physical BOOTSEL button across that reset is a separate
ROM behavior. A rail not restored by a saved v1 configuration snapshot returns
to its boot-off default; `vdd_5v` is independent and returns to its boot-off
default. Re-establish and verify any rail or PD setup after the board returns.

The physical BOOTSEL button has its own semantics:

- Hold it for 3 seconds while firmware runs, then release: normal cold restart.
- Short press: no action.
- Keep holding without releasing: no reset.
- Hold it during power-on or reset: the RP2350 ROM samples it and enters ROM
  BOOTSEL.

The button is serviced by a firmware thread. A completely wedged core cannot
run that thread, so a long press is not a recovery path for a full hang. The
hardware watchdog remains the fallback; a normal watchdog reset enters ROM
BOOTSEL through the recovery marker, while an OTA test-marker reset is handled
by MCUboot rollback. Do not describe a successful long-press restart as
`bootloader`, and do not use `bootloader` merely to restart the board.

## ROM BOOTSEL Recovery

For ROM BOOTSEL initial installation or recovery, use only the combined
MCUboot-plus-application artifact:

```text
radxa-linkr-debugger-rp2350.uf2
```

Never use the application-only `zephyr.uf2` for ROM BOOTSEL. It does not
include MCUboot and can leave the board unable to start normally.

When HTTP works, request BOOTSEL explicitly:

```sh
curl --fail-with-body -sS -X POST "$BOARD_URL/api/v1/bootloader"
```

The USB NCM connection can close during reset. Wait for an `RPI` vendor device
instead of assuming a fixed `/dev/sdX` name:

```sh
RPI_DISK=
attempts=10
while [ "$attempts" -gt 0 ]; do
  RPI_DISK=$(timeout 5s lsblk -dpno NAME,VENDOR | awk '$2 == "RPI" { print $1; exit }')
  [ -n "$RPI_DISK" ] && break
  attempts=$((attempts - 1))
  sleep 1
done
[ -n "$RPI_DISK" ] || { echo "BOOTSEL device not found"; exit 1; }
```

Mount the discovered partition and copy the combined UF2 selected for this
recovery:

```sh
RPI_PART=$(timeout 5s lsblk -lnpo NAME,TYPE "$RPI_DISK" | awk '$2 == "part" { print $1; exit }')
[ -n "$RPI_PART" ] || { echo "BOOTSEL partition not found"; exit 1; }
RPI_MOUNT=$(timeout 5s udisksctl mount -b "$RPI_PART" | awk -F" at " '{print $2}' | tr -d '[:space:]')
FLASH_UF2=/path/to/radxa-linkr-debugger-rp2350.uf2
cp "$FLASH_UF2" "$RPI_MOUNT/"
```

If the board is already in BOOTSEL, use the same combined UF2. Do not replace
it with an application-only file because a recovery route appears to work.

When HTTP is unavailable but CDC ACM remains available, use:

```text
linkr-debugger:~$ bootloader
```

The same ROM BOOTSEL path should then expose the `RPI-RP2` storage target. The
[OTA guide](../../docs/user/ota.md) and
[flashing guide](../../docs/developer/flashing.md) provide artifact detail.

## Watchdog And Transport Recovery

The watchdog is firmware-owned. Hosts do not arm or feed it. Read its state:

```sh
curl -fsS "$BOARD_URL/api/v1/watchdog"
radxa-linkr-debuggerctl --json watchdog status
```

If the normal HTTP path fails, work through the fallback order:

1. Check that USB NCM has enumerated and that `$BOARD_URL/api/v1/status` is
   reachable.
2. Use a supported MCP or CLI `doctor` read if the host adapter is present.
3. Use the CDC ACM shell for status, configuration, task visibility, or
   BOOTSEL actions that it exposes.
4. Enter ROM BOOTSEL through a confirmed physical recovery path when the board
   cannot resume normal control-plane operation.

Do not use a watchdog symptom to replay a previous mutation. Re-enumerate the
board, read current state, and ask for a new operation.

## Target OpenOCD / JTAG

Linkr controls target power and recovery lines. It is not a CMSIS-DAP,
Picoprobe, or JTAG/SWD probe. The onboard CH347F is the target debug path.

Before starting OpenOCD, power the target through the confirmed output that
actually supplies it. Then start OpenOCD with a CH347F interface script and
the target configuration:

```sh
curl --fail-with-body -sS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"on"}' \
  "$BOARD_URL/api/v1/power/5v_out"
openocd -f interface/<ch347-interface>.cfg -f target/<target>.cfg
```

If a reset is needed, prefer the target reset mechanism or OpenOCD commands:

```text
reset halt
reset run
```

Power-cycle only when soft reset is unavailable or the target is unresponsive.
For adapter setup and target-specific detail, use the
[OpenOCD guide](../../docs/user/openocd.md).

## Reference Links

- [Quick Start](../../docs/user/quickstart.md) for initial connection.
- [Install](../../docs/user/install.md) for MCP, CLI, and host tools.
- [HTTP API](../../docs/user/api.md) for raw endpoint contracts.
- [CLI](../../docs/user/cli.md) for command grammar and task behavior.
- [Web UI](../../docs/user/webui.md) for dashboard and serial-terminal use.
- [Workflows](../../docs/user/workflows.md) for SD, serial, power, OTA, and
  capture operation sequences.
- [MCP Server](../../docs/reference/mcp-server.md) and
  [Serial Broker](../../docs/reference/serial-broker.md) for shared UART
  behavior.
- [Persistent Configuration](../../docs/reference/persistent-configuration.md)
  for snapshot semantics.
- [ADC Telemetry](../../docs/reference/adc-telemetry.md) for channel ownership
  and wire details.
- [x1.2 Hardware and BSP](../../docs/reference/hardware-x1.2.md) for board
  rails, INA226, PIO I2C, J4 UART, and the flash/partition model.

## Final Safety Checklist

- Confirm the target and current firmware-advertised state before mutation.
- Keep state-changing operations explicit and human-confirmed.
- Preserve serial cursors; do not replay old writes after an error.
- Keep configuration saves distinct from ordinary volatile setters.
- Keep stored tasks inert until explicitly run, and do not invent task cleanup.
- Use only `radxa-linkr-debugger-rp2350-ota.bin`-style MCUboot binaries for OTA.
- Use only `radxa-linkr-debugger-rp2350.uf2` for ROM BOOTSEL.
- Keep `reboot` (normal cold restart) and `bootloader` (ROM BOOTSEL) separate.
- Retain physical recovery access while operating OTA or BOOTSEL.
