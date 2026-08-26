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

Returns everything in one shot: `project`, `mcu`, `usb`, `power_inputs`, `power_outputs`, `switches`, `adc_channels`, `watchdog`, `board_monitoring`, `gpios`.

## Power

```sh
curl http://172.29.203.1/api/v1/power                    # list all
curl http://172.29.203.1/api/v1/power/12v_out             # read one
curl -X PUT http://172.29.203.1/api/v1/power/12v_out -d '{"state":"on"}'  # set
```

`{name}` is `12v_out`, `5v_out`, or `20v_out`. Body: `{"state": "on"}` or `{"state": "off"}`.

## ADC (Current Monitor)

```sh
curl http://172.29.203.1/api/v1/adc/read                    # all channels
curl http://172.29.203.1/api/v1/adc/read?channel=5v_out      # single channel
```

Each reading in the response carries: `name`, `signal`, `power_enabled`, `raw`, `mv`, `current_ua`, `sensor_value`, `unit`.

## Switch Routes

```sh
curl http://172.29.203.1/api/v1/switch                       # list all
curl http://172.29.203.1/api/v1/switch/sd                    # read one
curl -X PUT http://172.29.203.1/api/v1/switch/sd -d '{"route":"usb-reader"}'
curl -X PUT http://172.29.203.1/api/v1/switch/vin -d '{"route":"1.8v"}'
```

`{name}` is `sd`, `usb`, or `vin`. Valid routes: `sd` → `target`/`usb-reader`; `usb` → `pc`/`target`; `vin` → `1.8v`/`3.3v`.

For the `usb` switch, J12's upper port connects to the target and its lower
port holds the USB device. The `pc`/`target` route selects which host controls
that device.

## GPIO

`{identifier}` accepts `GPxx`, raw pin number, or board note (e.g. `CON_MAS`).

```sh
curl http://172.29.203.1/api/v1/gpio                         # list all
curl http://172.29.203.1/api/v1/gpio/GP13                    # read one
curl -X PUT http://172.29.203.1/api/v1/gpio/GP13 -d '{"direction":"output","value":1}'
curl -X PUT http://172.29.203.1/api/v1/gpio/GP13 -d '{"direction":"input"}'
```

## Bootloader

### `POST /api/v1/bootloader`

Enter ROM BOOTSEL mode. The MCU resets 250ms after the response.

```sh
curl -X POST http://172.29.203.1/api/v1/bootloader
```

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

### Method Handling

GET, PUT, and DELETE are the only supported methods; any other verb on
`/api/v1/tasks` returns `method_not_allowed` (HTTP 405). The endpoint never
replays stored requests; the firmware contract is store-only. Execution is the
client's job.

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

Watchdog status. Fields: `supported`, `automatic`, `healthy`, `armed`, `timeout_ms`, `bootloader_on_timeout`, `failing_service`.

## OTA Firmware Update

### `GET /api/v1/ota`

OTA status. Fields: `state` (`idle`/`uploading`/`verified`/`pending_test`/`rebooting`/`failed`), `expected_size`, `written_size`, `max_size`, `swap_type`, `current_image_confirmed`, and `test_marker_present`. The marker is cleared after manual or watchdog-gated confirmation and remains set when MCUboot rolls back a test image during a controlled run.

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
- `rate_hz`: 1–1000 (default 10)
- `batch_size`: 1–20 (>1 enables batch mode)

**Unsubscribe**:
```json
{"type": "unsubscribe"}
```

**Commands**:
```json
{"type": "command", "command": "power_set", "id": "1", "output": "12v_out", "state": "on"}
```

Available commands: `power_set`, `switch_route`, `gpio_set`, `bootloader`, `capture_arm`, `capture_trigger`, `capture_stop`, `capture_cancel`. Saved task sequences are managed over HTTP only.

Power capture requires an explicit host-stream protocol mode:

```json
{"type":"command","command":"capture_arm","id":"capture-1","mode":"host-stream-v1","trigger":"current","output":"5v_out","threshold_ua":500000,"rate_hz":100}
```

Compare `mode` with the `power_capture_protocol` advertised by status before
arming. This prevents mixed firmware/Web versions from waiting indefinitely.

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
{"type": "snapshot", "topic": "status", "power_capture_protocol": "host-stream-v1", "sequence": 1, "power_outputs": [], "switches": {}, "watchdog": {}, "gpios": [], "board_monitoring": {}}
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
