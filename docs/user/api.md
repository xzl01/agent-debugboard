[中文](api.zh-CN.md)

# HTTP API Reference

Base URL: `http://172.29.203.1` (USB NCM interface, port 80)

All JSON responses include `"schema": "radxa-linkr-debugger.v1"` and `Cache-Control: no-store`.

## Response Envelope

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": true,
  "command": "<command>",
  "error": { "code": "<code>", "message": "<message>" }
}
```

`error` is present only when `ok` is `false`.

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

## Watchdog

### `GET /api/v1/watchdog`

Watchdog status. Fields: `supported`, `automatic`, `healthy`, `armed`, `timeout_ms`, `bootloader_on_timeout`, `failing_service`.

## OTA Firmware Update

### `GET /api/v1/ota`

OTA status. Fields: `state` (`idle`/`uploading`/`verified`/`pending_test`/`rebooting`/`failed`), `expected_size`, `written_size`, `max_size`, `swap_type`, `current_image_confirmed`.

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

Available commands: `power_set`, `switch_route`, `gpio_set`, `bootloader`, `capture_arm`, `capture_trigger`, `capture_cancel`.

### Server → Client Messages

**Status snapshot** (pushed on state change):
```json
{"type": "snapshot", "topic": "status", "sequence": 1, "power_outputs": [...], "switches": {...}, "watchdog": {...}, "gpios": [...], "board_monitoring": {...}}
```

**ADC telemetry** (at subscribed rate):
```json
{"type": "telemetry", "topic": "adc", "sequence": 1, "uptime_us": 12345, "readings": [...]}
```

**Batch telemetry** (when `batch_size > 1`):
```json
{"type": "telemetry-batch", "topic": "adc", "channels": [...], "samples": [...]}
```

**Command result**:
```json
{"type": "result", "command": "power_set", "id": "1", "ok": true}
```

**Capture messages**: `capture_begin` → `capture_sample` (per sample) → `capture_complete`.

## Error Codes

| HTTP Status | Code | Meaning |
|---|---|---|
| 400 | `missing_power_output`, `invalid_state`, `invalid_config`, `invalid_route` | Bad request |
| 403 | `power_output_locked`, `not_allowed` | Forbidden |
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
