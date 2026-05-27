---
name: radxa-linkr-debugger
description: Use curl or the optional Radxa Linkr Debugger CLI to diagnose and operate target-board power outputs, ADC current monitors, safe GPIOs, TF/SD routing, firmware-owned watchdog recovery, and RP2040 BOOTSEL mode over USB NCM HTTP while keeping USB CDC ACM available for fallback cmdline access.
---

# Radxa Linkr Debugger

Prefer direct HTTP requests with `curl` for Agent-side automation. The board
enumerates as a USB NCM network interface and exposes its control API at the
default device URL `http://172.29.203.1:8080`.

The board also runs a DHCPv4 server on the NCM link so the host can acquire a
compatible IPv4 address automatically. mDNS is not required for the normal
workflow.

`curl` remains the lowest-common-denominator path across macOS, Linux, and
Windows. The primary actively-developed host CLI/TUI path in this repository is
now the Rust implementation under `./cmd-ng/`. The older Go
`radxa-linkr-debuggerctl` path remains only as a deprecated legacy/reference path.
The RP2040 USB CDC ACM port is intentionally kept as a secondary path for
Zephyr cmdline access and BOOTSEL fallback.

For long-lived telemetry and bidirectional control, the firmware also exposes a
live-session workflow: create a live session over HTTP, then connect to the
returned dedicated websocket URL under `/api/v1/ws/<slot>`.
The interactive TUI is expected to close only its own websocket session
explicitly when it exits; unused sessions should expire automatically in
firmware. Firmware supports four live websocket slots and keeps the HTTP server
client capacity aligned with that limit. If you rebuild the CLI after websocket
lifecycle fixes, prefer verifying repeated open/close cycles and concurrent
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

After installation, run the matching binary. The release download path prefers
`radxa-linkr-debuggerctl-rust_<os>_<arch>.*`; legacy Go compatibility archives
remain in the release only for transition/reference use.

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

## JSON Contract

Agent automation should expect the top-level fields:

- `schema`: must be `radxa-linkr-debugger.v1`
- `ok`: boolean success flag
- `command`: command name
- `error`: present on failure, with `code` and `message`

If `ok` is `false`, do not infer success from partial fields. Handle
`error.code` first.

`GET /api/v1/status` and WebSocket `snapshot/status` messages include the same
`board_monitoring` object. Its `temperature`, `heap`, `runtime`, and `cpu`
members each report `available` and a machine-readable `reason`. Treat
`available: false` as authoritative; the firmware does not invent sensor,
memory, runtime, or CPU values when Zephyr has no reliable source enabled. On
the default RP2040 configuration, the board should report internal CPU die
temperature, system heap runtime statistics, real board uptime (`uptime_ms` /
`uptime_seconds`), and CPU utilization deltas. The CPU percentage can still
temporarily report `insufficient_runtime_window` until enough runtime delta has
been collected.

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
```

Each recorder output row keeps the existing JSON schema, records host receive
timestamps, and includes `metadata.requested_rate_hz`. If firmware telemetry
contains device-side timing fields, the recorder copies them into
`metadata.device_timing`. Current ADC telemetry carries `sequence` but no
explicit device timestamp, so do not assume `device_timing` is present when
analyzing capture cadence.

WebSocket clients can subscribe to telemetry and send control commands on the
same connection. Example subscription payload:

```json
{"type":"subscribe","topic":"live","rate_hz":60}
```

The host TUI does not need to redraw at the same rate as the live data stream.
The board controls WebSocket push cadence, and the same live push path carries
ADC telemetry plus status snapshots with `board_monitoring`, while the TUI can
render at a lower fixed frame rate.

Example control payload:

```json
{"type":"command","command":"power_set","output":"12v_out","state":"on"}
```

Autonomous watchdog recovery is firmware-owned. The host does not arm or feed
the watchdog. Firmware keeps the RP2040 hardware watchdog alive only while core
firmware, the HTTP/API service, and the CDC ACM cmdline fallback are still
reporting healthy liveness. WebSocket session silence, subscription timeout,
and session expiration are not watchdog failure conditions. If core firmware
wedges, the API service stops responding, or the CDC ACM cmdline fallback stops
reporting liveness, firmware stops feeding the watchdog, the RP2040 resets, and
the next boot enters ROM BOOTSEL using a retained marker. The direct
`bootloader` command and the CDC ACM shell fallback remain independent recovery
paths. Periodic memory diagnostics are log-only debug output and must not be
treated as an additional watchdog participant or a change to BOOTSEL
marker/reset semantics. The watchdog trace line is equally diagnostic-only.

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

When validating switch behavior, run the sequence strictly in order: send the
`switch route ...` request, wait briefly for settling, then issue the matching
`switch get ...`. Do not run conflicting route changes in parallel if your goal
is to verify stability on real hardware.

The unified `/api/v1/switch/*` family is the interface for mux-style controls
in this repository. `switch sd` controls the RS2099XTQC16 TF/SD route
between `target` and `usb-reader`, while `switch usb` controls the GP03 USB mux
between `pc` and `target`.

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

GPIO list/status responses expose `name`, `pin`, and `note`. Control targets may
use canonical `GPxx`, raw numeric pins such as `4`, or exact notes such as
`CON_MAS` / `J17_PIN1`.

Enter RP2040 BOOTSEL mode for flashing.

```sh
curl -fsS -X POST "$BOARD_URL/api/v1/bootloader"
```

After firmware changes, treat this HTTP BOOTSEL flow and the CDC ACM shell
fallback below as required validation paths before you finish; verify that the
serial fallback path still reaches the standard RP2040 ROM BOOTSEL workflow.

If the HTTP control plane is unavailable but the RP2040 CDC ACM shell is still
reachable, use the local Zephyr shell command instead:

```text
linkr-debugger:~$ bootloader
```

This shell command still uses the standard RP2040 ROM USB BOOTSEL path, so the
device should reappear as the usual `RP2 Boot` / `RPI-RP2` target for UF2 or
`picotool` workflows. On Linux you can also flash without root by mounting the
`RPI-RP2` volume with `udisksctl` and copying the canonical UF2:

```sh
RPI_RP2=$(udisksctl mount -b /dev/sdX1 | awk -F" at " '{print $2}' | tr -d '[:space:]')
cp build/radxa_linkr_debugger/zephyr/zephyr.uf2 "$RPI_RP2/"
```

Replace `/dev/sdX1` with the actual RP2040 BOOTSEL block device path on your
system.

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
debug connector. The RP2040 firmware controls target power and recovery lines;
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
- Treat power-output changes, GPIO changes, SD routing, and
  `bootloader` as side-effectful operations. Confirm the target and desired
  state before running them.
- Prefer soft reboot/reset for target-board restarts. Treat power-cycling as a
  hard-restart fallback that is destructive to target runtime state. Confirm
  the exact output and only cycle the output powering the target.
- `5V_FIN` is an input/source power input. Do not present it as a controllable output.
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
- Current-monitor hardware in this repository is documented as INA139 with a
  10 mOhm shunt and a 51 kOhm output load.
