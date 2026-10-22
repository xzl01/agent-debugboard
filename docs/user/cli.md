# CLI Reference

[中文](cli.zh-CN.md)

`radxa-linkr-debuggerctl` talks to the board over USB NCM. Installed release
packages also provide `rdb` as a link to that exact executable, so either name
accepts the same commands. Examples below omit `--url` for brevity; prefix them
with `--url http://<board-ip>` or set `LINKR_BOARD_URL=http://<board-ip>`. The board runs a DHCPv4 server, so the host gets a
compatible address automatically. Each board derives and persists a unique
private `/30` and MAC on first boot. The host NCM address is the network's
`.2`; the board is `.1` in the same `/30`. Pass that board URL with `--url`
(the historical compiled default is only a fallback for old images). The CDC
shell command `ip` also prints the board URL.

With the unified desktop stack installed, running the CLI/TUI also starts one
`linkr-tray` daemon if it is not already running. The tray and Host services
share that process. Use `-d` / `--desktop` to start only the desktop launcher
and background daemon, then exit; the daemon remains alive after the CLI
exits. If no `DISPLAY` / `WAYLAND_DISPLAY` is available, or the GTK-linked tray
starts but cannot load an AppIndicator implementation, the CLI relaunches the
same binary with `--headless`. The Linux tray executable still requires the GTK
loader libraries; a minimal system without GTK should run `linkr-host serve`
directly. Ordinary CLI/TUI commands continue when desktop components are absent;
only explicit `-d` treats startup failure as fatal. Set `LINKR_SKIP_DESKTOP=1`
only when no background Host should be attempted. When a graphical session becomes available
later, the next CLI/TUI or `-d` launch gracefully replaces that headless daemon
with one graphical Tray daemon instead of running both.
On desktop startup failure, the CLI writes daemon and Host startup output
to `$XDG_DATA_HOME/radxa-linkr-debugger/host.log` and names that path in the
error; the Linux default is `~/.local/share/radxa-linkr-debugger/host.log`. When
running a graphical debug binary directly from the source tree on NixOS,
launch it inside `nix-shell` so `libayatana-appindicator3` is available to the
Tray. Headless mode does not construct the AppIndicator UI.

The daemon still contains the legacy CH347 `uart0`/`uart1` Serial Broker and MCP
serial client. Those commands are non-J4 compatibility/test-fixture paths
pending separate removal. They are not the production target terminal. Use the
board-hosted xterm.js terminal, MCU `serial.j4`, J4 CDC ACM, or raw TCP 5555 for
J4.

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


## Network Identity

```sh
radxa-linkr-debuggerctl --url http://10.20.30.1 ip
radxa-linkr-debuggerctl --url http://10.20.30.1 ip network 10.20.30.0/30
radxa-linkr-debuggerctl --url http://10.20.30.1 ip mac 02:11:22:33:44:55
radxa-linkr-debuggerctl --url http://10.20.30.1 --json ip
```

`ip` prints the persisted MAC, origin, private `/30`, board address, DHCP base,
and board URL. `network` and `mac` updates are persisted atomically by the
firmware. `net` is an alias for `ip`. Invalid MACs and non-private or unaligned
`/30` values are rejected locally. A network change may require reconnecting
the USB NCM interface or renewing the host DHCP lease; use `--url` with the new
board URL afterward.

## Power

Four rails are controllable: `12v_out`, `5v_out`, `20v_out`, and `vdd_5v`.
`vdd_5v` is the auxiliary 5 V rail and is exposed like the other outputs.

```sh
radxa-linkr-debuggerctl power list
radxa-linkr-debuggerctl power set 12v_out on
radxa-linkr-debuggerctl power set 5v_out off
radxa-linkr-debuggerctl power set 20v_out on
radxa-linkr-debuggerctl power set vdd_5v on
```

## ADC (Current And Voltage Monitor)

Four INA226 channels cover `5v_out`, `12v_out`, `20v_out`, and `vdd_5v`.
`adc0` is the PD-input source-side channel: its canonical value is the
Device-Tree-reconstructed DC20V source after the R46/R47 divider. `raw` and
`mv` remain ADC-pad diagnostics. `adc1` through `adc3` are native pad-voltage
channels. Default output is concise (`5v_out=0.540000A`); add `-v` for raw ADC
fields such as `signal`, `source_voltage`, `pad_mv`, and `raw`.

```sh
radxa-linkr-debuggerctl adc read
radxa-linkr-debuggerctl adc read 5v_out
radxa-linkr-debuggerctl adc read vdd_5v
radxa-linkr-debuggerctl adc read adc0
radxa-linkr-debuggerctl adc read adc3
```

The CLI displays the firmware canonical value and never applies `115/15`,
a calibration table, or zero-point correction. ADC0's uncalibrated accuracy is
approximately 2%; do not claim better precision without per-board calibration.

### Recording

`adc record` opens a websocket session and writes telemetry to disk:

```sh
radxa-linkr-debuggerctl adc record /tmp/adc.ndjson 500
radxa-linkr-debuggerctl adc record /tmp/adc.csv 500 --rate-hz 250
```

- Output format is determined by file extension: `.ndjson` or `.csv`
- Default subscription rate is 250 Hz; `--rate-hz HZ` accepts `1..250`. The
  CLI and firmware reject higher rates instead of clamping them.
- Each output record includes host receive timestamps plus `metadata.requested_rate_hz`
- Device timing is preserved under `metadata.device_timing`
- Ring overruns are reported as `metadata.dropped_samples` on the first affected row
- NDJSON preserves all eight descriptors. CSV starts with the four current
  rails and appends `adc0_voltage_uv` through `adc3_voltage_uv`; ADC0 is the
  canonical source-side value, ADC1-3 are native pad values, and unknown or
  unavailable fields are empty cells.

### JSON ADC output

`--json adc read` carries the raw diagnostic chain for the fixed eight-channel
order: four current rails followed by ADC0..ADC3. ADC0's canonical scalar is
source-side DC20V; its `raw`/`mv` are pad diagnostics; ADC1-3 are native pad
voltages. The host CLI presents firmware values directly without host-side
calibration or divider conversion. PD control is
available through `pd status`, `pd fixed MV`, and `pd pps MV` (board limit 20 V).

## Switch Routes

x1.2 exposes two firmware switch controls:

| Switch | Routes between | Values |
|--------|---------------|--------|
| `sd` | TF/SD card path | `target`, `usb-reader` |
| `tf_wp` | TF write protection | `writable`, `protected` |

```sh
radxa-linkr-debuggerctl switch list
radxa-linkr-debuggerctl switch get sd
radxa-linkr-debuggerctl switch route sd usb-reader
radxa-linkr-debuggerctl switch route tf_wp protected
```

x1.2 has no USB or VIN/VIO route. `/switch/usb` returns HTTP 404; `vdd_5v` is
an independent power output.

## GPIO

The eleven safe GPIOs include ordinary direct GP5/GP6 controls and input-only
GP29. Identifiers accept `GP13` (canonical), `13` (raw pin), or `CON_MAS`
(board note). The CLI shows both `GPxx` and the note.

```sh
radxa-linkr-debuggerctl gpio list
radxa-linkr-debuggerctl gpio set GP13 1
radxa-linkr-debuggerctl gpio set CON_MAS 1
radxa-linkr-debuggerctl gpio input GP13
```

## Automated Test Scripts

`test run` executes a `linkr-test.v1` NDJSON script. Its `--serial` /
`--serial-uart0` / `--serial-uart1` options are retained generic legacy
dual-UART/CH347 test fixtures; they do not select MCU J4 and are not recommended
for operator J4 use. Within that legacy runner, UART1 still requires an explicit
device path and is never silently routed to UART0. `--output` writes the result
to disk; `.json`, `.csv`, and `.ndjson` select the report format.

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

## Reboot And BOOTSEL

`reboot` and `bootloader` are separate operations:

```sh
# Normal cold restart; 250 ms after the response, never ROM BOOTSEL
radxa-linkr-debuggerctl reboot

# Explicit ROM BOOTSEL entry; flash only the combined UF2
radxa-linkr-debuggerctl bootloader
```

`reboot` sends `POST /api/v1/reboot`. It returns through the normal JSON
envelope with `command: "reboot"`, waits 250 ms, and performs a normal cold
restart. The reboot action itself never requests ROM BOOTSEL and never
enumerates as an `RPI` mass-storage device; holding the physical BOOTSEL button
across that reset is a separate ROM behavior. A rail not restored by a saved v1
configuration snapshot returns to its boot-off default. Re-apply rail and PD
setup after the board returns.

`bootloader` sends `POST /api/v1/bootloader` and is the only CLI command that
requests the ROM USB boot path. Use only the combined
`radxa-linkr-debugger-rp2350.uf2`; the application-only `zephyr.uf2` does not
contain MCUboot and must never be used for ROM BOOTSEL.

For restart and ROM BOOTSEL, the CDC ACM shell commands are `reboot` and
`bootloader`. `reboot` prints the normal-restart notice and does not enter
BOOTSEL; `bootloader` prints the BOOTSEL notice. The physical BOOTSEL button
has different semantics: hold it for 3 seconds while firmware is running and
release to request a normal restart; a short press does nothing, and holding it
without releasing does not reset. Holding it during power-on or reset is
sampled by the RP2350 ROM, which enters ROM BOOTSEL. The button is polled by a
firmware thread, so it cannot recover a completely wedged core; the hardware
watchdog remains the fallback for that case. In the TUI, select the
**System / Reboot** row and confirm it to request a normal restart; the TUI has
no BOOTSEL row or direct key. Use the separate CLI `bootloader` command or Web
**Enter BOOTSEL** card for ROM BOOTSEL. See the [TUI guide](tui.md) and
[Web UI guide](webui.md).

## Tasks (firmware catalog and task-v2 storage)

Built-in automation tasks come from the firmware-owned immutable catalog at
`GET /api/v1/tasks/catalog`. The CLI keeps no recovery recipes of its own:
every rail, GPIO, polarity, wait, and cleanup step arrives through the
catalog. The current catalog always exposes the six MASKROM/EDL recipes,
one per combination of mode and the `5v_out`, `12v_out`, and `20v_out` rails.

Stored tasks live in the separate four-slot task-v2 store. The board keeps
exact canonical `linkr-test.v1` source up to 4000 bytes and `linkr-task.v1`
deployment IR up to 4000 bytes per task. The store has its own UUID, uint64
revision, catalog fingerprint, and SHA-256 source/IR digests. Built-ins do not
consume slots and never appear in the stored metadata.

```sh
# List the merged firmware catalog (built-in and stored)
radxa-linkr-debuggerctl task list

# Run a built-in task without storing it in firmware flash
radxa-linkr-debuggerctl task run builtin/maskrom/12v_out --confirm

# Store a one-task IR file as an IR-only board task
radxa-linkr-debuggerctl task store my-task.ndjson my-task-id

# Import a full linkr-task.v1 IR set, replacing all board tasks
radxa-linkr-debuggerctl task import all-tasks.ndjson

# Export the exact full IR set
radxa-linkr-debuggerctl task export all-tasks.ndjson

# Export the board-backed exact source for one slot when present
radxa-linkr-debuggerctl task source my-task-id my-task.linkr-test.ndjson

# Delete one task or clear all tasks atomically
radxa-linkr-debuggerctl task delete my-task-id
radxa-linkr-debuggerctl task clear

# Run a stored task; the CLI fetches the IR and dispatches records
radxa-linkr-debuggerctl task run my-task-id --confirm
```

`task store` accepts a canonical `linkr-test.v1` workflow source or a
one-task `linkr-task.v1` IR and compiles/canonicalizes it locally. The current
CLI sends no source payload, so the board task is **IR-only**. Use the Web
workflow editor when board-backed lossless source is required. `task source`
therefore succeeds only for tasks whose source was stored through the
source-bearing Web/API upsert; otherwise it returns `task_source_absent`.
`task import` is full-set IR only and preserves a source only when the same
task ID keeps an unchanged task-IR digest. `task export` writes the exact full
IR set and reports the full-IR digest.

The task-v2 store uses only four slots and 4000-byte source/full-IR limits.
Mutations carry the current revision and catalog fingerprint; a stale value
returns a conflict with zero writes. `task clear` keeps the store UUID and
does not change live hardware. Task writes use only the JSON/base64 mutation endpoint; the CLI never sends a
blob upload or per-ID write/delete request.
The removed `linkr/task/tasks` Settings blob is not read or migrated.

`task list` merges the immutable built-ins with task-v2 metadata. Each row
shows `source=builtin|stored`, request count, and `lossless-source=present|
absent|unavailable`. Text output also prints a `storage` line with NVS
used/free, logical task bytes, reserve, and `save_allowed`; JSON output preserves
the firmware `storage` object. Built-ins have no lossless workflow source; an IR-only
stored task is explicitly `absent`. If the catalog endpoint fails, stored
tasks remain visible with a structured `catalog_error` and
`catalog_available:false`; a requested `builtin/...` fails with
`catalog_unavailable` rather than falling back to a stored task.

Run behavior — `task run <task-id> --confirm` is fail-closed: omitting
`--confirm` returns `confirmation_required` before any board request. It first
fetches the firmware catalog and resolves a built-in before any stored task.
A matched built-in is executed without reading task-v2 metadata; otherwise
the CLI downloads `/api/v1/tasks/ir`, selects the named task, and dispatches
each record through `PUT` against the public HTTP API in order. Between
records the client sleeps `wait_ms` **only after a successful response**. A
failure stops immediately, and later records are not dispatched. The JSON
failure uses `error.record_index` (1-based) and `error.path` under the
`error` object; the non-JSON failure writes
`task record <n> path "<path>" failed: <message>` to stderr. The wire `body`
sent on each request is the stored record's `body` field verbatim; `wait_ms`
never reaches the board. Ctrl+C stops cooperatively between requests or
during `wait_ms`. Built-in cleanup comes from the catalog and is reported
separately from the primary failure/cancellation; cleanup failure never
replaces the primary error. Cancellation before the first request performs no
cleanup.

Boot behavior — the firmware never stores, executes, or seeds a task at boot.
Stored tasks remain inert until the CLI or Web UI explicitly runs one. There
is no migration from the old `linkr/task/tasks` blob, no read alias, and no
automatic reset.

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

`task show` prints one line with `available` (task-v2 NVS backend
reachability) and `task_count`. `task clear` acquires the capture
owner and then the flash owner; if either is busy (live capture or OTA
in flight) it returns `task clear error=busy` and exits with `-EBUSY`
without deleting stored tasks. A successful clear writes
`task clear ok` and exits zero. A backend failure (after the busy path)
prints `task clear error=storage_error` and exits with `-EIO`. The CDC
surface exposes only show and clear; it does not run, import, export,
delete one task, or maintain a host-side catalog. This is the fallback when
HTTP or WS is wedged; it does not change the HTTP/CLI command surface.

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
