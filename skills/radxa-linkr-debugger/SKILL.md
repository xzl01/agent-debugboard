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
BOARD_URL="${BOARD_URL:-http://172.29.203.1}"
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

```sh
radxa-linkr-debuggerctl --json doctor
radxa-linkr-debuggerctl --json status
radxa-linkr-debuggerctl
```

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

Do not use a fixed rail list as a target contract. Firmware may advertise a
different catalog, state, or confirmation requirement.

## Switch Routes And VIN

Read the switch catalog and the selected switch before changing a route:

```sh
curl -fsS "$BOARD_URL/api/v1/switch"
curl -fsS "$BOARD_URL/api/v1/switch/sd"
curl -fsS "$BOARD_URL/api/v1/switch/usb"
curl -fsS "$BOARD_URL/api/v1/switch/tf_wp"
curl -fsS "$BOARD_URL/api/v1/switch/vin"
```

Examples use only values returned by current firmware. Do not reuse them until
the matching GET confirms that the route is currently advertised:

```sh
curl --fail-with-body -sS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"usb-reader"}' \
  "$BOARD_URL/api/v1/switch/sd"

curl --fail-with-body -sS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"target"}' \
  "$BOARD_URL/api/v1/switch/usb"
```

Changing `switch usb` can affect the `vdd_5v` USB-hub domain. The firmware
owns this relationship and its boot state; inspect current status after a
user-approved route change rather than forcing a host-side default.

`switch tf_wp` affects write protection as interpreted by the reader. A route
change can require card reattachment before the reader observes it.

### VIN Safety

VIN defaults to `3.3v`. Selecting `1.8v` is side-effectful and can damage an
incompatible target. Before requesting it:

1. Confirm the target VIO supports the requested voltage.
2. Prepare physical measurement of the target VIO pin.
3. Obtain an explicit approval for the exact voltage transition.

Restore the safe default when the target session no longer requires 1.8 V:

```sh
curl --fail-with-body -sS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"3.3v"}' \
  "$BOARD_URL/api/v1/switch/vin"
```

If only CDC ACM remains, use:

```text
linkr-debugger:~$ vin get
linkr-debugger:~$ vin set 3.3v
```

## Safe GPIO

Discover the current GPIO catalog before selecting a pin:

```sh
curl -fsS "$BOARD_URL/api/v1/gpio"
curl -fsS "$BOARD_URL/api/v1/gpio/GP13"
```

Only use output-capable entries advertised by firmware. The usual G3 safe
surface includes `GP7` through `GP20`, but the live catalog is authoritative.
`GP29` is owned by `adc3` on this firmware and is input-only; do not retry an
output request that firmware rejects.

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

## ADC, Power Analysis, And Logic Capture

Read current and voltage telemetry without changing hardware:

```sh
curl -fsS "$BOARD_URL/api/v1/adc/read"
curl -fsS "$BOARD_URL/api/v1/adc/read?channel=5v_out"
curl -fsS "$BOARD_URL/api/v1/adc/read?channel=adc3"
```

Treat firmware values and channel metadata as authoritative. `adc3` reports
GP29 voltage; it is not a controllable current output.

Use the [Power Analyzer guide](../../docs/user/power-analyzer.md) for recorded
current workflows. Use the [Logic Analyzer guide](../../docs/user/logic-analyzer.md)
for target capture setup. Use the [logic analyzer reference](../../docs/reference/logic-analyzer.md)
only when a protocol limit or transport response needs interpretation.

Do not start concurrent capture transports without first checking the current
capture state. A capture or OTA activity can make other mutations busy.

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

- An immutable built-in catalog at `GET /api/v1/tasks/catalog`.
- Explicitly stored task blobs at `GET`, `PUT`, and `DELETE /api/v1/tasks`.

Read both sources before selecting a task:

```sh
curl -fsS "$BOARD_URL/api/v1/tasks/catalog"
curl -fsS "$BOARD_URL/api/v1/tasks"
radxa-linkr-debuggerctl task list
```

Built-ins and stored tasks remain separate. A catalog failure must fail closed
for a requested built-in; never fall back to a stored task with the same ID.
Stored tasks are inert until an operator explicitly runs one. Firmware does not
run, replay, or seed task sequences at boot.

Use the CLI to store, run, or clear an explicit task sequence:

```sh
radxa-linkr-debuggerctl task store my-task.ndjson my-task-id
radxa-linkr-debuggerctl task run builtin/maskrom/12v_out --confirm
radxa-linkr-debuggerctl task run my-task-id --confirm
radxa-linkr-debuggerctl task clear
```

`task run` dispatches ordinary control requests in order. `wait_ms` is client
timing metadata and is not sent to the board. A failed built-in stops its main
sequence and attempts only catalog-declared cleanup. Inspect both diagnostics,
then inspect and restore the selected rail through ordinary APIs. Do not invent
cleanup or replay a failed task.

With CDC ACM only, use the intentionally limited task surface:

```text
linkr-debugger:~$ task show
linkr-debugger:~$ task clear
```

There are no CDC task boot, default, run, or replay commands. The complete task
wire contract is in the [HTTP API](../../docs/user/api.md) and
[CLI guide](../../docs/user/cli.md).

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
3. Use the CDC ACM shell for status, configuration, task visibility, VIN, or
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

## Final Safety Checklist

- Confirm the target and current firmware-advertised state before mutation.
- Keep state-changing operations explicit and human-confirmed.
- Preserve serial cursors; do not replay old writes after an error.
- Keep configuration saves distinct from ordinary volatile setters.
- Keep stored tasks inert until explicitly run, and do not invent task cleanup.
- Use only `radxa-linkr-debugger-rp2350-ota.bin`-style MCUboot binaries for OTA.
- Use only `radxa-linkr-debugger-rp2350.uf2` for ROM BOOTSEL.
- Retain physical recovery access while operating OTA or BOOTSEL.
