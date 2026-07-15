# radxa-linkr-debugger

[中文](README.zh-CN.md)

RP2040 / RP2350 firmware for **Radxa Linkr Debugger**, a USB-controlled hardware
bridge that lets a PC-side Agent/AI operate target-board power, boot-mode, TF/SD
routing, current-monitor ADC channels, and a small safe GPIO surface.

![Radxa Linkr Debugger promo](doc/marketing/radxa-linkr-debugger-promo.png)

## Overview

Radxa Linkr Debugger is designed for automated board bring-up, recovery, production
test, and remote debugging workflows. The firmware enumerates as a composite
USB device with a USB NCM network interface for the main control plane and a
USB CDC ACM serial port reserved for Zephyr cmdline and BOOTSEL fallback.
Ordinary users operate it through the Web UI; advanced users, Agents,
automation, and HIL validation use the released Rust
`radxa-linkr-debuggerctl` CLI/TUI, whose source lives under `cmd-ng/`.

This repository contains the Zephyr application, Web UI, Rust host CLI/TUI,
unit tests, schematic copy, and project documentation.

The supported advanced host-side path is [`cmd-ng/`](cmd-ng/). The published
`radxa-linkr-debuggerctl` speaks the board HTTP API over USB NCM and provides
machine-readable automation, diagnostics, recording, and an interactive TUI.

## Features

| Area | Supported in this firmware |
| --- | --- |
| USB control | Composite USB device: NCM HTTP/WS control plane + CDC ACM fallback console |
| Host automation | Rust `cmd-ng` CLI/TUI with JSON output and `doctor` diagnostics |
| Live telemetry | Bidirectional WebSocket stream on a live-session URL under `/api/v1/ws/<slot>` |
| Triggered power capture | Device-timestamped current capture with pre/post ring buffer, manual/current/GPIO/power-on triggers, and CSV/NDJSON export |
| Power outputs | `12v_out`, `5v_out`, `20v_out` |
| ADC monitor | Current monitor reads for `5v_out`, `12v_out`, `20v_out` |
| Board self-monitoring | `/api/v1/status` and status WebSocket snapshots report board CPU/runtime/heap/temperature availability and values when Zephyr exposes reliable sources; the watchdog supervisor also prints periodic heap diagnostics for short-reset debugging |
| TF/SD routing | Switch route between `target` and `usb-reader` |
| VIN control (G3 only) | Switch route between `1.8v` and `3.3v`; firmware uses a Device Tree VIO regulator |
| GPIO | G2: `GP4`, `GP7`, `GP8`, `GP13`-`GP24`; G3: `GP7`, `GP8`, `GP9`, `GP10`-`GP20`, `GP29` |
| Autonomous watchdog recovery | Firmware-supervised watchdog resets into ROM BOOTSEL when core services stop reporting healthy liveness |
| Firmware update | USB command to reboot RP2040/RP2350 into BOOTSEL |

`5V_FIN` is intentionally treated as a separate input/source power input. It is
not exposed as a controllable output.

## For AI Agents

AI agents should read [skills/radxa-linkr-debugger/SKILL.md](skills/radxa-linkr-debugger/SKILL.md)
before operating hardware through this project. The skill is the canonical,
curl-first Agent-facing procedure for diagnosing the board connection,
building/running the primary host CLI when needed, and using JSON commands
safely.

Before making repository changes, AI agents should also read
[AGENTS.md](AGENTS.md). Repository-local rules:

- Any code change must update the related skill and documentation in the same change.
- Firmware or host CLI logic changes must update the related guidance and run the relevant tests.
- Firmware changes must verify and preserve the USB CDC ACM serial BOOTSEL fallback path before finishing.
- Skill changes must include a subagent validation/test run.
- When adding new functionality, add corresponding functional tests whenever practical.
- Firmware and hardware-interactive host changes require HIL functional testing before conclusion; see `AGENTS.md` and `doc/testing/hil-functional-test-spec.md`.
- Prefer describing board hardware in Device Tree whenever Zephyr bindings and the board model can express it cleanly.
- Keep software implementation standard, consistent, and elegant; avoid ad hoc patterns that make maintenance, automation, or documentation harder to follow.
- Keep MCU-side output as close as practical to raw interface values; prefer host-side interpretation, calibration, and presentation when that preserves the raw firmware contract.

Recommended agent flow:

```sh
radxa-linkr-debuggerctl --version
radxa-linkr-debuggerctl --json doctor
radxa-linkr-debuggerctl --json status
```

If the released host CLI is not downloaded or installed yet, use the release
installation path below. When following the Agent skill itself, keep using the
skill's curl-first workflow. For automation through the CLI, prefer `--json`;
parse `schema`, `ok`, `command`, and `error.code` instead of human-readable
text.

## Install Advanced/Agent Host CLI

Advanced users and Agents can install the released Rust
`radxa-linkr-debuggerctl` CLI. Download the matching archive from GitHub
Releases, or from a checkout use the repo-local installer scripts below with an
explicit version so they fetch the published release artifact instead of
building from source.

Install a specific release version:

```sh
./skills/radxa-linkr-debugger/scripts/install.sh --version <tag>
```

For a private repository release download, export a GitHub token first and
request the release version explicitly. `gh auth token` works if the GitHub CLI
is logged in:

```sh
export GH_TOKEN="$(gh auth token)"
./skills/radxa-linkr-debugger/scripts/install.sh --version <tag>
```

Windows PowerShell:

```powershell
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\skills\radxa-linkr-debugger\scripts\install.ps1
```

Private repository PowerShell release download:

```powershell
$env:GH_TOKEN = gh auth token
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\skills\radxa-linkr-debugger\scripts\install.ps1 -Version <tag>
```

Manual downloads are also available from each GitHub Release:

| OS / CPU | Artifact |
| --- | --- |
| Windows x64 | `radxa-linkr-debuggerctl-rust_windows_amd64.zip` |
| Linux x64 | `radxa-linkr-debuggerctl-rust_linux_amd64.tar.gz` |
| macOS Apple Silicon | `radxa-linkr-debuggerctl-rust_darwin_arm64.tar.gz` |

On macOS, unsigned release binaries may trigger a Gatekeeper warning saying Apple
cannot verify the software. The installer verifies `SHA256SUMS.txt` first and
then removes the quarantine flag from the installed binary. If you unpack a
release archive manually, verify the checksum and remove the quarantine flag
from the unpacked `radxa-linkr-debuggerctl` binary:

```sh
xattr -dr com.apple.quarantine ./radxa-linkr-debuggerctl
```

After installation, the examples below assume `radxa-linkr-debuggerctl` is on
your `PATH`, or that you invoke the unpacked release binary with the same
command name:

```sh
radxa-linkr-debuggerctl --help
radxa-linkr-debuggerctl --version
radxa-linkr-debuggerctl doctor
radxa-linkr-debuggerctl
```

Running the released host CLI without a subcommand starts the interactive TUI.
Use subcommands such as `status`, `adc read`, or `power set` when you want the
traditional command-line mode.

If you are developing `cmd-ng` itself from source, build it directly:

```sh
cargo build --manifest-path cmd-ng/Cargo.toml
./cmd-ng/target/debug/radxa-linkr-debuggerctl --help
```

The TUI keeps its own redraw cadence modest (60 Hz) and uses HTTP polling for
status plus ADC reads, which keeps multiple concurrent TUI instances stable.
Power outputs, switch controls, and safe GPIOs now share the control surface:
power outputs stay in their own section, the `Switch` section groups both
`switch sd [target|usb-reader]` and `switch usb [pc|target]`, and GPIO controls
remain separate. Arrow keys or Tab move selection, Space/Enter toggles the
selected item, and `i` returns the selected GPIO to input mode. The `t` / `u`
shortcuts still target the SD switch route directly. The status block now shows
both the optimistic `desired` switch state and the backend-confirmed `actual`
switch state, which helps diagnose transient or one-direction route failures.
For high-rate capture, use `adc record`, which
creates a live websocket session and records telemetry to an NDJSON file. It
defaults to a 1000Hz websocket subscription and accepts `--rate-hz HZ` for lower
rates. Each output record includes host receive timestamps plus
`metadata.requested_rate_hz`. Requests above 100Hz use batch JSON on the wire,
then the recorder expands each device sample into its own NDJSON or CSV row.
Firmware telemetry includes `sequence`, `sample_sequence`, `uptime_us`, and
`device_t_mono_us`; the recorder preserves device timing under
`metadata.device_timing`, uses `device_t_mono_us` for CSV when present, falls
back to `uptime_us` and then zero, and reports ring overruns as
`metadata.dropped_samples` on the first affected row.

Power-analyzer captures add a firmware ring buffer and device monotonic
timestamps. G3 keeps 2048 samples and G2 keeps 512. The Web UI can arm manual,
current-threshold, GPIO-edge, or power-on captures, overlay four runs, and
export CSV or NDJSON. See [doc/power-analyzer.md](doc/power-analyzer.md).

## Build Firmware

Create the Python environment and fetch Zephyr:

```sh
python3 -m venv .venv
source .venv/bin/activate
pip install -U pip west

west init -l .
west update
west zephyr-export
pip install -r zephyr/scripts/requirements.txt
```

Install the Zephyr SDK if it is not already installed. The current local build
has been verified with Zephyr SDK `1.0.1`.

Build the RP2040 firmware (G2 revision boards):

```sh
source .venv/bin/activate
west build -p always -b rpi_pico/rp2040 apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
```

Build the RP2350 firmware (G3 revision boards):

```sh
source .venv/bin/activate
west build -p always -b rpi_pico2/rp2350a/m33 apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
```

The generated UF2 is:

```text
build/radxa_linkr_debugger/zephyr/zephyr.uf2
```

For this repository, keep the firmware build/flash path fixed: always build
into `build/radxa_linkr_debugger/` and always flash
`build/radxa_linkr_debugger/zephyr/zephyr.uf2`. Do not switch to alternate build
directories or stale UF2 copies from temporary mount points.

## Flashing

If the board is already running this firmware, ask it to enter BOOTSEL and then
load the new UF2:

```sh
radxa-linkr-debuggerctl bootloader
picotool load -v -x build/radxa_linkr_debugger/zephyr/zephyr.uf2
```

After firmware changes, treat this BOOTSEL flow and the RP2040/RP2350 CDC ACM shell
fallback below as required validation paths; do not conclude the change until
you have verified the serial fallback path still works.

If HTTP/WS control is unavailable but the MCU CDC ACM shell is still up, you
can enter the same BOOTSEL path from the local Zephyr shell:

```text
linkr-debugger:~$ bootloader
```

If the board is already mounted as `RPI-RP2`, only run:

```sh
picotool load -v -x build/radxa_linkr_debugger/zephyr/zephyr.uf2
```

On Linux you can also flash without root by using `udisksctl` to mount the
`RPI-RP2` volume and then copying the canonical UF2:

```sh
RPI_RP2=$(udisksctl mount -b /dev/sdX1 | awk -F" at " '{print $2}' | tr -d '[:space:]')
cp build/radxa_linkr_debugger/zephyr/zephyr.uf2 "$RPI_RP2/"
```

Replace `/dev/sdX1` with the actual BOOTSEL block device path on your
system (use `lsblk -o NAME,SIZE,VENDOR,MOUNTPOINT` and look for the `RPI` vendor
entry). If you use drag-and-drop flashing through the `RPI-RP2` volume instead of
`picotool`, copy this same canonical artifact:

```text
build/radxa_linkr_debugger/zephyr/zephyr.uf2
```

## GitHub Actions Artifacts

The `Build` workflow checks every push and pull request. Tagging `v*` triggers
the `Release` workflow, which builds firmware, packages the host CLI, creates a
GitHub Release, and uploads the fixed release assets.

- `radxa-linkr-debugger-rp2040.uf2`: RP2040 firmware (G2) for drag-and-drop or `picotool`.
- `radxa-linkr-debugger-rp2040.elf`: RP2040 ELF for debugging.
- `radxa-linkr-debugger-rp2040.map`: RP2040 linker map.
- `radxa-linkr-debugger-rp2350.uf2`: RP2350 firmware (G3) for drag-and-drop or `picotool`.
- `radxa-linkr-debugger-rp2350.elf`: RP2350 ELF for debugging.
- `radxa-linkr-debugger-rp2350.map`: RP2350 linker map.
- `radxa-linkr-debuggerctl-rust_windows_amd64.zip`: advanced/Agent Rust CLI/TUI for Windows x64.
- `radxa-linkr-debuggerctl-rust_linux_amd64.tar.gz`: advanced/Agent Rust CLI/TUI for Linux x64.
- `radxa-linkr-debuggerctl-rust_darwin_arm64.tar.gz`: advanced/Agent Rust CLI/TUI for macOS Apple Silicon.
- `skills-radxa-linkr-debugger.tar.gz`: Agent skill bundle for `skills/radxa-linkr-debugger/`.
- `SHA256SUMS.txt`: SHA256 checksums for all release assets.

Advanced users and Agents should download one of the
`radxa-linkr-debuggerctl-rust_*` archives above. If you are developing
`cmd-ng` itself from source:

```sh
cargo build --manifest-path cmd-ng/Cargo.toml
./cmd-ng/target/debug/radxa-linkr-debuggerctl --help
```

## Advanced/Agent CLI Usage

Query board status:

```sh
radxa-linkr-debuggerctl status
radxa-linkr-debuggerctl doctor
```

Agent or automation code should prefer JSON output. JSON responses use
`schema: "radxa-linkr-debugger.v1"`, `ok`, `command`, and either command-specific
fields or `error: {code, message}`:

```sh
radxa-linkr-debuggerctl --json doctor
radxa-linkr-debuggerctl --json status
radxa-linkr-debuggerctl --json power list
radxa-linkr-debuggerctl --json adc read
radxa-linkr-debuggerctl --json gpio list
radxa-linkr-debuggerctl --json watchdog status
```

GPIO names such as `GP13` are derived from MCU pin numbers in firmware; each
allowlisted GPIO also carries a board-specific `note` such as `CON_MAS` or
`J17_PIN1`. CLI/TUI display both, and HTTP control accepts canonical `GPxx`,
raw numeric pins like `4`, or exact notes such as `CON_MAS`.

Status JSON also includes `board_monitoring`. Each category (`temperature`,
`heap`, `runtime`, and `cpu`) carries `available` plus a machine-readable
`reason`. Firmware only reports values from Zephyr devices or runtime-stat APIs
that are actually enabled; on the default RP2040/RP2350 configuration this includes the
internal CPU die temperature sensor, system heap runtime statistics, real board
uptime (`uptime_ms` / `uptime_seconds`), and CPU utilization deltas. The first
CPU sample can still report `insufficient_runtime_window` until the board has
accumulated enough runtime delta to derive a percentage.
WebSocket `snapshot/status` messages carry the same `board_monitoring` object as
`GET /api/v1/status`.

For short reset debugging, the watchdog supervisor thread also prints periodic
memory diagnostics to the firmware log. Those lines prioritize system-heap
allocated, free, total, and peak bytes, plus real uptime; they are
side-effect-free and do not advance the CPU utilization sampling window used by
`board_monitoring`.

The same 1 Hz diagnostics cadence also emits a watchdog trace line with the
current feed decision: whether the supervisor is alive, whether hardware
watchdog feeding succeeded, or whether feeding was skipped because `core`,
`api`, or `cmdline` is currently the blocker. This trace is log-only and is
intended to answer "who stopped the watchdog from being fed" during short reset
investigations.

At boot, the firmware logs the reset cause and, when entering ROM BOOTSEL via
watchdog recovery, prints the previous watchdog source (explicit bootloader
request versus unhealthy liveness stop). USB device lifecycle events are also
logged for diagnostic correlation with CDC ACM disconnects.

Control power outputs:

```sh
radxa-linkr-debuggerctl power set 12v_out on
radxa-linkr-debuggerctl power set 12v_out off
radxa-linkr-debuggerctl power set 5v_out on
radxa-linkr-debuggerctl power set 5v_out off
radxa-linkr-debuggerctl power set 20v_out on
```

The board-internal VDD_5V rail is intentionally omitted from CLI/TUI status,
power lists, and power controls. The raw firmware API retains its compatibility
entry for low-level diagnostics.

Read current-monitor ADC channels:

```sh
radxa-linkr-debuggerctl adc read
radxa-linkr-debuggerctl adc read 5v_out
radxa-linkr-debuggerctl adc record /tmp/adc.ndjson 1000 --rate-hz 250
radxa-linkr-debuggerctl adc record /tmp/adc.csv 1000 --rate-hz 250
radxa-linkr-debuggerctl adc read -v 5v_out
radxa-linkr-debuggerctl adc read 12v_out
radxa-linkr-debuggerctl adc read 20v_out
```

Human-readable ADC output is concise by default, for example
`5v_out=0.540000A`. Use `-v` / `--verbose` when you need debug fields such as
`signal` and `mv`. `radxa-linkr-debuggerctl --json adc read` carries the raw
diagnostic chain (`raw`, `mv`, `current_ua`, `sensor_value`) in addition
to the power-output state. The host CLI no longer applies host-side ADC
calibration tables or zero-point correction.

Switch routes:

```sh
radxa-linkr-debuggerctl switch list
radxa-linkr-debuggerctl switch get sd
radxa-linkr-debuggerctl switch get usb
radxa-linkr-debuggerctl switch route sd usb-reader
radxa-linkr-debuggerctl switch route usb target --confirm
```

VIN control (G3 only, RP2040 omits this switch):

```sh
radxa-linkr-debuggerctl switch get vin
radxa-linkr-debuggerctl switch route vin 3.3v --confirm
```

VIN defaults to 3.3V. Switching to 1.8V is an expert operation: first confirm
that the attached target supports 1.8V signaling, connect physical VIO
measurement equipment, and explicitly accept the hardware side effect. Follow
the gated procedure in the
[firmware app README](apps/radxa_linkr_debugger/README.md#expert-g3-vin-18v-switching).

For functional verification of mux/switch controls, run the commands strictly
sequentially: issue `switch route ...`, wait briefly for hardware settling
(typically a few seconds in local validation), then run `switch get ...`. Avoid
parallel or interleaved opposite-direction tests on the same board when you are
trying to validate stability.

Use safe GPIOs:

```sh
radxa-linkr-debuggerctl gpio list
radxa-linkr-debuggerctl gpio set GP13 1
radxa-linkr-debuggerctl gpio set CON_MAS 1
radxa-linkr-debuggerctl gpio input J17_PIN1  # G2
radxa-linkr-debuggerctl gpio input J16_PIN1  # G3
radxa-linkr-debuggerctl gpio input GP13
```

Use autonomous firmware watchdog recovery:

```sh
radxa-linkr-debuggerctl watchdog status
```

The watchdog is owned by firmware, not the host. Firmware automatically arms
the MCU hardware watchdog and only keeps feeding it while core firmware,
the HTTP/API service, and the CDC ACM cmdline fallback are still reporting
healthy local liveness. WebSocket session silence, subscription timeout, and
session expiration do not count as watchdog failures. If core firmware wedges,
the API service stops responding, or the CDC ACM cmdline fallback stops
reporting liveness, firmware stops feeding the watchdog, the MCU resets,
and the next earliest boot path enters the standard ROM BOOTSEL mode via the
retained recovery marker. The direct `bootloader` command and the CDC ACM
shell fallback remain separate and unchanged. Periodic memory diagnostics are
log-only debug output and do not add watchdog participants or change BOOTSEL
marker/reset behavior. The watchdog trace line is also log-only and does not
change feed policy.

## OpenOCD / JTAG

Radxa Linkr Debugger can be used together with OpenOCD by using the Linkr Debugger for
target power and recovery control while the onboard CH347F path handles target
JTAG/SWD. The CH347F is wired directly to the target debug connector; RP2040/RP2350
does not sit in that path and does not act as a CMSIS-DAP or JTAG probe.

Install OpenOCD, then verify it:

```sh
openocd --version
```

Power the target, then start OpenOCD with the CH347F interface script available
in your OpenOCD installation and the target configuration for the board under
test:

```sh
radxa-linkr-debuggerctl power set 5v_out on
openocd -f interface/<ch347-interface>.cfg -f target/<target>.cfg
```

CH347F support depends on the OpenOCD build. If the system OpenOCD package does
not include a CH347F interface script, use the WCH/vendor OpenOCD build or add
the matching interface script.

OpenOCD normally exposes GDB on TCP `3333` and telnet control on TCP `4444`.
Prefer OpenOCD reset commands such as `reset halt` or the target OS reboot path
first. Use power-cycling only as a hard-restart fallback.

See [doc/openocd/README.md](doc/openocd/README.md) for the full workflow.

## NCM Network Interface

The firmware enumerates as a composite USB device. The released
`radxa-linkr-debuggerctl` CLI talks to the board over HTTP on the USB NCM
interface, with the default device URL `http://172.29.203.1:8080`. The board
runs a DHCPv4 server on the NCM link so the host can automatically obtain a
compatible address; pass `radxa-linkr-debuggerctl --url ...` only if you
intentionally override the default addressing in your environment.

Ordinary users should use the Web UI. Advanced users and Agents can use the
released Rust CLI, which wraps the same HTTP JSON API; direct `curl` remains the
raw API and curl-first Agent path. The CDC ACM port is kept intentionally as a
secondary path for Zephyr cmdline access and recovery workflows such as BOOTSEL
fallback; it is not the primary automation/control transport. When the CDC ACM
shell is available, the local `bootloader` shell command enters the same MCU ROM
BOOTSEL path used by the HTTP API.

For long-lived telemetry and bidirectional control over a single socket, create
a live session over HTTP first and then connect to the returned dedicated
WebSocket URL under `/api/v1/ws/<slot>`. WebSocket clients can observe watchdog
status in status snapshots, but they do not feed the watchdog; firmware
supervision is autonomous. The firmware supports up to four concurrent
WebSocket clients; each live session gets a dedicated `/api/v1/ws/<slot>` URL.

mDNS is intentionally not part of the first-class workflow yet. DHCP already
solves the plug-and-play addressing problem across operating systems; mDNS is a
possible future enhancement for friendlier naming, not a requirement for
normal use.

## Hardware Mapping

### G2 (RP2040) Revision

| Function | Firmware name | Schematic signal | GPIO |
|---|---|---|---|
| 12 V output enable | `12v_out` | `GP02_12V_EN` | 2 |
| 5 V output enable | `5v_out` | `GP05_5V_EN` | 5 |
| 5 V WS enable | `5v_ws` | `GP09_5V_WS_EN` | 9 |
| 20 V output enable | `20v_out` | `GP10_20V_EN` | 10 |
| TF/SD route switch | `switch sd` | `GP06_TF_SW` | 6 |
| USB mux switch | `switch usb` | `GP03_USB_MUX` | 3 |
| 5 V current monitor | `adc read 5v_out` | `S_C_5V` | 26 (ADC0) |
| 12 V current monitor | `adc read 12v_out` | `S_C_12V` | 27 (ADC1) |
| 20 V current monitor | `adc read 20v_out` | `S_C_20V` | 28 (ADC2) |

### G3 (RP2350A) Revision

| Function | Firmware name | Schematic signal | GPIO |
|---|---|---|---|
| 12 V output enable | `12v_out` | `GP02_12V_EN` | 2 |
| 5 V output enable | `5v_out` | `GP05_5V_EN` | 0 |
| 5 V WS / VDD_5V always-on rail | `5v_ws` | `GP09_5V_WS_EN` | 1 |
| 20 V output enable | `20v_out` | `GP10_20V_EN` | 3 |
| TF/SD route switch | `switch sd` | `GP06_TF_SW` | 4 |
| USB hub mux switch | `switch usb` | `GP03_USB3_HUB` | 5 |
| CH347 1.8 V VIN supply | internal to `switch vin` | `1V8_EN` | 6 |
| TF write-protect | — | `TF_WP` | 22 |
| CH347 VIO voltage select | `switch vin` | `VIO_SEL` | 23 |
| Test point | — | `TP15` | 24 |
| Status LED | — | `LED_BLUE` | 25 |
| GPIO alias | `CON_MAS` | `CON_MAS` | 7 |
| GPIO alias | `CON_REST` | `CON_REST` | 8 |
| GPIO alias | `CON_USER` | `CON_USER` | 9 |
| J16 GPIO range | `GP10`-`GP20` | — | 10-20 |
| J16 ADC3 / GPIO | `ADC3` / `GP29` | — | 29 (ADC3) |
| 5 V current monitor | `adc read 5v_out` | `S_C_5V` | 26 (ADC0) |
| 12 V current monitor | `adc read 12v_out` | `S_C_12V` | 27 (ADC1) |
| 20 V current monitor | `adc read 20v_out` | `S_C_20V` | 28 (ADC2) |

VIN defaults to 3.3V at boot. GPIO1 VDD_5V and its GPIO6 VDD_1V8 child rail are
always on in the G3 Device Tree model. The selectable CH347 VIO level is modeled
as a standard `regulator-gpio` regulator with exact 1.8V and 3.3V states, and
firmware selects it through the Zephyr regulator API. Confirm your target
supports the selected voltage before applying it.

The G3 current monitor channels use INA139 with a 10 mOhm shunt and 50 kOhm
output load. (G2 uses 51 kOhm.) The MCU reports raw ADC diagnostics plus
standard sensor current values from Zephyr's `current-sense-amplifier`
interface, and the host CLI now presents those values directly without any
host-side calibration table or zero-point correction.
See the public
[TI INA139 datasheet](https://www.ti.com/product/INA139) for the sensor
transfer function.

The current schematic copies are stored at:
- G3 revision: [doc/radxa-linkr-debugger-schematic-x1.1.pdf](doc/radxa-linkr-debugger-schematic-x1.1.pdf)
- G2 revision: [doc/radxa-linkr-debugger-schematic.pdf](doc/radxa-linkr-debugger-schematic.pdf)

## Development

Run unit tests:

```sh
./apps/radxa_linkr_debugger/tests/run_unit_tests.sh
```

The test runner covers:

- host C tests for the shared board model.

## Repository Layout

```text
apps/radxa_linkr_debugger/        Zephyr application
apps/radxa_linkr_debugger/src/    Firmware source and shared board model
apps/radxa_linkr_debugger/tests/  Unit tests
cmd-ng/                          Rust host CLI/TUI for advanced users and Agents
web/                             Web UI and local device/serial bridge
doc/                          Hardware documents, OpenOCD configs, and marketing assets
skills/radxa-linkr-debugger/      Agent-facing skill and operating guide
west.yml                      Zephyr workspace manifest
```
