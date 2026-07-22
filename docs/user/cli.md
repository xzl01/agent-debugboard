# CLI Reference

[中文](cli.zh-CN.md)

## NCM Network Interface

The firmware enumerates as a composite USB device. The CLI talks to the board over HTTP on the USB NCM interface, with the default device URL `http://172.29.203.1`. The board runs a DHCPv4 server on the NCM link so the host can automatically obtain a compatible address. Pass `--url <URL>` only if you intentionally override the default addressing.

```sh
radxa-linkr-debuggerctl --url http://192.168.1.100 status
```

## Basic Commands

Query board status and run diagnostics:

```sh
radxa-linkr-debuggerctl status
radxa-linkr-debuggerctl doctor
```

## JSON Output

Agent or automation code should prefer JSON output. JSON responses use a standard envelope:

- `schema`: `"radxa-linkr-debugger.v1"`
- `ok`: boolean indicating success
- `command`: the command that was executed
- `error`: `{code, message}` on failure

```sh
radxa-linkr-debuggerctl --json doctor
radxa-linkr-debuggerctl --json status
radxa-linkr-debuggerctl --json power list
radxa-linkr-debuggerctl --json adc read
radxa-linkr-debuggerctl --json gpio list
radxa-linkr-debuggerctl --json watchdog status
```

## Power Control

Control the three power outputs:

```sh
radxa-linkr-debuggerctl power set 12v_out on
radxa-linkr-debuggerctl power set 12v_out off
radxa-linkr-debuggerctl power set 5v_out on
radxa-linkr-debuggerctl power set 5v_out off
radxa-linkr-debuggerctl power set 20v_out on
```

List all power outputs:

```sh
radxa-linkr-debuggerctl power list
```

The board-internal VDD_5V rail is intentionally omitted from CLI status and power controls.

## ADC (Current Monitor)

Read current-monitor ADC channels:

```sh
radxa-linkr-debuggerctl adc read
radxa-linkr-debuggerctl adc read 5v_out
radxa-linkr-debuggerctl adc read 12v_out
radxa-linkr-debuggerctl adc read 20v_out
```

Human-readable output is concise by default (e.g. `5v_out=0.540000A`). Use `-v` / `--verbose` for debug fields such as `signal` and `mv`.

### Recording

`adc record` creates a live websocket session and records telemetry to a file:

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

### List and query

```sh
radxa-linkr-debuggerctl switch list
radxa-linkr-debuggerctl switch get sd
radxa-linkr-debuggerctl switch get usb
radxa-linkr-debuggerctl switch get vin
```

### SD switch

Route the TF/SD card between the target board and a USB reader:

```sh
radxa-linkr-debuggerctl switch route sd target
radxa-linkr-debuggerctl switch route sd usb-reader
```

### USB switch

Route USB between the PC and the target:

```sh
radxa-linkr-debuggerctl switch route usb pc --confirm
radxa-linkr-debuggerctl switch route usb target --confirm
```

### VIN control

Select the CH347 VIO voltage level:

```sh
radxa-linkr-debuggerctl switch route vin 3.3v --confirm
radxa-linkr-debuggerctl switch route vin 1.8v --confirm
```

VIN defaults to 3.3V. Switching to 1.8V is an expert operation: first confirm that the attached target supports 1.8V signaling, connect physical VIO measurement equipment, and explicitly accept the hardware side effect.

## GPIO

### List GPIOs

```sh
radxa-linkr-debuggerctl gpio list
```

### Set GPIO output

```sh
radxa-linkr-debuggerctl gpio set GP13 1
radxa-linkr-debuggerctl gpio set GP13 0
radxa-linkr-debuggerctl gpio set CON_MAS 1
```

### Read GPIO input

```sh
radxa-linkr-debuggerctl gpio input GP13
radxa-linkr-debuggerctl gpio input J16_PIN1
```

### GPIO naming

GPIO names accept three formats:
- `GPxx` — canonical MCU pin name (e.g. `GP13`)
- Numeric pin — raw MCU pin number (e.g. `4`)
- Board note — schematic label (e.g. `CON_MAS`, `J16_PIN1`)

CLI/TUI display both `GPxx` and the board `note`.

## Watchdog

```sh
radxa-linkr-debuggerctl watchdog status
```

The watchdog is owned by firmware, not the host. Firmware automatically arms the MCU hardware watchdog and only keeps feeding it while core firmware, the HTTP/API service, and the CDC ACM cmdline fallback are reporting healthy local liveness. WebSocket session silence, subscription timeout, and session expiration do **not** count as watchdog failures. If core firmware wedges, the API service stops responding, or the CDC ACM cmdline fallback stops reporting liveness, firmware stops feeding, the MCU resets, and the next boot enters ROM BOOTSEL via the retained recovery marker.

## OTA Firmware Update

```sh
radxa-linkr-debuggerctl ota status
radxa-linkr-debuggerctl ota upload /path/to/firmware.bin
radxa-linkr-debuggerctl ota test
radxa-linkr-debuggerctl ota confirm
```

- `ota status` reports the current OTA state (`idle`, `uploading`, `verified`, `pending_test`, `rebooting`, `failed`), flash sizes, and MCUboot swap type
- `ota upload` sends a MCUboot-format `.bin` file with SHA256 integrity check
- `ota test` requests a test boot of the verified image
- `ota confirm` manually confirms the running image immediately

OTA expects a MCUboot-format application binary. Do not upload `.uf2` or `.elf` files. Use the release asset `radxa-linkr-debugger-rp2350-ota.bin`.

### JSON OTA output

```sh
radxa-linkr-debuggerctl --json ota status
radxa-linkr-debuggerctl --json ota upload /path/to/firmware.bin
radxa-linkr-debuggerctl --json ota test
radxa-linkr-debuggerctl --json ota confirm
```

## Board Monitoring

Status JSON includes `board_monitoring` with the following categories:

| Category | Fields |
| --- | --- |
| `temperature` | CPU die temperature sensor readings |
| `heap` | System heap runtime statistics |
| `memory` | Additive pressure objects (see below) |
| `runtime` | Board uptime (`uptime_ms` / `uptime_seconds`) |
| `cpu` | CPU utilization deltas |

Each category carries `available` (bool) and `reason` (string). Firmware only reports values from Zephyr devices or runtime-stat APIs that are actually enabled.

### Memory pressure fields

The `memory` category carries three pressure-reporting fields:

- `pressure_pct_x100` (legacy) — `max(current system heap %, highest thread stack high-water %)`
- `current_pressure` — additive object: `max(current heap %, RX packet slab %, TX packet slab %, RX data buffer pool %, TX data buffer pool %)`. Can rise and fall dynamically.
- `peak_pressure` — boot-lifetime additive object with the same coverage plus thread stack high-water, plus `since: "boot"`.

Both `current_pressure` and `peak_pressure` include:
- `pressure_pct_x100` in the range 0..10000
- `limiting_component` — the component driving the maximum
- `limiting_name` — the instance name
- `tie_count` when multiple components share the maximum

The root `memory.coverage` keeps the legacy heap/stack meaning; `current_pressure.coverage` and `peak_pressure.coverage` describe the Phase 2 sources instrumented by their respective objects.

### Physical memory and stacks

- `physical` reports linker/Kconfig-reserved footprint (`total_bytes`, `image_reserved_bytes`, `reserved_pct_x100`). It is not live occupancy or free RAM.
- `stacks` reports aggregate high-water values: `thread_count`, `measured_count`, `error_count`, `total_bytes`, `used_high_water_bytes`, `max_pressure_pct_x100`, and `max_pressure_thread`.

Rust and Web clients prefer `current_pressure` when available, fall back to the legacy root `pressure_pct_x100` for Phase 1 compatibility, and fall back again to heap-only when `memory` is absent from the status response entirely.

## Status LED

On RP2350A boards, the blue status LED on GPIO25 acts as a watchdog heartbeat. It blinks approximately once per second and advances only after a successful hardware watchdog feed. Skipped or failed feeds reset it to the inactive state.
