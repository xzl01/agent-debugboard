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

`test run` executes a `linkr-test.v1` NDJSON script. `--serial` remains an alias for `--serial-uart0`; use `--serial-uart1` when a script contains UART1 steps. UART1 is never silently routed to UART0: its device path must be explicit. `--output` writes the result to disk; `.json`, `.csv`, and `.ndjson` extensions select the report format, while other extensions default to JSON.

```sh
radxa-linkr-debuggerctl test run startup.ndjson --serial /dev/tty.usbserial-1234
radxa-linkr-debuggerctl test run dual-uart.ndjson \
  --serial-uart0 /dev/tty.usbserial-A \
  --serial-uart1 /dev/tty.usbserial-B
radxa-linkr-debuggerctl test run startup.ndjson --output startup-report.json
```

A `gpio_assert` step evaluates the `direction` and `value` in its `params` directly. Direction accepts only `input`/`output`, and value accepts only `0`/`1`. A Ctrl+C interruption marks the report as `aborted`. With global `--json`, stdout uses the standard `radxa-linkr-debugger.v1` envelope and places counters under `summary`.

To repeat a consecutive group of commands, add a top-level `loop` item. `count`
accepts 1-1000 rounds; `steps` must contain at least one normal command. Nested
loops are not supported, and a script may expand to at most 10,000 executable
commands. The Web UI can create the same structure by selecting consecutive
commands and placing them in a loop frame.

```json
{"id":"boot-loop","type":"loop","params":{"count":3,"steps":[{"id":"cycle-off","type":"power_off","params":{"rail":"5v_out"}},{"id":"settle","type":"delay","params":{"ms":1000}},{"id":"cycle-on","type":"power_on","params":{"rail":"5v_out"}}]}}
```

## Tasks (firmware catalog and saved request sequences)

Built-in automation tasks come from the firmware-owned immutable catalog at
`GET /api/v1/tasks/catalog`. The CLI keeps no recovery recipes of its own:
every rail, GPIO, polarity, wait, and cleanup step arrives through the
catalog. The current catalog always exposes the six MASKROM/EDL recipes,
one per combination of mode and the `5v_out`, `12v_out`, and `20v_out` rails:

```text
builtin/maskrom/5v_out     builtin/edl/5v_out
builtin/maskrom/12v_out    builtin/edl/12v_out
builtin/maskrom/20v_out    builtin/edl/20v_out
```

Built-ins do not consume firmware task slots and are never read from, written
to, or deleted through `/api/v1/tasks`; they are dispatched through the
generic task runner, so each record is an ordinary `gpio` or `power` PUT.

The same merged listing includes task blobs explicitly persisted in debugger
flash under `linkr/task/tasks`. The firmware only validates and stores those
blobs; the CLI fetches a selected stored task through `GET /api/v1/tasks`,
parses it locally, and dispatches each record through normal HTTP. `wait_ms`
is applied client-side after a successful request and never reaches a control
endpoint. The first failure stops either built-in or stored execution. A
failed or partially cancelled built-in then runs the cleanup sequence
declared by the catalog; the CLI never infers or hardcodes its own cleanup.
Stored tasks never infer cleanup. A successful built-in finishes with the
selected rail on. This task flow is separate from `bootloader`, which enters
the debugger MCU's RP2350 BOOTSEL mode.

If the firmware catalog endpoint fails, `task list` still shows the stored
tasks together with a structured `catalog_error` and `catalog_available:false`,
and any `task run builtin/...` invocation returns `catalog_unavailable`
without falling back to a shadowed stored task. Stored tasks continue to work
while the catalog is unavailable.

```sh
# List the merged firmware catalog (built-in and stored)
radxa-linkr-debuggerctl task list

# Run a built-in task without storing it in firmware flash
radxa-linkr-debuggerctl task run builtin/maskrom/12v_out --confirm

# Store a linkr-task.v1 blob read from a file
radxa-linkr-debuggerctl task store my-task.ndjson my-task-id

# Run a stored task; the CLI fetches the blob and dispatches records
radxa-linkr-debuggerctl task run my-task-id --confirm

# Clear all stored tasks
radxa-linkr-debuggerctl task clear
```

Saved record rules — every record is a single `PUT` against an allowlisted
path under `/api/v1/power/`, `/api/v1/gpio/`, or `/api/v1/switch/`, with a
JSON-string `body` and an optional integer `wait_ms` in `0..60000`. The blob
starts with `# linkr-task.v1`, lists each task under `# task <id>`, and is
bounded at 4096 bytes with at most 4 tasks and 32 records per task. Records
that fall outside these limits, paths that fail the allowlist, malformed
JSON bodies, or non-PUT methods are rejected with `invalid_blob` before the
blob is written, so what is returned by the next `GET /api/v1/tasks` is
exactly what was stored.

Run behavior — `task run <task-id> --confirm` is fail-closed: omitting
`--confirm` returns `confirmation_required` before any board request. It first
fetches the firmware catalog from `GET /api/v1/tasks/catalog` and resolves a
built-in before any stored task; if the catalog is unavailable and the
requested id starts with `builtin/`, the CLI returns `catalog_unavailable`
instead of falling back to a shadowed stored task. A matched built-in is
executed directly without accessing `/api/v1/tasks`; otherwise the CLI
downloads the current stored blob, selects the named task, and dispatches each
record through `PUT` against the public HTTP API in order. Between records the
client sleeps `wait_ms` **only after a successful response**. A failure
(for example an unknown pin or a 4xx error) stops immediately: no later
record is dispatched, and the CLI exits nonzero. The JSON failure uses
`error.record_index` (1-based) and `error.path` under the `error` object;
the non-JSON failure writes
`task record <n> path "<path>" failed: <message>` to stderr. The wire
`body` sent on each request is the stored record's `body` field verbatim;
`wait_ms` never reaches the board. If a stored task uses a built-in ID
declared by the firmware catalog, `task list` marks it as shadowed and
`task run` selects the immutable built-in. `task clear` still removes that
stored entry but never removes the catalog entries. JSON `task list` reports
the merged `task_count` and the firmware entry count separately as
`stored_task_count`; each row carries `source`, and `catalog_available` plus
`catalog_error` report the catalog fetch outcome. Ctrl+C stops cooperatively
between requests or during `wait_ms`. Built-in cleanup comes from the catalog
and is reported separately from the primary failure/cancellation; cleanup
failure never replaces the primary error. Cancellation before the first
request performs no cleanup.

Boot behavior — the firmware no longer stores or executes any task at
boot. Saved tasks are inert until the CLI or Web UI explicitly runs them.
Old development data (anything written under the previous task-store
marker or settings key) is intentionally invalidated by the new marker
`# linkr-task.v1` and the new key `linkr/task/tasks`. There is no
migration, no read alias, and no reset for stale entries. A board that
still holds old data simply shows the empty task contract.

```text
GET    /api/v1/tasks
PUT    /api/v1/tasks
DELETE /api/v1/tasks
```

The firmware catalog built-ins remain available after `task clear` because
they are part of the firmware image, not the stored blob. Explicitly stored
custom tasks survive normal firmware reboot and combined-UF2 recovery through
the `linkr/task/tasks` key until `task clear` removes them; clearing stored
tasks does not change live hardware.

### CDC Fallback

When HTTP or the Web UI is unavailable but the USB CDC ACM shell still
works, the firmware exposes the same task surface on the serial console.
The shell commands are `task show` and `task clear` only; there are no
boot, default, or replay commands on the CDC surface.

```console
linkr-debugger:~$ task show
task show available=true task_count=1
linkr-debugger:~$ task clear
task clear ok
```

`task show` prints one line with `available` (firmware Settings+NVS
backend reachability) and `task_count`. `task clear` acquires the capture
owner and then the flash owner; if either is busy (live capture or OTA
in flight) it returns `task clear error=busy` and exits with `-EBUSY`
without deleting the stored blob. A successful clear writes
`task clear ok` and exits zero. A backend failure (after the busy path)
prints `task clear error=storage_error` and exits with `-EIO`. The CDC
shell uses the same firmware-enumerated IDs as the HTTP layer and
does not maintain a host-side catalog. This is the fallback when HTTP
or WS is wedged; it does not change the HTTP/CLI command surface.

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
