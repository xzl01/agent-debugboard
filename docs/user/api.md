[中文](api.zh-CN.md)

# HTTP API Reference

Base URL: `http://172.29.203.1` (USB NCM interface, port 80)

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

## Status

```sh
curl http://172.29.203.1/api/v1/status
```

Returns everything in one shot: `project`, `build` (including `profile` and `image_version`), `mcu`, `usb`, `power_inputs`, `power_outputs`, `switches`, `adc_channels`, `watchdog`, `board_monitoring`, `gpio_layout_groups`, and `gpios`.

The encoded status body must remain strictly smaller than the dedicated
6656-byte firmware buffer. This is a protocol size gate for additions to the
status snapshot, not permission for clients to assume a fixed response length.

`power_outputs` is firmware-advertised and dynamic. On x1.2 it contains the
five controllable entries `12v_out`, `5v_out`, `20v_out`, `vdd_5v`, and
`5v_usb_hub`. `5v_usb_hub` is the `vdd_5v` child branch, has no INA226/ADC
descriptor, and does not add a status field outside the existing power-output
shape.

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
curl -fsS http://172.29.203.1/api/v1/serial/j4
```

### `PUT /api/v1/serial/j4/config`

Admits an asynchronous compare-and-swap line-coding update. Read the current
`generation`, then send the complete tuple and that generation:

```sh
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"baud":115200,"data_bits":8,"parity":"none","stop_bits":1,"flow_control":"none","expected_generation":1}' \
  http://172.29.203.1/api/v1/serial/j4/config
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
  or noVNC semantics. For example: `nc 172.29.203.1 5555`.
- USB CDC ACM4 is the direct USB fallback for the same logical J4 stream. USB
  tty numbering is host-dependent; identify the interface rather than assuming
  a `/dev/ttyACM*` number.

Legacy desktop-local serial transports are retired and are not product paths.
Port 5173 is only a Vite development preview; the tested embedded
UI is served by the board on port 80.

## Power

```sh
curl http://172.29.203.1/api/v1/power                    # list all
curl http://172.29.203.1/api/v1/power/12v_out             # read one
curl -X PUT http://172.29.203.1/api/v1/power/12v_out -d '{"state":"on"}'  # set
```

`{name}` is `12v_out`, `5v_out`, `20v_out`, `vdd_5v`, or `5v_usb_hub`. Body: `{"state": "on"}` or `{"state": "off"}`.

`5v_usb_hub` is the switched child of `vdd_5v` on GPIO24 and boots off. Its
setter is parent-aware: enabling the hub enables `vdd_5v` first when needed;
disabling the hub leaves `vdd_5v` unchanged; disabling `vdd_5v` disables the hub
first; enabling `vdd_5v` alone does not enable the hub. `5v_usb_hub` appears in
status, `/api/v1/power`, and the persistent-config catalog, but it has no
INA226/ADC channel.

## ADC (Current And Voltage Monitor)

```sh
curl http://172.29.203.1/api/v1/adc/read                    # all channels
curl http://172.29.203.1/api/v1/adc/read?channel=5v_out      # single channel
```

The endpoint returns eight readings in fixed order: the four INA226 current
channels `5v_out`, `12v_out`, `20v_out`, and `vdd_5v`, followed by the four
monitor-only pad-voltage channels `adc0`, `adc1`, `adc2`, and `adc3`. The fifth
power output `5v_usb_hub` has no INA226 and no ADC descriptor; `vdd_5v` is the
parent total including the hub branch, so clients must not synthesize a hub
current reading. Each INA226
reading carries `name`, `signal`, `power_enabled`, `raw`, `mv`, `current_ua`,
`voltage_valid`, `voltage_uv`, `sensor_value`, and `unit`. Each ADC reading
carries `name`, `signal`, `raw`, `mv`, `sensor_channel`, `unit`, and
`sensor_value`; `adc0` also carries the firmware note
`PD input divider; ADC pad voltage`. Its scalar is the direct ADC0 pad voltage,
not a divider-reconstructed source voltage, so clients must not multiply it by
`115/15`.

## Switch Routes

```sh
curl http://172.29.203.1/api/v1/switch                       # list all
curl http://172.29.203.1/api/v1/switch/sd                    # read one
curl -X PUT http://172.29.203.1/api/v1/switch/sd -d '{"route":"usb-reader"}'
curl -X PUT http://172.29.203.1/api/v1/switch/tf_wp -d '{"route":"protected"}'
```

`{name}` is `sd` or `tf_wp`. Valid routes: `sd` → `target`/`usb-reader`;
`tf_wp` → `writable`/`protected`. x1.2 has no `switch/usb`; GET or PUT on
`/api/v1/switch/usb` returns HTTP 404. `vdd_5v` is an independent power output
from switch routing, and `5v_usb_hub` is its power child.

## GPIO

`{identifier}` accepts `GPxx`, raw pin number, or board note (e.g. `CON_MAS`).

```sh
curl http://172.29.203.1/api/v1/gpio                         # list all
curl http://172.29.203.1/api/v1/gpio/GP13                    # read one
curl -X PUT http://172.29.203.1/api/v1/gpio/GP13 -d '{"direction":"output","value":1}'
curl -X PUT http://172.29.203.1/api/v1/gpio/GP13 -d '{"direction":"input"}'
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
curl -X POST http://172.29.203.1/api/v1/reboot
```

Each of `5v_out`, `12v_out`, `20v_out`, `vdd_5v`, and `5v_usb_hub` returns to
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
curl -X POST http://172.29.203.1/api/v1/bootloader
```

Flash only the combined `radxa-linkr-debugger-rp2350.uf2` (MCUboot plus
application). Never use the application-only `zephyr.uf2` for ROM BOOTSEL; it
does not include MCUboot and can leave the board unable to start normally.

## Tasks (firmware catalog and saved request sequences)

The firmware API stores generic task blobs in flash; it does not execute,
replay, or auto-seed tasks. The firmware also owns the immutable built-in task
catalog at `GET /api/v1/tasks/catalog`; the CLI and Web UI consume that
catalog strictly and dispatch every selected task through the existing
public power, GPIO, and switch APIs. Hosts hold no recovery recipes of their
own. The current catalog exposes the six MASKROM/EDL entries; stored entries
remain on a separate path and never appear in `GET /api/v1/tasks/catalog`.

### `GET /api/v1/tasks/catalog`

Returns the firmware-owned immutable built-in catalog. Hosts validate the
response envelope (`schema`, `ok`, `command: "task"`, `action: "catalog"`,
`version: 1`) and reject unknown fields. Each catalog entry declares its id,
display name, the list of `PUT` records to dispatch against the allowlisted
`/api/v1/power`, `/api/v1/gpio`, and `/api/v1/switch` paths, and a
mandatory cleanup sequence. Hosts dispatch catalog entries through the same
generic task runner used for stored tasks.

```sh
curl -fsS http://172.29.203.1/api/v1/tasks/catalog
```

### Frozen Limits

| Limit | Value |
|---|---|
| Tasks per blob | 4 |
| Requests per task | 32 |
| Blob bytes | 4096 |
| Task id bytes | 31 |
| Task name bytes | 63 |
| Request line bytes | 256 |
| Path bytes | 96 |
| Body bytes | 192 |
| `wait_ms` range | 0 through 60000 |

### Storage Layout

Tasks are persisted in `Settings+NVS` under the key
`linkr/task/tasks`, and the blob always starts with the literal
`# linkr-task.v1`. Each task follows a `# task <id>` marker and one or more
NDJSON request records. A typical record is:

```json
{"method":"PUT","path":"/api/v1/power/12v_out","body":"{\"state\":\"off\"}","wait_ms":1000}
```

A second record in the same task would appear on its own NDJSON line:

```json
{"method":"PUT","path":"/api/v1/gpio/CON_MAS","body":"{\"direction\":\"input\"}","wait_ms":0}
```

The full file looks like:

```text
# linkr-task.v1
# task power-cycle
{"method":"PUT","path":"/api/v1/gpio/GP13","body":"{\"direction\":\"input\"}","wait_ms":0}
{"method":"PUT","path":"/api/v1/power/12v_out","body":"{\"state\":\"off\"}","wait_ms":1000}
```

Every record must use `method:"PUT"`, a JSON-string `body`, and a `path`
under one of `/api/v1/power/`, `/api/v1/gpio/`, or `/api/v1/switch/`.
`wait_ms` defaults to 0 when omitted and must be an integer from 0 through
60000. The blob, the marker text, the path allowlist, and the bounds above
are validated at parse time; an invalid record returns `invalid_blob`. Bytes
under `0x20` other than tab/LF/CR are rejected before storage so the GET
JSON encoder always reproduces the stored blob exactly.

Task IDs are nonempty UTF-8 strings of at most 31 bytes. Space, tab, CR,
LF, `#`, the reserved ID `catalog`, and the exact IDs `.` and `..` are
invalid; `PUT` rejects a blob containing them with HTTP 400 `invalid_blob`.
Other IDs containing dots, such as `.hidden` or `...`, remain valid.

Old development data (anything written under the previous task-store marker
or settings key) is intentionally invalidated by the new marker
`# linkr-task.v1` and the new key `linkr/task/tasks`. There is no migration
path and no read alias; a board that still holds old data simply shows the
empty task contract after upgrade.

### `GET /api/v1/tasks`

Returns the task state and the exact stored blob. An empty board returns
`tasks: []` and `blob: ""` with HTTP 200; the response is the same envelope
that every other endpoint uses:

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": true,
  "command": "task",
  "action": "list",
  "backend": { "available": true },
  "task_count": 1,
  "tasks": [
    { "id": "power-cycle", "name": "power-cycle", "request_count": 3 }
  ],
  "blob": "# linkr-task.v1\n# task power-cycle\n{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":\"{\\\"direction\\\":\\\"input\\\"}\",\"wait_ms\":0}\n{\"method\":\"PUT\",\"path\":\"/api/v1/power/12v_out\",\"body\":\"{\\\"state\\\":\\\"off\\\"}\",\"wait_ms\":1000}\n{\"method\":\"PUT\",\"path\":\"/api/v1/power/12v_out\",\"body\":\"{\\\"state\\\":\\\"on\\\"}\",\"wait_ms\":0}\n"
}
```

Stable fields: `schema`, `ok`, `command:"task"`, `action:"list"`, `backend`
({`available`}), `task_count` (numeric), `tasks[]` (`id`, `name`,
`request_count`), `blob` (string, exact bytes). `name` defaults to `id`
when the stored task did not set one. The response capacity is derived
from `2 * 4096` plus bounded envelope/summary overhead; a request that
exceeds the capacity returns `response_too_large` instead of a truncated
body. The GET handler does not acquire the capture or flash arbiter.

### `PUT /api/v1/tasks`

Replaces the stored blob. The body is the full `linkr-task.v1` text. A JSON
string literal is also accepted (`"..."`), in which case the server unescapes
standard escapes before validation. The endpoint requires the shared
mutation helper, so a concurrent capture or OTA session returns
`busy` (HTTP 409) without writing. A blob larger than 4096 bytes returns
`body_too_large` (HTTP 413). On success the response is:

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": true,
  "command": "task",
  "action": "store",
  "stored": true
}
```

### `DELETE /api/v1/tasks`

Clears the stored blob; live hardware is unchanged. Acquires the shared
mutation helper first and returns `busy` (HTTP 409) when capture or OTA
is in flight. Successful response:

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": true,
  "command": "task",
  "action": "clear",
  "cleared": true
}
```

### `DELETE /api/v1/tasks/{id}`

Deletes only the named stored task, preserving all other task sections
byte-for-byte and leaving live hardware unchanged. Deleting the last task
removes the backing key. Encode the ID as one UTF-8 percent-encoded URL
path component (for example `café` becomes `caf%C3%A9`, `/` becomes `%2F`,
and a literal `%` becomes `%25`). Decoding occurs once; decoded NUL (`%00`)
is rejected rather than truncating the ID. The exact decoded IDs `.` and
`..` are invalid, including `%2E` and `%2E%2E` spellings.

| HTTP status | Result |
| --- | --- |
| 200 | Task deleted; response shown below |
| 400 | Missing or invalid ID, invalid percent decoding, or decoded NUL; no task is deleted |
| 404 | `not_found`: no stored task matches the valid ID |
| 409 | `busy`: capture or OTA owns the shared mutation resource; no task is deleted |

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": true,
  "command": "task",
  "action": "delete",
  "id": "power-cycle",
  "deleted": true
}
```

### Method Handling

GET, PUT, and DELETE are the only supported methods; any other verb on
`/api/v1/tasks` returns `method_not_allowed` (HTTP 405). The endpoint never
replays stored requests; the firmware contract is store-only. Execution is the
client's job.

`/api/v1/tasks/{id}` accepts DELETE only; other methods return HTTP 405.

### Client-Side Run And Limits

The CLI and the Web UI fetch the blob via `GET /api/v1/tasks`, select one
task by id, and dispatch each record in order through the public HTTP API.
Each successful request is followed by a client-side `wait_ms` sleep when
the record carries one; `wait_ms` is task metadata only and is **not**
forwarded to the power, GPIO, or switch endpoints. The first failure stops
the run. Records past the failure are never dispatched, and the wire `body`
sent to the board never includes `wait_ms`.

The CLI and the page report the failure on their own human/JSON surface,
not on the shared HTTP envelope. The CLI JSON failure uses
`error.record_index` (1-based) and `error.path` (snake_case under the
`error` object) and exits nonzero on the `task` action. The CLI's
non-JSON failure writes `task record <n> path "<path>" failed: <message>`
to stderr. The Web page surfaces an internal result that carries
`failedIndex` (1-based) and `failedPath` (camelCase) plus the resolver
error string.

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
curl -X POST http://172.29.203.1/api/v1/ota/upload \
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
curl -X POST http://172.29.203.1/api/v1/live-sessions
```

Response: `session_id`, `ws_url` (`ws://172.29.203.1/api/v1/ws/<slot>`), `connected`.

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

Connect to `ws://172.29.203.1/api/v1/ws/{0|1|2|3}` (slot URL from live-sessions).

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
