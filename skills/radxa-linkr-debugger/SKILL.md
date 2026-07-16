---
name: radxa-linkr-debugger
description: Use curl or the optional Radxa Linkr Debugger CLI to diagnose and operate target-board power outputs, ADC current monitors, safe GPIOs, TF/SD routing, firmware-owned watchdog recovery, and RP2040/RP2350 BOOTSEL mode over USB NCM HTTP while keeping USB CDC ACM available for fallback cmdline access.
---

# Radxa Linkr Debugger

Prefer direct HTTP requests with `curl` for Agent-side automation. The board
enumerates as a USB NCM network interface and exposes its control API at the
default device URL `http://172.29.203.1:8080`.

Production firmware also serves its embedded Web control panel from the root of
that URL. The page talks to the same-origin `/api/v1` HTTP and WebSocket paths;
Agent automation should continue to use curl because it is deterministic and
machine-readable.

The board also runs a DHCPv4 server on the NCM link so the host can acquire a
compatible IPv4 address automatically. mDNS is not required for the normal
workflow.

`curl` remains the lowest-common-denominator path across macOS, Linux, and
Windows. The host CLI/TUI path in this repository is the Rust implementation
under `./cmd-ng/`. The RP2040/RP2350 USB CDC ACM port is intentionally kept as a
secondary path for Zephyr cmdline access and BOOTSEL fallback.

For long-lived telemetry and bidirectional control, the firmware also exposes a
live-session workflow: create a live session over HTTP, then connect to the
returned dedicated websocket URL under `/api/v1/ws/<slot>`.
The interactive TUI is expected to close only its own websocket session
explicitly when it exits; unused sessions should expire automatically in
firmware. The firmware supports up to four concurrent websocket clients via
dedicated slot URLs under `/api/v1/ws/<slot>`. If you rebuild the CLI after
websocket lifecycle fixes, verify repeated open/close cycles and concurrent
subscriber behavior with the freshly built skill-local binary.

> **Agent automation rule**: always try `curl` HTTP requests first. Only
> download or build the CLI binary when `curl` is unavailable (not installed)
> or when the task specifically needs the interactive TUI or `doctor`
> diagnostic. The HTTP REST API at `http://172.29.203.1:8080` is the canonical
> automation path.

The examples below assume this skill is checked into the current repository at
`./skills/radxa-linkr-debugger` and commands are run from the repository root. If
the skill is installed elsewhere, for example under `.claude/skills`, replace
the `./skills/radxa-linkr-debugger` prefix with the actual skill directory. Do not
use `./skills/...` from another repository unless that repository contains this
skill at that path.

- Default device URL: `http://172.29.203.1:8080`
- Optional CLI binary (macOS/Linux): `./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl`
- Optional CLI binary (Windows): `./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl.exe`

## Repository Change Rules

When an agent changes files in this repository, follow `./AGENTS.md` and keep
this skill aligned with user-facing docs.

- Any code change must update the corresponding skill and documentation in the
  same change.
- If firmware behavior or host CLI logic changes, update the
  related skill/docs and run the relevant tests before finishing.
- If firmware changes, verify and preserve the USB CDC ACM serial BOOTSEL
  fallback path before finishing.
- If this skill or another repo skill changes, run a subagent validation/test
  before finishing.
- When adding new functionality, add corresponding functional tests whenever
  practical.

## Canonical Build and Flash Paths

For this repository, the firmware build and flash locations are fixed:

- Canonical build directory: `./build/radxa_linkr_debugger/`
- Canonical UF2 artifact: `./build/radxa_linkr_debugger/zephyr/zephyr.uf2`

When an agent builds or flashes firmware, always use that exact build directory
and UF2 path. Do not switch to alternate build directories and do not flash a
stale UF2 copied somewhere else, such as a temporary mount point.

The canonical firmware build includes the Web UI and requires Node.js 22 plus
npm. CMake runs the locked frontend build and embeds gzip-compressed assets in
flash; do not bypass that step with stale files from `web/dist`.

## First Checks

1. Confirm that curl is available.

   macOS/Linux:

   ```sh
   curl --version
   ```

   Windows PowerShell or CMD:

   ```powershell
   curl.exe --version
   ```

2. Confirm that the board answers on the default HTTP endpoint.

   macOS/Linux:

   ```sh
    curl -fsS http://172.29.203.1:8080/api/v1/status
   ```

   Windows PowerShell:

   ```powershell
    curl.exe -fsS http://172.29.203.1:8080/api/v1/status
   ```

   For interactive browser use, open `http://172.29.203.1:8080/`. The embedded
   page controls the board without a gateway for normal HTTP/WS board features.
   Its target serial panel has two supported paths:

   - **Override path** (direct CH347 Web Serial): after manually adding
       `http://172.29.203.1:8080` to
       `chrome://flags/#unsafely-treat-insecure-origin-as-secure`, relaunching
       the browser, and reopening the board page. Edge accepts this Chromium
       address. Ordinary web pages cannot navigate to browser-internal URLs; the
       address must be copied and pasted into the address bar. The board-hosted
       setup dialog presents a three-step tutorial: copy the flag URL, copy the
       exact origin, then enable the flag and relaunch. Both address surfaces
       are independent copy buttons with their own feedback. Copy success only
       confirms the text was placed in the clipboard; it does not confirm a
       working serial connection. The only Web Serial action is beside
       **Bridge** in each visible UART pane; the card does not render a separate
       serial guidance control. The chooser still appears when **Web Serial** is
       clicked and must be accepted. Because the board page is
       served over HTTP, the Clipboard API may not be available in all browser
       contexts; the copy controls must use an HTTP-compatible fallback when
       that API is unavailable. If copying still fails, the full address remains
       visible for manual selection. The modal dialog
       carries `role="dialog"` and `aria-modal="true"`, traps initial focus
       inside, contains Tab/Shift+Tab navigation within the dialog, closes on
       Escape with focus restored to the trigger element, and restores body
       scroll on close. This override is experimental and weakens origin
       security for that page; it does not remove the user gesture or chooser
       requirement.
   - **Bridge fallback**: when the override is not enabled or not available,
       keep the board page open, run `npm run device-bridge` in a separate
       terminal, and use the page's **Bridge** button.

   When testing the board-hosted UI with Playwright, distinguish between two
   failure modes. A `page.goto` failure or resource-load timeout points to a
   board, NCM, HTTP server, or browser setup problem. An assertion failure after
   the page loads points to a UI regression. The insecure-context test must
   confirm that the red button opens the setup dialog without requesting a
   serial port. The override-active test must confirm that the button uses the
   direct Web Serial path. Actual chooser display, manual CH347 selection, and
   serial I/O remain manual HIL because the chooser is a mandatory browser
   security mechanism and cannot be bypassed programmatically.

3. Treat these outcomes as follows:
   - Exit code `0` with valid JSON and `ok: true`: the board is ready.
   - Valid JSON with `ok: false`: read `error.code` and `error.message`; HTTP transport works, but the board rejected the operation.
   - Connection refused / timeout / no route: the board NCM link or address needs attention.
   - `curl` missing: install curl or use the optional repo-local CLI as a fallback.

## Optional: Install/Build Repo-Local CLI

This skill uses only repo-local scripts and binaries. Do not modify `PATH`,
shell profiles, or global install directories.

Install the CLI into the repo-local output directory when you want the TUI,
`doctor`, or the convenience wrapper around the HTTP API.

macOS/Linux:

```sh
./skills/radxa-linkr-debugger/scripts/install.sh
```

Install a specific release version. An explicit version always skips local
source builds and downloads the requested primary Rust CLI release artifact:

```sh
./skills/radxa-linkr-debugger/scripts/install.sh --version <tag>
```

Windows PowerShell:

```powershell
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\skills\radxa-linkr-debugger\scripts\install.ps1
```

Windows PowerShell specific release version:

```powershell
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\skills\radxa-linkr-debugger\scripts\install.ps1 -Version <tag>
```

After installation, run the matching binary. The release download path uses
`radxa-linkr-debuggerctl-rust_<os>_<arch>.*` archives.

macOS/Linux:

```sh
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --version
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --json doctor
```

Windows CMD:

```bat
.\skills\radxa-linkr-debugger\scripts\bin\radxa-linkr-debuggerctl.exe --version
.\skills\radxa-linkr-debugger\scripts\bin\radxa-linkr-debuggerctl.exe --json doctor
```

## Build Primary Rust cmd-ng

The repository's primary host CLI/TUI path is the Rust `cmd-ng`
implementation.

macOS/Linux/Windows with Rust installed:

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- --help
cargo run --manifest-path cmd-ng/Cargo.toml -- --json status
cargo run --manifest-path cmd-ng/Cargo.toml --
```

The Rust tool keeps the same default URL (`http://172.29.203.1:8080`) and still expects the `radxa-linkr-debugger.v1` JSON envelope. Running it without a subcommand starts the primary TUI, which polls HTTP status/ADC endpoints and keeps power, SD route, and GPIO controls in one adaptive grid.

The board-internal `5v_ws` rail is intentionally omitted from CLI/TUI status,
power lists, and controls. The raw HTTP API retains that compatibility entry for
low-level firmware diagnostics only.

## JSON Contract

Agent automation should expect the top-level fields:

- `schema`: must be `radxa-linkr-debugger.v1`
- `ok`: boolean success flag
- `command`: command name
- `error`: present on failure, with `code` and `message`

If `ok` is `false`, do not infer success from partial fields. Handle
`error.code` first.

`GET /api/v1/status` and WebSocket `snapshot/status` messages include the same
`board_monitoring` object. Its `temperature`, `heap`, `memory`, `runtime`, and `cpu`
members each report `available` and a machine-readable `reason`. Treat
`available: false` as authoritative; the firmware does not invent sensor,
memory, runtime, or CPU values when Zephyr has no reliable source enabled. On
the default RP2040/RP2350 configuration, the board should report internal CPU die
temperature, system heap runtime statistics, real board uptime (`uptime_ms` /
`uptime_seconds`), CPU utilization deltas, and the Phase 2 additive memory pressure objects.
The CPU percentage can still temporarily report `insufficient_runtime_window`
until enough runtime delta has been collected.

The `memory` category carries three pressure-reporting fields:

- `pressure_pct_x100` (legacy root, Phase 1 semantics) is `max(current system heap %, highest thread stack high-water %)` and remains for backward compatibility.
- `current_pressure` is an additive object: `max(current heap %, RX packet slab %, TX packet slab %, RX data buffer pool %, TX data buffer pool %)`. It can rise and fall dynamically and is not total or free RAM.
- `peak_pressure` is a boot-lifetime additive object with the same coverage as `current_pressure` plus thread stack high-water, plus a `since: "boot"` field.

Both `current_pressure` and `peak_pressure` include:
- `available: bool` and `reason: string` (fallback when the data source is absent)
- `pressure_pct_x100: int` in the range 0..10000
- `limiting_component: string` naming the component driving the maximum: `system_heap`, `net_pkt_rx`, `net_pkt_tx`, `net_buf_rx_data`, `net_buf_tx_data`, or `thread_stack` (peak only)
- `limiting_name: string` describing the limiting instance (thread name or pool name)
- `tie_count: int` when multiple components share the maximum value

`physical` reports linker/Kconfig-reserved footprint (`total_bytes`, `image_reserved_bytes`, `reserved_pct_x100`) and is not live occupancy or free RAM. `stacks` reports aggregate high-water values with `thread_count`, `measured_count`, `error_count`, `total_bytes`, `used_high_water_bytes`, `max_pressure_pct_x100`, and `max_pressure_thread`. The root `memory.coverage` keeps the legacy heap/stack meaning; `current_pressure.coverage` and `peak_pressure.coverage` describe the Phase 2 sources instrumented by their respective objects.

Rust and Web clients prefer `current_pressure` when available, fall back to the legacy root `pressure_pct_x100` for Phase 1 compatibility, and fall back again to heap-only when `memory` is absent entirely. Old firmware without `memory` is handled by the Rust CLI falling back to heap-only display and the Web UI falling back to heap free space. `memory` source is `zephyr` when emitted.

For short reset debugging, the watchdog supervisor also prints periodic memory
diagnostics in the firmware log. These lines prioritize heap allocated, free,
total, and peak bytes from the system heap plus real uptime. Treat them as
side-effect-free log output only: they do not feed the watchdog, do not advance
the CPU utilization sampling window, and do not change watchdog behavior.

The same 1 Hz diagnostics cadence also emits a watchdog trace line. Use it to
see whether the supervisor is still alive, whether hardware watchdog feeding was
`ok`, `failed`, or `skipped`, and which liveness source (`core`, `api`, or
`cmdline`) is currently blocking feeding. Treat this as log-only debug output;
it does not itself change watchdog behavior.

Firmware also logs the reset cause at boot and USB device lifecycle events
during runtime. When watchdog recovery enters ROM BOOTSEL, the boot message
distinguishes between an explicit bootloader command and an unhealthy liveness
stop.

## Common Commands

Set the board URL once per shell/session.

macOS/Linux:

```sh
BOARD_URL="http://172.29.203.1:8080"
```

Windows PowerShell:

```powershell
$BoardUrl = 'http://172.29.203.1:8080'
```

Read full board state.

macOS/Linux:

```sh
curl -fsS "$BOARD_URL/api/v1/status"
```

Windows PowerShell:

```powershell
curl.exe -fsS "$BoardUrl/api/v1/status"
```

List power outputs.

macOS/Linux:

```sh
curl -fsS "$BOARD_URL/api/v1/power"
```

Windows PowerShell:

```powershell
curl.exe -fsS "$BoardUrl/api/v1/power"
```

Control power outputs.

macOS/Linux:

```sh
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"on"}' \
  "$BOARD_URL/api/v1/power/12v_out"

curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"off"}' \
  "$BOARD_URL/api/v1/power/5v_out"
```

Windows PowerShell:

```powershell
curl.exe -fsS -X PUT -H "Content-Type: application/json" `
  --data '{"state":"on"}' `
  "$BoardUrl/api/v1/power/12v_out"

curl.exe -fsS -X PUT -H "Content-Type: application/json" `
  --data '{"state":"off"}' `
  "$BoardUrl/api/v1/power/5v_out"
```

Restart a target board with its normal software reboot or reset interface
first. Use power-cycling only as a hard-restart fallback when soft reboot/reset
is unavailable, the target is unresponsive, or no reset line is exposed.
Confirm the output name first, then turn it off, wait briefly for discharge,
and turn it back on:

```sh
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"off"}' \
  "$BOARD_URL/api/v1/power/5v_out"
sleep 2
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"on"}' \
  "$BOARD_URL/api/v1/power/5v_out"
curl -fsS "$BOARD_URL/api/v1/power"
```

Read ADC current monitors.

```sh
curl -fsS "$BOARD_URL/api/v1/adc/read"
curl -fsS "$BOARD_URL/api/v1/adc/read?channel=5v_out"
curl -fsS "$BOARD_URL/api/v1/adc/read?channel=12v_out"
curl -fsS "$BOARD_URL/api/v1/adc/read?channel=20v_out"
```

High-rate recording is a separate websocket workflow. Use the Rust CLI when you
need to write NDJSON telemetry records to a file. It defaults to 1000Hz and
accepts `--rate-hz HZ` for a 1..1000Hz requested websocket subscription rate:

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- adc record /tmp/adc.ndjson 1000 --rate-hz 250
cargo run --manifest-path cmd-ng/Cargo.toml -- adc record /tmp/adc.csv 1000 --rate-hz 250
```

Each recorder row keeps the existing JSON schema, host receive timestamps, and
`metadata.requested_rate_hz`. Requests above 100Hz use batch JSON on the wire,
while the recorder still writes one row per device sample. Single-sample
firmware telemetry keeps `sequence` and `uptime_us` and also emits
`sample_sequence` plus `device_t_mono_us`. Compact batch samples carry
`sequence` and `uptime_us`; the recorder normalizes them to the same aliases and
also accepts explicit aliases from compatible firmware. It preserves device time
under `metadata.device_timing` and deletes its live session on both successful
completion and error paths. Prefer device time over host receive time when
analyzing cadence, and treat
`metadata.dropped_samples` as authoritative evidence that the per-client
sampling ring overran. A `.csv` output path writes device time and three current
channels directly, using `device_t_mono_us` first, then `uptime_us`, then `0`.

For triggered acquisition, arm the firmware ring buffer over the same live
WebSocket. G3 supports 2048 samples and G2 supports 512; require
`pre_samples + post_samples + 1` to stay within capacity. Trigger names are
`manual`, `current`, `gpio`, and `power_on`:

```json
{"type":"command","command":"capture_arm","id":"capture-1","trigger":"current","output":"5v_out","threshold_ua":500000,"rate_hz":100,"pre_samples":100,"post_samples":300}
```

For manual capture send `{"type":"command","command":"capture_trigger"}`.
Cancel with `capture_cancel`. Firmware returns `capture_begin`, ordered
`capture_sample` frames, and `capture_complete`; normalize time against the
sample at `trigger_offset`. Only the owning WebSocket can trigger or cancel it.

WebSocket clients can subscribe to telemetry and send control commands on the
same connection. Example subscription payload:

```json
{"type":"subscribe","topic":"live","rate_hz":60}
```

Raw clients may add `"batch_size":20` for high-rate capture. Batch size is
limited to 20, and each client has an independent cursor into the shared sample
ring.

The host TUI does not need to redraw at the same rate as the live data stream.
The board controls WebSocket push cadence, and the same live push path carries
ADC telemetry plus status snapshots with `board_monitoring`, while the TUI can
render at a lower fixed frame rate.

Example control payload:

```json
{"type":"command","command":"power_set","output":"12v_out","state":"on"}
```

Autonomous watchdog recovery is firmware-owned. The host does not arm or feed
the watchdog. Firmware keeps the RP2040/RP2350 hardware watchdog alive only while core
firmware, the HTTP/API service, and the CDC ACM cmdline fallback are still
reporting healthy liveness. WebSocket session silence, subscription timeout,
and session expiration are not watchdog failure conditions. If core firmware
wedges, the API service stops responding, or the CDC ACM cmdline fallback stops
reporting liveness, firmware stops feeding the watchdog, the MCU resets, and
the next boot enters ROM BOOTSEL using a retained marker. The direct
`bootloader` command and the CDC ACM shell fallback remain independent recovery
paths. Periodic memory diagnostics are log-only debug output and must not be
treated as an additional watchdog participant or a change to BOOTSEL
marker/reset semantics. The watchdog trace line is equally diagnostic-only.

On G3 (RP2350A) boards, GPIO25 (the blue status LED) functions as a watchdog
heartbeat. It blinks at approximately 1 Hz and advances only after a successful
hardware watchdog feed. Skipped or failed feeds reset it to the inactive state
while firmware owns the GPIO. G2 (RP2040) has no firmware heartbeat LED.

```sh
curl -fsS "$BOARD_URL/api/v1/watchdog"
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --json watchdog status
```

For raw ADC inspection or hardware debugging, use the optional CLI verbose
output to inspect the firmware-reported diagnostic fields:

```sh
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl adc read -v 5v_out
```

Switch mux routes.

```sh
curl -fsS "$BOARD_URL/api/v1/switch"
curl -fsS "$BOARD_URL/api/v1/switch/sd"
curl -fsS "$BOARD_URL/api/v1/switch/usb"
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"usb-reader"}' \
  "$BOARD_URL/api/v1/switch/sd"
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"target"}' \
  "$BOARD_URL/api/v1/switch/usb"
```

VIN control (G3 only; RP2040 firmware omits this switch):

```sh
curl -fsS "$BOARD_URL/api/v1/switch/vin"   # G3: 1.8v or 3.3v; G2: unavailable
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"3.3v"}' \
  "$BOARD_URL/api/v1/switch/vin"   # safe default; G3 only
```

VIN defaults to 3.3V at boot. Switching to 1.8V is G3-only, side-effectful,
and requires confirmed target voltage compatibility and physical measurement
setup before use. The 1.8V procedure is documented in the Expert: VIN 1.8V
Switching section below.

When validating switch behavior, run the sequence strictly in order: send the
`switch route ...` request, wait briefly for settling, then issue the matching
`switch get ...`. Do not run conflicting route changes in parallel if your goal
is to verify stability on real hardware.

The unified `/api/v1/switch/*` family is the interface for mux-style controls
in this repository. `switch sd` controls the RS2099XTQC16 TF/SD route
between `target` and `usb-reader`, while `switch usb` controls the GP03 USB mux
between `pc` and `target`. On G3, GPIO1 VDD_5V and its GPIO6 VDD_1V8 child rail
are always on in Device Tree. The selectable CH347 VIO level is modeled as a
standard `regulator-gpio` regulator with exact 1.8V and 3.3V states; firmware
uses the Zephyr regulator API for `switch vin`. VIN defaults to 3.3V at boot.
Voltage switching is side-effectful; confirm your target supports the selected
level before applying it.

Use allowlisted GPIOs.

```sh
curl -fsS "$BOARD_URL/api/v1/gpio"
curl -fsS "$BOARD_URL/api/v1/gpio/GP13"
curl -fsS "$BOARD_URL/api/v1/gpio/CON_MAS"
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"direction":"output","value":1}' \
  "$BOARD_URL/api/v1/gpio/GP13"
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"direction":"output","value":1}' \
  "$BOARD_URL/api/v1/gpio/CON_MAS"
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"direction":"input"}' \
  "$BOARD_URL/api/v1/gpio/GP13"
```

GPIO list/status responses expose `name`, `pin`, and `note`. G3 safe allowlist:
`GP7` (`CON_MAS`), `GP8` (`CON_REST`), `GP9` (`CON_USER`), `GP10`-`GP20`
(J16), and `GP29` (ADC3). Control targets may use canonical `GPxx`, raw
numeric pins such as `4`, or board-specific exact notes such as `CON_MAS`,
`J17_PIN1` (G2), or `J16_PIN1` (G3).

Enter BOOTSEL mode for flashing.

```sh
timeout 5s curl -fsS -X POST "$BOARD_URL/api/v1/bootloader" || true
```

The USB connection can close while the MCU resets, so BOOTSEL enumeration is
the authoritative success check. Use a bounded retry loop with `timeout 5s
lsblk` to poll for a disk whose VENDOR column is exactly `RPI`:

```sh
RPI_DISK=
attempts=10
while [ "$attempts" -gt 0 ]; do
  RPI_DISK=$(timeout 5s lsblk -dpno NAME,VENDOR | awk '$2 == "RPI" { print $1; exit }')
  [ -n "$RPI_DISK" ] && break
  attempts=$((attempts - 1))
  sleep 1
done
[ -n "$RPI_DISK" ] || { echo "BOOTSEL device not found after 10s"; exit 1; }
```

Never assume a device letter such as `/dev/sdb`. The device name depends on
how many other USB storage devices are connected. The `lsblk` approach with the
exact `RPI` vendor match is the reliable discovery method.

Mount the discovered partition and copy the canonical UF2:

```sh
RPI_PART=$(timeout 5s lsblk -lnpo NAME,TYPE "$RPI_DISK" | awk '$2 == "part" { print $1; exit }')
[ -n "$RPI_PART" ] || { echo "BOOTSEL partition not found"; exit 1; }
RPI_MOUNT=$(timeout 5s udisksctl mount -b "$RPI_PART" | awk -F" at " '{print $2}' | tr -d '[:space:]')
cp build/radxa_linkr_debugger/zephyr/zephyr.uf2 "$RPI_MOUNT/"
```

After copying, allow a settle period before declaring success. Use bounded
retries against the HTTP endpoint to confirm the board has re-enumerated and is
responding:

```sh
BOARD_READY=
attempts=15
while [ "$attempts" -gt 0 ]; do
  if timeout 5s curl -fsS "$BOARD_URL/api/v1/status" >/dev/null; then
    BOARD_READY=1
    break
  fi
  attempts=$((attempts - 1))
  sleep 2
done
[ "$BOARD_READY" = 1 ] || { echo "board HTTP did not recover"; exit 1; }
```

After firmware changes, treat this HTTP BOOTSEL flow and the CDC ACM shell
fallback below as required validation paths before you finish; verify that the
serial fallback path still reaches the standard ROM BOOTSEL workflow.

If the HTTP control plane is unavailable but the CDC ACM shell is still
reachable, use the local Zephyr shell command instead:

```text
linkr-debugger:~$ bootloader
```

On G3 firmware, the same CDC ACM shell also exposes VIN control:

```text
linkr-debugger:~$ vin get   # G3: 1.8v/3.3v; G2: unavailable
linkr-debugger:~$ vin set 3.3v   # safe default; G3 only
```

On G2 firmware these `vin` commands return unavailable and do not change hardware.

This shell command still uses the standard ROM USB BOOTSEL path, so the
device should reappear as the usual `RP2 Boot` / `RPI-RP2` target for UF2 or
`picotool` workflows.

If you want the TUI or convenience wrapper instead of raw HTTP, the CLI still
works:

```sh
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --json doctor
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --json status
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl
```

## OpenOCD / JTAG Workflow

Use OpenOCD through the onboard CH347F path when the target board exposes
JTAG/SWD through the debug fixture. CH347F is wired directly to the target
debug connector. The firmware controls target power and recovery lines;
it does not sit in the JTAG/SWD path and does not act as a CMSIS-DAP,
Picoprobe, or JTAG probe.

First check OpenOCD availability:

```sh
openocd --version
```

Power the target first, then start OpenOCD with the CH347F interface script
available in the host OpenOCD installation and the target config for the board
under test:

```sh
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"on"}' \
  "$BOARD_URL/api/v1/power/5v_out"
curl -fsS "$BOARD_URL/api/v1/power"
openocd -f interface/<ch347-interface>.cfg -f target/<target>.cfg
```

CH347F support depends on the OpenOCD build. If the system OpenOCD package
does not include a CH347F interface script, use the WCH/vendor OpenOCD build
or add the matching interface script.

Use the output that actually powers the target. If the target uses `12v_out`
or `20v_out`, replace `5v_out` accordingly.

When a reset is needed, prefer a target software reboot or OpenOCD reset
command first:

```text
reset halt
reset run
```

Only use power-cycling as a hard-restart fallback when soft reset is not
available or the target is unresponsive.

## Safety Rules

- Prefer machine-readable JSON responses for all non-interactive use.
- Treat power-output changes, GPIO changes, SD routing, VIN switching, and
  `bootloader` as side-effectful operations. Confirm the target and desired
  state before running them.
- Prefer soft reboot/reset for target-board restarts. Treat power-cycling as a
  hard-restart fallback that is destructive to target runtime state. Confirm
  the exact output and only cycle the output powering the target.
- `5V_FIN` is an input/source power input. Do not present it as a controllable output.
- VIN switching is side-effectful. Confirm your target supports the selected
  voltage (1.8V or 3.3V) before applying it. The TUI requires confirmation
  before changing VIN; the CLI requires `--confirm` flag.
- Only use allowlisted GPIOs reported by `GET /api/v1/gpio` or the equivalent
  CLI command.
- Do not expose board-internal schematic codenames in user-facing output.

## ADC Notes

- `GET /api/v1/adc/read` exposes both raw ADC diagnostics (`readings[].raw`,
  `readings[].mv`) and the current-sense-amplifier result
  (`readings[].current_ua`, `readings[].sensor_value`).
- The host CLI no longer applies any host-side ADC calibration table or
  zero-point correction. Treat the reported current values as the firmware's
  direct readings.
- G3 current-monitor hardware uses INA139 with a 10 mOhm shunt and a
  50 kOhm output load. G2 uses 51 kOhm.

## Expert: VIN 1.8V Switching

VIN 1.8V switching applies only to G3 (RP2350A) boards and is not available on
G2 (RP2040). This operation is side-effectful and requires confirmed target
voltage compatibility and physical measurement setup before use.

Prerequisites before any 1.8V switch:

1. Confirm your target device's VIO supports 1.8V signaling.
2. Connect a voltmeter or oscilloscope to the target VIO pin.
3. Acknowledge that incorrect voltage will likely damage the target.

To switch to 1.8V (G3 only):

```sh
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"1.8v"}' \
  "$BOARD_URL/api/v1/switch/vin"
# Then immediately measure target VIO pin — expect ~1.8V
```

To restore safe 3.3V:

```sh
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"3.3v"}' \
  "$BOARD_URL/api/v1/switch/vin"
# Then immediately measure target VIO pin — expect ~3.3V
```

CDC ACM shell equivalent (G3 only):

```text
linkr-debugger:~$ vin set 1.8v
linkr-debugger:~$ vin set 3.3v
```
