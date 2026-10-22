[中文](api.zh-CN.md)

# HTTP API Reference

Base URL: the board `.1` address of the host NCM `/30` (the CDC shell `ip` command prints it); examples use `$BOARD_URL` (USB NCM interface, port 80). The board derives and persists a unique private `/30` on first boot; see [Network Identity](../reference/network.md).

```sh
export BOARD_URL=http://<board-ip>
```

All JSON responses include `"schema": "radxa-linkr-debugger.v1"` and `Cache-Control: no-store`.

## Response Envelope

Success:

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": true,
  "command": "<command>"
}
```

Failure (`error` is present only when `ok` is `false`):

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": false,
  "command": "<command>",
  "error": { "code": "<code>", "message": "<message>" }
}
```

## Network Identity

### `GET /api/v1/net`

Returns the persisted MAC address, origin (`derived` or `manual`), private `/30`
network, board address, DHCP base address, and board URL.

```sh
curl -fsS "$BOARD_URL/api/v1/net"
```

### `PUT /api/v1/net`

Updates `mac`, `network`, or both. Omitted fields keep their current value.
`mac` must be a locally administered unicast address; `network` must be a
private `/30` base, optionally with a `/30` suffix. Manual values are persisted
in the same atomic identity blob and take precedence over derivation.

```sh
curl -fsS -X PUT -H 'Content-Type: application/json' \
  -d '{"network":"10.20.30.0"}' "$BOARD_URL/api/v1/net"
curl -fsS -X PUT -H 'Content-Type: application/json' \
  -d '{"mac":"02:11:22:33:44:55"}' "$BOARD_URL/api/v1/net"
```

## Status

```sh
curl $BOARD_URL/api/v1/status
```

Returns everything in one shot: `project`, `build` (including `profile` and `image_version`), `mcu`, `usb`, `power_inputs`, `power_outputs`, `switches`, `adc_channels`, `watchdog`, `board_monitoring`, `gpio_layout_groups`, and `gpios`.

The encoded status body must remain strictly smaller than the dedicated
6656-byte firmware buffer. This is a protocol size gate for additions to the
status snapshot, not permission for clients to assume a fixed response length.

`power_outputs` is firmware-advertised and dynamic. On x1.2 it contains the
six controllable entries `12v_out`, `5v_out`, `20v_out`, `vdd_5v`, `5v_usb_hub`,
and `5v_otg`. `5v_usb_hub` and `5v_otg` are `vdd_5v` child branches, have no
INA226/ADC descriptor, and do not add a status field outside the existing
power-output shape.

## J4 Target Serial

The product exposes one logical target serial port, `j4`. It is the dedicated
RP2350 UART1 path on GP10/GP11. Firmware fans one shared
4096-byte UART RX ring out independently to USB CDC ACM4, four WebSocket sinks,
and two raw-TCP sinks. A slow sink loses only its own oldest bytes.

### `GET /api/v1/serial/j4`

Returns availability, runtime state, current line coding, configuration
generation, DTR/RTS and USB-configuration state, attached sink counts, queue
state, transport counters, per-sink drop counters, and `config_request` /
`config_counters` diagnostics. The latter distinguishes CDC/Web requests,
invalid/conflict/no-change outcomes, configure/apply results, and rollback
results. `state` is `ready`,
`reconfig`, `paused`, `faulted`, or `unavailable`.

The same response reports the global automatic terminal responder as
`terminal_responder`, an object such as
`{"transport":"cdc","slot":0}` or `{"transport":"websocket","slot":2}`, or
`null` when none is selected. CDC wins while DTR is asserted; otherwise the
lowest-numbered attached WebSocket is selected. Raw TCP is never selected.

`dtr` is the published CDC/gate state, true only after promotion completes; it
is not the raw physical DTR sample. The response does not expose an internal CDC
phase or block-reason field.

```sh
curl -fsS $BOARD_URL/api/v1/serial/j4
```

### `PUT /api/v1/serial/j4/config`

Admits an asynchronous compare-and-swap line-coding update. Read the current
`generation`, then send the complete tuple and that generation:

```sh
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"baud":115200,"data_bits":8,"parity":"none","stop_bits":1,"flow_control":"none","expected_generation":1}' \
  $BOARD_URL/api/v1/serial/j4/config
```

Success is HTTP 202 with `accepted:true` and `request_id`. A stale generation
or an in-progress transition returns HTTP 409. Baud accepts every integer from
4800 through 115200 inclusive. The five recommended presets are 9600, 19200,
38400, 57600, and 115200; custom values such as 5200 are valid. Requests above
115200, including 115201, return HTTP 400 and leave the current tuple unchanged.
Data bits are 5–8, parity is `none`/`odd`/`even`, stop bits are
1 or 2, and flow control is currently `none` only.

Rates above 115200 are not product-supported. The 230400/921600/1500000
samples in the HIL record are diagnostics that motivated the cap, not accepted
client configurations.

Application is asynchronous. Poll `GET /api/v1/serial/j4`; its current tuple,
generation, completion result and diagnostics are authoritative. A USB CDC host
SET_LINE_CODING transfer cannot report a later application-level rejection to
the host, so CDC tools must also read this status rather than assuming their SET
was applied.

While CDC DTR is false, a USB default line-coding notification only stages a
candidate tuple and does not change active line coding or generation. An HTTP/WS
PUT in that low state applies immediately. When the CDC session owns the port
(DTR high/promoting), HTTP/WS PUT returns conflict instead of being staged. On
LOW→HIGH the board applies any staged USB tuple, then publishes `dtr=true` and
opens ingress; a failed apply/attach/detach keeps the path blocked until a
successful LOW/reset.

### Streaming transports

- The embedded Web terminal creates an ordinary live session and subscribes
  with `{"type":"subscribe","topic":"serial.j4"}`. MCU-to-browser bytes are
  binary WebSocket messages. Each browser-to-MCU binary message is one atomic
  serial write and may contain at most 480 bytes.
- Explicit J4 input is multiwriter. Only automatic terminal replies use the one
  selected responder. The WS `serial_state` frame carries a per-client
  `terminal_responder` boolean plus a `responder` descriptor; only an explicit
  `true` authorizes that page to answer terminal queries. Other Web clients still
  send keyboard, IME, paste, and Ctrl-C input; they are observers for automatic
  replies, not read-only. ANSI sequences are not filtered.
- Raw TCP listens on `0.0.0.0:5555`, with at most two clients. It is a binary
  stream with no banner, echo, CR/LF conversion, Telnet/IAC negotiation, SSH,
  or noVNC semantics. For example: `nc <board-ip> 5555`.
- USB CDC ACM4 is the direct USB fallback for the same logical J4 stream. USB
  tty numbering is host-dependent; identify the interface rather than assuming
  a `/dev/ttyACM*` number.

Legacy desktop-local serial transports are retired and are not product paths.
Port 5173 is only a Vite development preview; the tested embedded
UI is served by the board on port 80.

## Power

```sh
curl $BOARD_URL/api/v1/power                    # list all
curl $BOARD_URL/api/v1/power/12v_out             # read one
curl -X PUT $BOARD_URL/api/v1/power/12v_out -d '{"state":"on"}'  # set
```

`{name}` is `12v_out`, `5v_out`, `20v_out`, `vdd_5v`, `5v_usb_hub`, or `5v_otg`. Body: `{"state": "on"}` or `{"state": "off"}`.

`5v_otg` is the bidirectional J3 VBUS switch (`OTG_USB_EN`, GPIO23) and a child
of `vdd_5v`. Its current `state`/`value` reflect the requested/desired enable;
actual conduction requires `vdd_5v`, and the port can be externally backfed
through Q10. The current API does not yet expose `effective` or `backfeed`;
do not treat a requested-on port as a measured 5 V output.

`5v_usb_hub` is the switched child of `vdd_5v` on GPIO24 and boots off. Its
setter is parent-aware: enabling the hub enables `vdd_5v` first when needed;
disabling the hub leaves `vdd_5v` unchanged; disabling `vdd_5v` disables the hub
first; enabling `vdd_5v` alone does not enable the hub. `5v_usb_hub` appears in
status, `/api/v1/power`, and the persistent-config catalog, but it has no
INA226/ADC channel.

## ADC (Current And Voltage Monitor)

```sh
curl $BOARD_URL/api/v1/adc/read                    # all channels
curl $BOARD_URL/api/v1/adc/read?channel=5v_out      # single channel
```

The endpoint returns eight readings in fixed order: the four INA226 current
channels `5v_out`, `12v_out`, `20v_out`, and `vdd_5v`, followed by
`adc0`, `adc1`, `adc2`, and `adc3`. The other power outputs `5v_usb_hub` and
`5v_otg` have no INA226 and no ADC descriptor; `vdd_5v` is the parent total
including the hub branch, so clients must not synthesize a hub or OTG current
reading. Each INA226
reading carries `name`, `signal`, `power_enabled`, `raw`, `mv`, `current_ua`,
`voltage_valid`, `voltage_uv`, `sensor_value`, and `unit`.

ADC readings carry `name`, `signal`, `role`, `raw`, `mv`, `sensor_channel`,
`unit`, and `sensor_value`. `adc0` has `role:"pd_input"` and a canonical
`sensor_value` reconstructed by Device Tree from the R46/R47 divider as
source-side DC20V (`115/15` scaling, done in firmware with 64-bit arithmetic).
Its `raw` and `mv` remain pad-side diagnostics. `adc1` through `adc3` have
`role:"native"` and their canonical value is the pad voltage. Clients must not
apply `115/15` themselves or use `raw`/`mv` as a rescaled source measurement.
ADC0 accuracy is uncalibrated and approximately 2%; do not claim better
precision without per-board calibration.

## Switch Routes

```sh
curl $BOARD_URL/api/v1/switch                       # list all
curl $BOARD_URL/api/v1/switch/sd                    # read one
curl -X PUT $BOARD_URL/api/v1/switch/sd -d '{"route":"usb-reader"}'
curl -X PUT $BOARD_URL/api/v1/switch/tf_wp -d '{"route":"protected"}'
```

`{name}` is `sd` or `tf_wp`. Valid routes: `sd` → `target`/`usb-reader`;
`tf_wp` → `writable`/`protected`. x1.2 has no `switch/usb`; GET or PUT on
`/api/v1/switch/usb` returns HTTP 404. `vdd_5v` is an independent power output
from switch routing, and `5v_usb_hub` is its power child.

## GPIO

`{identifier}` accepts `GPxx`, raw pin number, or board note (e.g. `CON_MAS`).

```sh
curl $BOARD_URL/api/v1/gpio                         # list all
curl $BOARD_URL/api/v1/gpio/GP13                    # read one
curl -X PUT $BOARD_URL/api/v1/gpio/GP13 -d '{"direction":"output","value":1}'
curl -X PUT $BOARD_URL/api/v1/gpio/GP13 -d '{"direction":"input"}'
```

`GET /api/v1/status`, the `GET /api/v1/gpio` list response, and WebSocket
status snapshots carry this top-level connector metadata:

```json
{
  "gpio_layout_groups": [
    {"name": "J8", "edge": "right", "order": 0},
    {"name": "J13", "edge": "right", "order": 1}
  ]
}
```

`name` matches each GPIO's `layoutGroup`; `edge` is the board-edge placement
hint, and `order` defines connector order independently of GPIO array order.
For mapped pins, `layoutLabel`, `layoutRow`, and `layoutColumn` describe the
component-side top view, with `row=0` at the top and `column=0` on the left.
The confirmed coordinates are J8 GP12 `(9,0)`, GP13 `(8,0)`, GP14 `(7,0)`,
GP15 `(6,0)`, GP16 `(5,0)`, GP29/ADC3 `(4,0)`, GP5 `(4,1)`, GP6 `(5,1)`;
and J13 GP9 `(0,0)`, GP8 `(0,1)`, GP7 `(1,0)`. GP5/GP6 are ordinary direct
RP2354 GPIOs, default input/no-pulls, not switch controls. GP17 and GP18 are
absent.
Clients must keep any future firmware-described group and treat an unknown `edge`
value as no edge-alignment hint rather than dropping its GPIOs. These GPIO
arrays are the current 11-entry firmware safe-GPIO allowlist, not a complete
connector pinout or a mating-face diagram. Ten entries are output-capable;
GP29 is input-only.

## Reboot

### `POST /api/v1/reboot`

Request a normal cold restart. The MCU performs it 250 ms after the response,
and this path never requests ROM BOOTSEL. The planned-reboot state is prepared
before the response is sent, so a successful response guarantees that, absent a
physical BOOTSEL hold across the reset, the next boot returns to normal firmware
instead of the ROM USB boot path.

```sh
curl -X POST $BOARD_URL/api/v1/reboot
```

Each of `5v_out`, `12v_out`, `20v_out`, `vdd_5v`, `5v_usb_hub`, and `5v_otg` returns to
its inactive boot state unless a saved v1 configuration snapshot restores that
rail. `vdd_5v` is the parent and `5v_usb_hub` is its child, which also boots off.
Re-apply rail and PD setup after the board returns.

The supported mutation method is `POST`. `GET`, `PUT`, and `DELETE` return
HTTP 405 with `error.code=method_not_allowed` and do not schedule a reset.
Repeating `POST` while a restart is already pending returns HTTP 409 with
`error.code=reboot_pending`; it does not schedule a second or earlier reset.

## Bootloader

### `POST /api/v1/bootloader`

Enter ROM BOOTSEL mode. This is not a normal restart and is not an alias for
`POST /api/v1/reboot`. The MCU resets 250 ms after the response.

```sh
curl -X POST $BOARD_URL/api/v1/bootloader
```

Flash only the combined `radxa-linkr-debugger-rp2350.uf2` (MCUboot plus
application). Never use the application-only `zephyr.uf2` for ROM BOOTSEL; it
does not include MCUboot and can leave the board unable to start normally.

## Tasks (firmware catalog and task-v2 storage)

The firmware owns two separate task sources. The immutable built-in catalog at
`GET /api/v1/tasks/catalog` contains the MASKROM/EDL recipes and is never
stored in the four task slots. The task-v2 store holds up to four operator
tasks as canonical `linkr-test.v1` source plus `linkr-task.v1` deployment IR.
Firmware validates and stores tasks; it does not execute, replay, or seed them
at boot. The CLI and Web UI fetch the IR and dispatch each request as an
ordinary power, GPIO, or switch operation.

### Built-in catalog

`GET /api/v1/tasks/catalog` returns the firmware-owned immutable catalog. The
current catalog exposes six MASKROM/EDL entries, one per mode and
`5v_out`/`12v_out`/`20v_out`. Catalog entries have no slot, store revision, or
source file and do not consume task-v2 slots. A host must fail closed for a
requested `builtin/...` ID when the catalog is unavailable; it must never fall
back to a stored task with the same ID.

### Task-v2 storage model

- One Settings NVS partition: **128 KiB**, `0x1B0000..0x1D0000`, holding the
  config snapshot and all task-v2 manifest/IR/source keys. It remains below the
  2 MiB internal flash boundary. The size is deliberately bounded so the first
  NVS format and later garbage collection stay inside the 5 s watchdog window.
  Pre-release 320 KiB development layouts are not migrated; use a full-chip
  erase/reflash when moving such a board to this layout.
- Four task slots. Exact canonical source is at most **4000 bytes** per
  board-backed task; full-set IR and one-task IR sections are also at most
  **4000 bytes**. A mutation body is at most **12288 bytes**.
- Task IDs are 1..63 UTF-8 bytes and cannot contain whitespace, control bytes,
  DEL, or `#`. IDs are opaque identifiers; clients must not derive hardware
  behavior from their text.
- Each slot has two source banks and two full-set IR banks. The manifest holds
  the active banks, `task_store_id` UUID, uint64 revision, catalog fingerprint,
  full-IR digest, and per-slot source/IR digests.
- A mutation writes inactive banks, reads them back, verifies the digests, then
  commits the manifest. The manifest write is the only linearization point, so
  a power loss leaves the previous manifest active. Revision never wraps.
- `task_store_id` is created with the first manifest and is stable across
  clear and normal reboot; only factory erase changes it.
- The store is task-v2 only. It does not read or migrate the removed
  `linkr/task/tasks` Settings blob. Legacy task blob writes and per-ID delete
  routes are not part of this contract and are not aliased.

Exact source bytes are authoritative. A board task with `source_state:
"present"` can be reopened losslessly from the board. An IR-only task, an
imported task whose IR changed, and a built-in have no lossless workflow
source. The Web UI may additionally keep source larger than 4000 bytes in its
local workflow library and deploy only the IR; that source is not on the board
and is not available to a fresh browser.

### `GET /api/v1/tasks`

Returns metadata only. It does not return the stored blob. The response has
`schema`, `ok`, `command:"tasks"`, `action:"list"`, `task_store_id` as UUID
text, `revision` as a decimal string, `catalog_fingerprint` as 64 lowercase
hex characters, a `storage` object, `full_ir_bytes`, `full_ir_digest`, and a
`tasks` array:

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": true,
  "command": "tasks",
  "action": "list",
  "task_store_id": "12345678-1234-4678-9abc-123456789abc",
  "revision": "7",
  "catalog_fingerprint": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "storage": {
    "total_bytes": 131072,
    "used_bytes": 5532,
    "free_bytes": 125540,
    "task_bytes": 680,
    "reserve_bytes": 8192,
    "measured": true,
    "save_allowed": true
  },
  "full_ir_bytes": 320,
  "full_ir_digest": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "tasks": [
    {
      "slot": 0,
      "id": "power-cycle",
      "request_count": 3,
      "source_state": "present",
      "source_bytes": 180,
      "source_digest": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      "ir_bytes": 240,
      "task_ir_digest": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    }
  ]
}
```

`storage` is measured from the shared Settings NVS. `used_bytes + free_bytes ==
total_bytes`; `used_bytes` includes NVS metadata and the garbage-collection
reserve, while `task_bytes` is the logical manifest/source/IR payload. When
`measured` is false, firmware could not read NVS statistics and `save_allowed`
is advisory. A healthy task save/import additionally requires
`free_bytes >= reserve_bytes + required_new_bytes`; otherwise it returns HTTP
507 `storage_full` before writing. `delete` and `clear` remain allowed so they
can free space.

`tasks[]` contains only occupied slots. A present source has nonzero
`source_bytes` and a SHA-256 `source_digest`. An IR-only task reports
`source_state:"absent"`, `source_bytes:0`, and `source_digest:null`. Do not
fabricate a source digest for a built-in or IR-only task. `task_ir_digest` is
the SHA-256 of a standalone blob made from exact ASCII
`# linkr-task.v1\n` plus the task's exact section bytes in the active full IR,
from `# task <id>\n` through that task's final request LF.

### `GET /api/v1/tasks/ir`

Returns the exact active full-set `linkr-task.v1` bytes with content type
`application/vnd.radxa.linkr-task-ir.v1`. The response body length and SHA-256
must match `full_ir_bytes` and `full_ir_digest` from `GET /api/v1/tasks`.
An empty store returns an empty body. The raw response is owned until the HTTP
transaction completes.

### `GET /api/v1/tasks/slots/{0..3}/source`

Returns the exact canonical source bytes for that occupied slot with content
type `application/vnd.radxa.linkr-test.v1+ndjson`. The body SHA-256 must match
`source_digest`. A slot with no lossless source returns HTTP 404
`task_source_absent`; a slot that is not present returns the same error.

Clients should read the metadata, fetch the raw bytes, verify the advertised
digest, and retry the metadata read once if the revision/digest changed.

### `POST /api/v1/tasks/mutations`

All task-v2 writes use one JSON mutation endpoint. The request must carry
`Content-Type: application/json`. The body is a UTF-8 JSON object with the
string fields `schema` (`radxa-linkr-debugger.v1`), `action`
(`upsert`/`delete`/`clear`/`import`), `expected_revision`, and
`catalog_fingerprint`, plus the action-scoped optional fields `id`,
`source_base64`, and `ir_base64`. Field order is stable.

`source_base64` and `ir_base64` are strict canonical RFC 4648 base64 strings
(standard alphabet, exact zero-pad-bit padding, no JSON escapes or
whitespace) carrying the exact source/IR bytes; firmware rejects any other
encoding. The whole body is at most **12288 bytes**, which holds both
4000-byte payloads (5336 base64 characters each) plus the fixed envelope
keys.

- **Upsert** requires a valid ID and a standalone one-task `linkr-task.v1` IR
  whose task ID matches. Source is optional. Omitting source makes that slot
  IR-only and clears any previous source link; it does not preserve old source
  bytes. The Web client includes source only when it is at most 4000 bytes.
  Firmware validates source UTF-8, length, and control-byte policy; it validates
  IR syntax and matching ID. Firmware computes and stores both digests; it does
  not compile workflows.
- **Delete** requires the ID only and removes that slot.
- **Clear** has no payload and removes all slots. It does not change
  `task_store_id`.
- **Full-IR import** has only full-set IR and replaces all slots. A source link
  is retained only when the same task ID still exists with an unchanged task-IR
  digest; changed or removed tasks lose their source. An empty import is
  invalid; use clear.

Every mutation checks `expected_revision` and `expected_catalog_fingerprint`
under the store/flash-owner mutex. A conflict returns HTTP 409 and writes
nothing. Inactive source/IR banks are written and read-back verified before the
manifest commit.

Successful mutations return the standard envelope with `action` set to
`upsert`, `delete`, `clear`, or `import`, plus the new `task_store_id`,
decimal-string `revision`, `catalog_fingerprint`, `storage`, and a `task` object
for the affected slot (or `null` for clear/delete). The task object uses the
same fields as a `tasks[]` entry.

Stable error statuses:

- `400`: `invalid_mutation`, `invalid_task`, `body_too_large`
- `404`: `task_source_absent`
- `409`: `revision_conflict`, `catalog_conflict`, `busy`
- `507`: `storage_full` (NVS write failure or all four task slots occupied)
- `500`: `readback_failed`, `storage_error`

A manifest or data-key readback failure marks the task backend unavailable; it
is not auto-repaired, and `clear` is rejected in that state. Recover with a
full-chip erase/reflash; this unpublished layout has no on-device repair path.

`revision_conflict` and `catalog_conflict` include the current revision and
catalog fingerprint in the error response.

### Task write surface

Task writes use only `POST /api/v1/tasks/mutations`. There is no task blob
upload route and no per-ID write/delete route; unsupported methods return HTTP
405. The only task-clearing operation is mutation action `clear`. The only
per-task removal is action `delete`. Read routes accept GET only, and the
mutation route accepts POST only.

### Client-side run

The CLI and Web UI read metadata plus `/api/v1/tasks/ir`, select one task by
ID, and dispatch each record in order through the public HTTP API. Every record
is a `PUT` under `/api/v1/power/`, `/api/v1/gpio/`, or `/api/v1/switch/`.
`wait_ms` is applied client-side after a successful request and is never sent
to the board. The first failure stops the run; records after the failure are
never dispatched. A built-in uses the catalog's own cleanup sequence; a stored
task has no inferred cleanup.

The CLI `task store` command compiles a workflow source or accepts one-task IR
but sends no source payload, so it creates an IR-only board task. Use the Web
workflow editor/API source-bearing upsert when board-backed lossless source is
required. `GET /tasks/slots/{slot}/source` and CLI `task source` fail with
`task_source_absent` for IR-only tasks.

## Watchdog

### `GET /api/v1/watchdog`

Watchdog status. Fields: `supported`, `automatic`, `healthy`, `armed`, `timeout_ms`, `bootloader_on_timeout`, `fault_injection_available`, `fault_injection_armed`, `failing_service`. Fault-injection flags are always reported but the HIL endpoint below exists only in the fault overlay build.

### `GET /api/v1/watchdog/fault`

HIL-only. Returns `action=fault_injection`, `available`, and `armed`.

### `POST /api/v1/watchdog/fault`

HIL-only. Arms the bounded watchdog fault injection. Before `POST /api/v1/ota/test` has set the retained marker, firmware returns HTTP 409 with `error.code=fault_injection_rejected`; if the hook is already armed, it returns HTTP 409 with `error.code=already_armed`; while OTA confirm holds its marker-clear transaction, it returns HTTP 409 with `error.code=confirm_in_progress`. Arming stops the watchdog feed and auto-disarms after 10 seconds.

### `DELETE /api/v1/watchdog/fault`

HIL-only. Disarms the fault injection before its deadline.

## OTA Firmware Update

### `GET /api/v1/ota`

OTA status. Fields: `state` (`idle`/`uploading`/`verified`/`pending_test`/`rebooting`/`failed`), `expected_size`, `written_size`, `max_size`, `swap_type`, `current_image_confirmed`, and `test_marker_present`. The marker is cleared after manual or watchdog-gated confirmation. After an MCUboot rollback it remains set briefly while firmware observes the already-confirmed running image, then is cleared so the board can return to a clean idle state.

### `POST /api/v1/ota/upload`

Upload MCUboot-format firmware binary.

Required headers:
- `Content-Type: application/octet-stream`
- `X-Linkr-Ota-Size: <byte_size>`
- `X-Linkr-Ota-Sha256: <hex_sha256>` (64 hex characters)

```sh
FIRMWARE=path/to/firmware.bin
SIZE=$(stat -c%s "$FIRMWARE" 2>/dev/null || stat -f%z "$FIRMWARE")
SHA256=$(sha256sum "$FIRMWARE" 2>/dev/null | cut -d' ' -f1 || shasum -a 256 "$FIRMWARE" | cut -d' ' -f1)
curl -X POST $BOARD_URL/api/v1/ota/upload \
  -H "Content-Type: application/octet-stream" \
  -H "X-Linkr-Ota-Size: $SIZE" \
  -H "X-Linkr-Ota-Sha256: $SHA256" \
  --data-binary "@$FIRMWARE"
```

### `POST /api/v1/ota/test`

Request test boot of verified image. Board reboots after ~750ms delay.

### `POST /api/v1/ota/confirm`

Confirm running image immediately.

## Live Sessions (WebSocket)

### `POST /api/v1/live-sessions`

Create a WebSocket session. Returns `ws_url` for the assigned slot.

```sh
curl -X POST $BOARD_URL/api/v1/live-sessions
```

Response: `session_id`, `ws_url` (`ws://<board-ip>/api/v1/ws/<slot>`), `connected`.

### `GET /api/v1/live-sessions/{id}`

Lookup session by ID.

### `DELETE /api/v1/live-sessions/{id}`

Delete session. Returns 409 if still connected.

## Logic Analyzer

### `GET /api/v1/logic-analyzer`

Current state: `idle`, `armed`, `capturing`, `done`, `error`.

### `POST /api/v1/logic-analyzer`

Arm capture.

```json
{
  "selected_pins": [13, 15],
  "sample_rate_hz": 1000000,
  "pre_samples": 0,
  "post_samples": 512,
  "trigger": "none",
  "trigger_pin": 0
}
```

Constraints: 100,000–125,000,000 Hz rate; max 512 total samples; max 16 channels from safe allowlist (GP7–GP9, GP10–GP20, GP29); `pre_samples > 0` requires edge trigger at ≤25 MHz.

Response: `requestedSampleRateHz`, `actualSampleRateHz`, `samplePeriodPs`, `backend`.

### `GET /api/v1/logic-analyzer/capture`

Retrieve captured data after state is `done`. Returns `sampleCount`, `triggerIndex`, `config`, and `samples` array.

### `DELETE /api/v1/logic-analyzer`

Release capture resources.

## WebSocket Protocol

Connect to `ws://<board-ip>/api/v1/ws/{0|1|2|3}` (slot URL from live-sessions).

Up to 4 concurrent clients.

### Client → Server Messages

**Subscribe** (start receiving telemetry):
```json
{"type": "subscribe", "topic": "live", "rate_hz": 60, "batch_size": 1}
```
- `rate_hz`: 1–250 (default 10); values above 250 return error code `invalid_rate`
- `batch_size`: 1–18 (>1 enables batch mode)

**J4 serial subscribe**:
```json
{"type":"subscribe","topic":"serial.j4","id":"terminal-j4"}
```

`serial.j4`, live ADC, and sigrok are mutually exclusive modes within one WS
slot. Open a separate live session when the page needs terminal and telemetry
at the same time. Binary payloads received in serial mode are admitted only
when J4 is ready. The server first emits `serial_write_accepted`, then emits a
matching `serial_write_complete` only after UART physical-wire completion (or
with `disposition:"canceled"` if the runtime cancels it). A queue-full,
paused, reconfiguring, faulted, or oversized write receives an explicit JSON
error and is not acknowledged as accepted.

`serial_state` identifies the automatic terminal responder. Its
`terminal_responder` field is a boolean for the receiving slot and `responder`
is the selected global descriptor. Only that slot answers automatic DSR/DA/CPR
queries; other slots remain explicit-input observers, not read-only clients.

**Unsubscribe**:
```json
{"type": "unsubscribe"}
```

**Commands**:
```json
{"type": "command", "command": "power_set", "id": "1", "output": "12v_out", "state": "on"}
```

Available commands: `power_set`, `switch_route`, `gpio_set`, `reboot`, `bootloader`, `capture_arm`, `capture_trigger`, `capture_stop`, `capture_cancel`. Saved task sequences are managed over HTTP only.

`reboot` requests the normal cold restart described under
[Reboot](#reboot); it is not a `bootloader` alias. Firmware emits the
`reboot`/`ok` result frame before the 250 ms restart delay begins. `bootloader`
remains the explicit ROM BOOTSEL command.

Power capture requires an explicit host-stream protocol mode:

```json
{"type":"command","command":"capture_arm","id":"capture-1","mode":"host-stream-v1","trigger":"current","output":"5v_out","threshold_ua":500000,"rate_hz":250}
```

Compare `mode` with the `power_capture_protocol` advertised by status before
arming. This prevents mixed firmware/Web versions from waiting indefinitely.
`rate_hz` accepts 1–250 and defaults to 100 when omitted; values above 250
return error code `invalid_rate`.

GPIO command (including direct recovery-line control):

```json
{"type":"command","command":"gpio_set","id":"2","gpio":"CON_MAS","direction":"output","value":1}
```

An output attempt on an input-only GPIO, such as ADC3-owned `GP29`, returns
HTTP 403 from `/api/v1/gpio/<id>` and an error frame from `gpio_set`. Both
transports use `error.code` `input_only` and an `error.message` containing
`input-only`. Other GPIO configuration failures use `configure_failed`.

There is no dedicated MASKROM/EDL WebSocket command and no generic task-run
WebSocket command. Built-ins come from the firmware catalog at
`GET /api/v1/tasks/catalog`; clients execute them by composing the existing
ordinary `gpio_set` and `power_set` commands through the generic task runner.

### Server → Client Messages

**Status snapshot** (pushed on state change):
```json
{"type": "snapshot", "topic": "status", "power_capture_protocol": "host-stream-v1", "sequence": 1, "power_outputs": [], "switches": {}, "watchdog": {}, "gpio_layout_groups": [{"name":"J8","edge":"right","order":0},{"name":"J13","edge":"right","order":1}], "gpios": [], "board_monitoring": {}}
```

**ADC telemetry** (at subscribed rate):
```json
{"type": "telemetry", "topic": "adc", "sequence": 1, "uptime_us": 12345, "readings": []}
```

**Batch telemetry** (when `batch_size > 1`):
```json
{"type": "telemetry-batch", "topic": "adc", "channels": [], "samples": []}
```

Each WebSocket client is backed by a 256-sample telemetry ring. Batch messages
include `dropped_samples`; a non-zero value means the client fell behind and
must mark a long-running host archive as incomplete. Long recordings should
persist these telemetry batches continuously instead of accumulating them in
debugger capture RAM.

**Command result**:
```json
{"type": "result", "command": "power_set", "id": "1", "ok": true}
```

J4 serial mode additionally emits `serial_state`, `serial_write_accepted`,
`serial_write_complete`, and `serial_overrun`. `serial_overrun.dropped_bytes`
is the increment since the preceding notice, while `total_dropped_bytes` is the
sink's cumulative counter. Binary downlink frames contain raw UART bytes.

**Capture event**:
```json
{"type":"capture_triggered","capture_id":1,"device_t_mono_us":123456,"sample_sequence":42,"dropped_samples":0}
```

Firmware keeps only trigger state and metadata. Raw samples continue through
ADC telemetry and must be buffered/persisted by the host. `dropped_samples` is
the owning telemetry cursor's cumulative overrun count since subscription.
`capture_stop` releases a triggered capture; `capture_cancel` disarms it.

## Error Codes

| HTTP Status | Code | Meaning |
|---|---|---|
| 400 | `missing_power_output`, `invalid_state`, `invalid_config`, `invalid_route` | Bad request |
| 403 | `power_output_locked`, `not_allowed`, `input_only` | Forbidden; `input_only` identifies firmware-owned ADC3/GP29 |
| 404 | `unknown_power_output`, `not_found`, `unknown_channel` | Not found |
| 405 | `method_not_allowed` | Method not allowed |
| 409 | `already_armed`, `no_verified_image`, `upload_in_progress` | Conflict |
| 413 | `body_too_large`, `image_too_large` | Payload too large |
| 500 | `set_failed`, `read_failed`, `arm_failed`, `confirm_failed` | Internal error |
| 503 | `no_slots_available` | No WebSocket slots |

## Related

- [CLI Reference](cli.md)
- [Web UI Guide](webui.md)
- [Logic Analyzer](logic-analyzer.md)
- [Power Analyzer](power-analyzer.md)
- [OTA Firmware Update](ota.md)
