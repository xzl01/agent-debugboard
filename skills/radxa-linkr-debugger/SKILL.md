---
name: radxa-linkr-debugger
description: Use curl or the optional Radxa Linkr Debugger CLI to diagnose and operate target-board power outputs, ADC current monitors, safe GPIOs, TF/SD routing, firmware-owned watchdog recovery, and RP2350 BOOTSEL mode over USB NCM HTTP while keeping USB CDC ACM available for fallback cmdline access.
---

# Radxa Linkr Debugger

The active hardware target is G3 with RP2350A. G2/RP2040 is retired and must
not be built, flashed, or treated as a supported fallback. RP2354 requires a
dedicated board definition and HIL validation before use with this skill.

Prefer direct HTTP requests with `curl` for Agent-side automation. The board
enumerates as a USB NCM network interface and exposes its control API at the
default device URL `http://172.29.203.1`.

Production firmware also serves its embedded Web control panel from the root of
that URL. The page talks to the same-origin `/api/v1` HTTP and WebSocket paths;
Agent automation should continue to use curl because it is deterministic and
machine-readable. The HTTP listener is bound to the NCM-local `172.29.203.1`
address rather than every network interface.

The board also runs a DHCPv4 server on the NCM link so the host can acquire a
compatible IPv4 address automatically. mDNS is not required for the normal
workflow.

`curl` remains the lowest-common-denominator path across macOS, Linux, and
Windows. The host CLI/TUI path in this repository is the Rust implementation
under `./cmd-ng/`. The RP2350 USB CDC ACM port is intentionally kept as a
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
> diagnostic. The HTTP REST API at `http://172.29.203.1` is the canonical
> automation path.

The examples below assume this skill is checked into the current repository at
`./skills/radxa-linkr-debugger` and commands are run from the repository root. If
the skill is installed elsewhere, for example under `.claude/skills`, replace
the `./skills/radxa-linkr-debugger` prefix with the actual skill directory. Do not
use `./skills/...` from another repository unless that repository contains this
skill at that path.

- Default device URL: `http://172.29.203.1`
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
- Combined MCUboot+app UF2 (safe for BOOTSEL): `./build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`
- App-only UF2 (auto-regenerated from signed hex): `./build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.uf2`
- RP2350 OTA release asset: `radxa-linkr-debugger-rp2350-ota.bin`

The build system auto-generates both UF2 artifacts via a post-build step.
The app-only UF2 is regenerated from `zephyr.signed.hex` (not the raw
`zephyr.hex`), so it is MCUboot-bootable and safe for BOOTSEL flashing.
The combined UF2 includes MCUboot and the signed application.

**Never BOOTSEL-flash a UF2 built from the raw unsigned `zephyr.hex`** —
MCUboot rejects it (no header magic) and the board enters an unrecoverable
state requiring physical BOOTSEL recovery.

When an agent builds firmware, always use that exact build directory. For RP2350
sysbuild, the MCUboot hex is under
`./build/radxa_linkr_debugger/mcuboot/zephyr/zephyr.hex`, and the application
artifacts are under `./build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/`.
The application `zephyr.signed.bin` is an unsigned MCUboot-format OTA payload
under this project config despite the filename. Do not switch to alternate build
directories and do not flash stale artifacts copied somewhere else, such as a
temporary mount point.

The canonical firmware build includes the Web UI and requires Node.js 22 plus
npm. CMake runs the locked frontend build and embeds gzip-compressed assets in
flash; do not bypass that step with stale files from `web/dist`. Before an
RP2350 sysbuild, install MCUboot's image-tool dependencies with
`scripts/setup-zephyr.sh`, then run
`pip install -r .zephyr-workspace/bootloader/mcuboot/scripts/requirements.txt`
in the active workspace Python environment. Use `scripts/build-firmware.sh` for
the canonical full build.

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
     curl -fsS http://172.29.203.1/api/v1/status
    ```

   Windows PowerShell:

   ```powershell
    curl.exe -fsS http://172.29.203.1/api/v1/status
   ```

   For interactive browser use, open `http://172.29.203.1/`. The embedded
   page controls the board without a gateway for normal HTTP/WS board features.
   Its target serial panel has two supported paths:

    - **Override path** (direct CH347 Web Serial): after manually adding
        `http://172.29.203.1` to
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

The Rust tool keeps the same default URL (`http://172.29.203.1`) and still expects the `radxa-linkr-debugger.v1` JSON envelope. Running it without a subcommand starts the primary TUI, which polls HTTP status/ADC endpoints and keeps power, SD route, and GPIO controls in one adaptive grid.

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
the default RP2350 configuration, the board should report internal CPU die
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
BOARD_URL="http://172.29.203.1"
```

Windows PowerShell:

```powershell
$BoardUrl = 'http://172.29.203.1'
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

Use the firmware logic analyzer for RP2350 PIO2+DMA high-speed single-shot
capture. It is not sustained streaming; 50MHz and 125MHz are very short bursts.
HTTP capture is capped at 512 samples, supports up to 16 GPIO channels from the
safe allowlist (GP7-GP9, GP10-GP20, GP29), sample rates from 100,000 through
125,000,000 Hz (100 kHz-125 MHz), and accepts edge trigger names `none`, `rising`,
`falling`, and `either`. Pre-trigger sampling is supported for edge triggers at
≤25 MHz: set `pre_samples > 0` with `rising`, `falling`, or `either` to capture
samples before and after the trigger edge (capped at 512 total). The arm
response includes `requestedSampleRateHz`, `actualSampleRateHz`, `samplePeriodPs`,
and `backend`. The capture response additionally includes `sampleCount`,
`triggerIndex`, and a `config` object.

The logic analyzer lives in the Terminal workspace in the Web UI, not under
Advanced & recovery. Captured samples can be decoded in-browser using the
project-owned Rust/WASM decoder served at stable URLs:
- `/assets/decoder/logic-decoder.js` (served with JS MIME)
- `/assets/decoder/logic-decoder_bg.wasm` (served with `application/wasm` MIME, gzip-compressed)

The decoder supports UART, I2C, and SPI protocols only; it is not a
libsigrokdecode Python plugin compatibility layer.

PulseView / sigrok-cli can also connect directly with no client-side changes:
the firmware emulates a Rigol DS1102D (rigol-ds driver) over raw TCP on port 80
(shared with the web server via first-byte multiplexing). Use
`sigrok-cli -d rigol-ds:conn=tcp-raw/<board-ip>/80 ...`; digital channels
channels follow physical J16 connector order: D0-D11 = J16_PIN1-PIN12 (GP10..GP15 pattern ending with GP29), D12-D14 = J13 CON pins (GP7/GP8/GP9), and CH1 is the GP29 analog input. GP10 (J16_PIN1) is channel D0. Edge
triggers use scope-style config keys (`--config triggersource=D0
--config triggerslope=f`) with real hardware pre-trigger at ≤25 MHz, burst
trigger at >25 MHz, and AUTO fallback when no edge arrives. Use `--frames`,
not `--samples`. Deep captures (up to one million samples into the 2 MB
SPI-flash storage partition, ≤25 kHz digital / ≤10 kHz analog with edge or
level trigger) use vendor SCPI commands `:LINKR:DEEP:START <rate> [seconds]`,
`:LINKR:DEEP:STATUS?`, `:LINKR:DEEP:DATA? <off> <count>`, `:LINKR:DEEP:STOP`
on the same channel; stock sigrok memory mode is not usable with the DS1102D
identity (driver V2 samplerate limitation). For the unlimited continuous
view in PulseView use the BeagleLogic emulation on TCP port 5555
(`-d beaglelogic:conn=tcp-raw/<board-ip>/5555`, 14 digital channels GP7-GP20,
8/16-bit samples, full rate to ~150 kHz 16-bit). Full semantics and limits:
`doc/logic-analyzer.md`.

 ```sh
 timeout 5s curl -fsS -X DELETE "$BOARD_URL/api/v1/logic-analyzer"
 timeout 5s curl -fsS -X POST -H 'Content-Type: application/json' \
   --data '{"selected_pins":[13,15],"sample_rate_hz":1000000,"pre_samples":0,"post_samples":512,"trigger":"either"}' \
   "$BOARD_URL/api/v1/logic-analyzer"
 timeout 5s curl -fsS "$BOARD_URL/api/v1/logic-analyzer"
 timeout 5s curl -fsS "$BOARD_URL/api/v1/logic-analyzer/capture"
 ```

 For recovery or diagnostics, do not POST again to release an active capture.
 Poll status first, then explicitly release with DELETE when you want to discard
 the armed/capturing state:

 ```sh
 timeout 5s curl -fsS "$BOARD_URL/api/v1/logic-analyzer"
 timeout 5s curl -fsS -X DELETE "$BOARD_URL/api/v1/logic-analyzer"
 timeout 5s curl -fsS "$BOARD_URL/api/v1/logic-analyzer"
 ```

 Repeated `POST /api/v1/logic-analyzer` while the analyzer is `armed` or
 `capturing` returns HTTP 409 with `error.code` set to `already_armed`. Invalid
 JSON, invalid values, and unsupported combinations such as edge trigger plus
  `pre_samples > 0` with `trigger: "none"` or `pre_samples > 0` with rate >25 MHz
  return HTTP 400 with `error.code`
 set to `invalid_config`. Unexpected internal arm failures remain HTTP 500 with
 `error.code` set to `arm_failed`.

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

For triggered acquisition, arm the trigger detector over the same live
WebSocket. Firmware does not retain the waveform; subscribe to ADC telemetry
first and persist the stream on the host. Trigger names are `manual`, `current`,
`gpio`, and `power_on`:

```json
{"type":"command","command":"capture_arm","id":"capture-1","mode":"host-stream-v1","trigger":"current","output":"5v_out","threshold_ua":500000,"rate_hz":100}
```

For manual capture send `{"type":"command","command":"capture_trigger"}`.
Firmware emits `capture_triggered` when recording starts. Send `capture_stop`
after host recording ends, or `capture_cancel` to disarm it. The trigger event
contains `device_t_mono_us`, `sample_sequence`, and cumulative
`dropped_samples`; match the sequence against telemetry to align the waveform.
Only the owning WebSocket can trigger, stop, or cancel it.

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
the watchdog. Firmware keeps the RP2350 hardware watchdog alive only while core
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
while firmware owns the GPIO.

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

VIN control:

```sh
timeout 5s curl -fsS "$BOARD_URL/api/v1/switch/vin"   # returns 1.8v or 3.3v
timeout 5s curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"3.3v"}' \
  "$BOARD_URL/api/v1/switch/vin"   # safe default
```

VIN defaults to 3.3V at boot. Switching to 1.8V is side-effectful,
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

GPIO list/status responses expose `name`, `pin`, and `note`, plus additive
firmware-owned physical layout metadata: `layoutGroup`, `layoutLabel`,
`layoutRow`, and `layoutColumn`. Safe allowlist: `GP7` (`CON_MAS`), `GP8`
(`CON_REST`), `GP9` (`CON_USER`), `GP10`-`GP20` (J16), and `GP29` (ADC3).
Control targets may use canonical `GPxx`, raw numeric pins such as `4`, or
board-specific exact notes such as `CON_MAS` or `J16_PIN1`.

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

Mount the discovered partition and copy the correct UF2:

```sh
RPI_PART=$(timeout 5s lsblk -lnpo NAME,TYPE "$RPI_DISK" | awk '$2 == "part" { print $1; exit }')
[ -n "$RPI_PART" ] || { echo "BOOTSEL partition not found"; exit 1; }
RPI_MOUNT=$(timeout 5s udisksctl mount -b "$RPI_PART" | awk -F" at " '{print $2}' | tr -d '[:space:]')
FLASH_UF2=radxa-linkr-debugger-rp2350.uf2
cp "$FLASH_UF2" "$RPI_MOUNT/"
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

On firmware, the CDC ACM shell also exposes VIN control:

```text
linkr-debugger:~$ vin get   # returns 1.8v/3.3v
linkr-debugger:~$ vin set 3.3v   # safe default
```

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

## MCUboot OTA Firmware Update

The firmware supports unsigned MCUboot OTA firmware update.

**Security facts**: No signature verification, no authentication, no secure boot,
and no anti-rollback protection. Any host with USB NCM access can submit a
firmware image. SHA256 is used only to verify the integrity of the uploaded
payload, not to authenticate the sender.

**Initial installation** requires ROM BOOTSEL flashing with the combined bootable
`radxa-linkr-debugger-rp2350.uf2` artifact. After MCUboot is installed,
subsequent updates can be delivered via OTA using MCUboot-format application
binaries such as `radxa-linkr-debugger-rp2350-ota.bin`.

**Auto-confirm behavior**: After `ota test` reboots into the new image, a
16-second watchdog health gate runs before the image is auto-confirmed. The
browser never calls confirm automatically; firmware owns the entire gate. If the
watchdog resets before auto-confirm completes, the dedicated retained marker
allows MCUboot to roll back to the previous confirmed image instead of forcing
ROM BOOTSEL. Explicit `bootloader` commands and ordinary non-OTA watchdog resets
still enter ROM BOOTSEL.

**Web dashboard OTA**: the embedded Web UI exposes the same OTA workflow under
**Advanced & recovery**. It accepts only MCUboot-format `.bin` files, computes
SHA-256 locally in the browser (Web Crypto API with pure-JS fallback), uploads
via the same `/api/v1/ota/*` endpoints, and shows raw firmware OTA state through
polling. The UI never auto-confirms; the firmware ~16-second watchdog gate is
the only auto-confirm path. When running the UI from GitHub Pages, start the
device-bridge gateway first (`npm run device-bridge`) so the browser can reach
the board OTA endpoints over the HTTPS-to-HTTP bridge. The gateway permits the
OTA-specific headers (`X-Linkr-Ota-Size`, `X-Linkr-Ota-Sha256`) in CORS
responses.

CLI commands:

```sh
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --json ota status
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --json ota upload /path/to/firmware.bin
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --json ota test
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --json ota confirm
```

Or with `cargo run`:

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- --json ota status
cargo run --manifest-path cmd-ng/Cargo.toml -- --json ota upload /path/to/firmware.bin
cargo run --manifest-path cmd-ng/Cargo.toml -- --json ota test
cargo run --manifest-path cmd-ng/Cargo.toml -- --json ota confirm
```

Raw HTTP API:

```sh
# Check OTA state
curl -fsS "$BOARD_URL/api/v1/ota"

# Upload MCUboot-format binary
curl -fsS -X POST \
  -H 'Content-Type: application/octet-stream' \
  -H 'X-Linkr-Ota-Size: <byte_size>' \
  -H 'X-Linkr-Ota-Sha256: <hex_sha256>' \
  --data-binary @/path/to/firmware.bin \
  "$BOARD_URL/api/v1/ota/upload"

# Request test boot
curl -fsS -X POST "$BOARD_URL/api/v1/ota/test"

# Manually confirm
curl -fsS -X POST "$BOARD_URL/api/v1/ota/confirm"
```

`GET /api/v1/ota` returns `state` (`idle`/`uploading`/`verified`/`pending_test`/
`rebooting`/`failed`), expected/written/max byte sizes, the MCUboot upload area
ID, swap type, and `current_image_confirmed`. OTA upload requires both the
`X-Linkr-Ota-Size` and `X-Linkr-Ota-Sha256` headers. Do not upload `.uf2` or
`.elf` files via OTA; use a MCUboot-format application binary. The release OTA
payload `radxa-linkr-debugger-rp2350-ota.bin` is copied from sysbuild
`zephyr.signed.bin`; with this project config, that filename still represents
unsigned MCUboot format.

**HIL validation**: Changes affecting Web/host OTA control behavior, including
any modification to the OTA upload endpoint, the auto-confirm watchdog gate,
the rollback retained marker, the Web dashboard OTA UI, or the CLI `ota`
command logic, require board-level HIL functional validation before final
production acceptance. The HIL must exercise the full OTA sequence:
upload the MCUboot-format payload, trigger `ota test`, observe the test boot
reboot, confirm the watchdog health gate auto-confirms or manually confirm,
and verify rollback behavior when the watchdog resets before confirm. See
`doc/testing/hil-functional-test-spec.md` for the full checklist. HIL is
required before final acceptance; if any step is deferred or blocked, record it
explicitly and do not claim that the corresponding validation has passed.

## Web OTA HIL Automation

Two automated runners exercise the Web OTA path end-to-end without manual
browser interaction. Both default to dry-run mode and require `--execute` to
perform side-effectful operations.

**API runner** (`scripts/web-ota-hil.sh`): Issues raw HTTP requests against the
board OTA endpoints. Headless and fast. Exercises the OTA state machine, error
codes, and gate logic.

**Browser runner** (`web/scripts/ota-hil.mjs`): Drives a real Chromium/Chromium
instance via Playwright against the board-hosted Web UI at
`http://172.29.203.1/`. Exercises the full OTA card including local SHA-256
computation, upload, confirmation dialogs, and state polling. Two flows are
available: `auto` (waits for firmware watchdog auto-confirm) and `manual`
(clicks Confirm image after pending_test).

Both runners require explicit gates for side-effectful operations:

| Gate flag | Enables |
|---|---|
| `--allow-upload-test-reboot` | OTA upload, test boot, confirm flows |
| `--allow-bootsel` | HTTP or CDC ACM BOOTSEL entry |
| `--allow-flash` | UF2 copy to RPI-RP2 mount point |

`--flow all` is dry-run-only and cannot be combined with `--execute`.

The browser runner accepts `--playwright-module` and `--chromium-executable` to
control Playwright loading and the browser binary. It does not require global
Playwright installation; playwright-core is loaded dynamically through Node's
module resolution. For Nix users, a temporary nix environment or explicit Nix
store paths can supply the Chromium dependency without claiming an exact
unverified package attribute.

**API runner examples (dry-run)**:

```sh
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh --flow preflight
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh --flow status
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh --flow negative-upload
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh --flow all
```

**API runner examples (executable)**:

```sh
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh \
  --flow api-auto-confirm \
  --image build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin \
  --execute --allow-upload-test-reboot

./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh \
  --flow negative-upload \
  --execute --allow-upload-test-reboot

./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh \
  --flow bootsel-http \
  --execute --allow-bootsel

./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh \
  --flow flash-uf2 \
  --uf2 radxa-linkr-debugger-rp2350.uf2 \
  --execute --allow-flash
```

Watchdog rollback is BLOCKED in both runners because no safe fault-injection
path exists. The API runner reports this explicitly when `--flow watchdog-rollback`
is selected.

**Browser runner examples (dry-run)**:

```sh
cd web
node scripts/ota-hil.mjs --dry-run
```

**Browser runner examples (executable)**:

```sh
cd web
node scripts/ota-hil.mjs --execute --flow both
node scripts/ota-hil.mjs --execute --flow auto
node scripts/ota-hil.mjs --execute --flow manual \
  --chromium-executable /nix/store/...-chromium-.../bin/chromium
```

Both runners use port 80 at `http://172.29.203.1`. The browser runner connects
to the board-hosted Web UI on the same NCM-assigned address. The shell runner
also uses port 80 and the same default URL.

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

## Captive Portal Discovery Diagnostics

The firmware exposes a multi-path captive portal detection helper on the NCM
interface. The single HTTP service is bound to `172.29.203.1` on port 80 and
routes by URL path: `/`,
`/assets/*`, `/api/v1/*`, `/api/v1/ws/*`, `/captive-portal/api`, and legacy
detection probes.

DHCP assigns a local NCM address without advertising a default router or DNS
server, so the debugger does not replace the host's Internet path. It sends
DHCP option 114 (Captive Portal URI) set to
`http://172.29.203.1/captive-portal/api`. HTTP on port 80 answers
`/captive-portal/api` with `application/captive+json`; all other GET paths
not matching the routed paths redirect (HTTP 302) to `http://172.29.203.1/`.
OS auto-open is best-effort, not guaranteed; results vary by OS and multi-homed routing.

The pinned Zephyr HTTP/1 server handles dynamic-resource `HEAD` requests before
the application callback and returns a default headers-only HTTP 200. Use `GET`
for the following captive portal checks.

Check the captive portal HTTP endpoint:

```sh
timeout 5s curl -fsS -D - -o /dev/null http://172.29.203.1:80/captive-portal/api
```

Expect HTTP 200 with `Content-Type: application/captive+json`.

The root path serves the embedded Web UI. Check redirect behavior for an
unregistered legacy detection path:

```sh
timeout 5s curl -fsS -D - -o /dev/null http://172.29.203.1:80/generate_204
```

Expect HTTP 302 with `Location: http://172.29.203.1/`.

Read-only API endpoints reject non-GET methods. `POST` requests to
`/api/v1/status` and `/api/v1/adc/read` must return HTTP 405, as must `POST` to
`/captive-portal/api`.

DNS A record check (requires a DNS query tool such as `dig` or `nslookup` if
`curl` alone is insufficient):

```sh
timeout 5s dig +short @172.29.203.1 example.com A
```

Expect `172.29.203.1`. DNS AAAA check:

```sh
timeout 5s dig +short @172.29.203.1 example.com AAAA
```

Expect empty output (NOERROR/NODATA).

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
- Current-monitor hardware uses INA139 with a 10 mOhm shunt and a
  50 kOhm output load.

## Expert: VIN 1.8V Switching

VIN 1.8V switching applies only to RP2350A boards. This operation is
side-effectful and requires confirmed target voltage compatibility and physical
measurement setup before use.

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
