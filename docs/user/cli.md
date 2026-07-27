# CLI Reference

[中文](cli.zh-CN.md)

`radxa-linkr-debuggerctl` talks to the board over USB NCM. The board runs a DHCPv4 server, so the host gets a compatible address automatically — no configuration needed. The default device URL is `http://172.29.203.1`; pass `--url` only to override it.

```sh
# Check connection and board health
radxa-linkr-debuggerctl doctor
radxa-linkr-debuggerctl status
```

Running without a subcommand starts the [interactive TUI](tui.md). All subcommands below work in CLI mode.

## JSON Output

For scripts and automation, add `--json` to get structured output. Every response follows the same envelope:

Success response:

```json
{"schema": "radxa-linkr-debugger.v1", "ok": true, "command": "status"}
```

Failure response (`error` is present only when `ok` is `false`):

```json
{"schema": "radxa-linkr-debugger.v1", "ok": false, "command": "status", "error": {"code": "request_failed", "message": "..."}}
```

```sh
radxa-linkr-debuggerctl --json status
radxa-linkr-debuggerctl --json power list
radxa-linkr-debuggerctl --json adc read
radxa-linkr-debuggerctl --json gpio list
```

## Power

Three rails are controllable: `12v_out`, `5v_out`, `20v_out`. The board-internal VDD_5V rail is intentionally hidden from CLI and TUI.

```sh
radxa-linkr-debuggerctl power list
radxa-linkr-debuggerctl power set 12v_out on
radxa-linkr-debuggerctl power set 5v_out off
radxa-linkr-debuggerctl power set 20v_out on
```

## ADC (Current Monitor)

Each power rail has a current-sense channel. Default output is concise (`5v_out=0.540000A`); add `-v` for raw ADC fields like `signal` and `mv`.

```sh
radxa-linkr-debuggerctl adc read
radxa-linkr-debuggerctl adc read 5v_out
radxa-linkr-debuggerctl adc read 12v_out
```

### Recording

`adc record` opens a websocket session and writes telemetry to disk:

```sh
radxa-linkr-debuggerctl adc record /tmp/adc.ndjson 1000 --rate-hz 250
radxa-linkr-debuggerctl adc record /tmp/adc.csv 1000 --rate-hz 250
```

- Output format is determined by file extension: `.ndjson` or `.csv`
- Default subscription rate is 1000 Hz; use `--rate-hz HZ` for lower rates
- Requests above 100 Hz use batch JSON on the wire; the recorder expands each device sample into its own row
- Each output record includes host receive timestamps plus `metadata.requested_rate_hz`
- Device timing is preserved under `metadata.device_timing`
- Ring overruns are reported as `metadata.dropped_samples` on the first affected row

### JSON ADC output

`--json adc read` carries the raw diagnostic chain: `raw`, `mv`, `current_ua`, `sensor_value`, plus the power-output state. The host CLI presents firmware values directly without host-side calibration.

## Switch Routes

Three mux switches control physical signal routing:

| Switch | Routes between | Values |
|--------|---------------|--------|
| `sd` | TF/SD card path | `target`, `usb-reader` |
| `usb` | J12 lower-port USB device routed to PC or target | `pc`, `target` |
| `vin` | CH347 VIO voltage | `3.3v`, `1.8v` |

```sh
radxa-linkr-debuggerctl switch list
radxa-linkr-debuggerctl switch get sd
radxa-linkr-debuggerctl switch route sd usb-reader
radxa-linkr-debuggerctl switch route usb target --confirm
radxa-linkr-debuggerctl switch route usb pc --confirm
```

USB and VIN routes require `--confirm` because they have visible side effects. VIN defaults to 3.3V at boot. Switching to 1.8V is an expert operation — confirm the target supports 1.8V signaling first and connect physical VIO measurement equipment.

For `switch usb`, J12's upper port connects to the target and the lower port
holds the USB device being switched. Route `pc` connects that device to the PC
at J15; route `target` connects it to the target through J12's upper port.

## GPIO

Safe GPIOs accept three name formats: `GP13` (canonical), `13` (raw pin), or `CON_MAS` (board note). The CLI shows both `GPxx` and the note.

```sh
radxa-linkr-debuggerctl gpio list
radxa-linkr-debuggerctl gpio set GP13 1
radxa-linkr-debuggerctl gpio set CON_MAS 1
radxa-linkr-debuggerctl gpio input GP13
```

## Automated Test Scripts

`test run` executes a `linkr-test.v1` NDJSON script. Use `--serial` to select the target serial port. `--output` writes the full result to disk; `.json`, `.csv`, and `.ndjson` extensions select the report format, while other extensions default to JSON.

```sh
radxa-linkr-debuggerctl test run startup.ndjson --serial /dev/tty.usbserial-1234
radxa-linkr-debuggerctl test run startup.ndjson --output startup-report.json
```

A `gpio_assert` step evaluates the `direction` and `value` in its `params` directly. Direction accepts only `input`/`output`, and value accepts only `0`/`1`. A Ctrl+C interruption marks the report as `aborted`.

## Target Recovery Modes

Use the dedicated recovery command instead of manually driving `CON_MAS`. The
firmware performs a complete power cycle and always releases `CON_MAS` back to
input when the sequence completes or fails.

```sh
# Qualcomm EDL samples an active-high recovery signal
radxa-linkr-debuggerctl recovery enter qualcomm-edl 5v_out --confirm

# Rockchip MASKROM samples an active-low recovery signal
radxa-linkr-debuggerctl recovery enter rockchip-maskrom 5v_out --confirm
```

The final argument must be a user-facing target rail: `5v_out`, `12v_out`, or
`20v_out`. `--confirm` is mandatory because the target device loses power and
unsaved state. This is separate from `bootloader`, which enters the debugger
MCU's RP2350 BOOTSEL mode.

## Watchdog

```sh
radxa-linkr-debuggerctl watchdog status
```

Firmware owns the hardware watchdog — the host cannot feed or control it. The watchdog stays fed only while these three services report healthy liveness:

- Core firmware loop
- HTTP/API service
- CDC ACM cmdline fallback

If any of them wedge or stop reporting, firmware stops feeding, the MCU resets, and the next boot enters ROM BOOTSEL via the recovery marker. WebSocket silence, subscription timeout, and session expiration do **not** trigger a watchdog failure.

## OTA Firmware Update

Upload, test, and confirm firmware updates over USB. Only MCUboot-format `.bin` files are accepted — do not upload `.uf2` or `.elf`.

```sh
radxa-linkr-debuggerctl ota status
radxa-linkr-debuggerctl ota upload radxa-linkr-debugger-rp2350-ota.bin
radxa-linkr-debuggerctl ota test
radxa-linkr-debuggerctl ota confirm
```

Full workflow and rollback behavior: [OTA Firmware Update](ota.md).

## Board Monitoring

`GET /api/v1/status` (and `radxa-linkr-debuggerctl --json status`) includes a `board_monitoring` object. Each category carries `available` and `reason` — firmware only reports values from devices or APIs that are actually enabled.

| Category | What it reports |
|----------|----------------|
| `temperature` | CPU die temperature |
| `heap` | System heap usage (free, allocated, total) |
| `memory` | Memory pressure across multiple pools |
| `runtime` | Board uptime (`uptime_ms`, `uptime_seconds`) |
| `cpu` | CPU utilization delta |

### Memory pressure

The `memory` category tracks pressure across heap, network packet slabs, and data buffer pools:

- **`current_pressure`** — live snapshot, can rise and fall. The main field to watch.
- **`peak_pressure`** — boot-lifetime high-water mark, includes thread stack usage.
- `pressure_pct_x100` (legacy) — max of heap and stack, kept for backward compatibility.

Both objects include `limiting_component` (which pool is maxed out), `limiting_name` (the instance), and `pressure_pct_x100` (0–10000 range).

`physical` reports linker-reserved footprint — not live occupancy. `stacks` reports per-thread high-water marks.

## Status LED

On RP2350A boards, the blue status LED on GPIO25 acts as a watchdog heartbeat. It blinks approximately once per second and advances only after a successful hardware watchdog feed. Skipped or failed feeds reset it to the inactive state.
