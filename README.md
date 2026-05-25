# agent-debugboard

[中文](README.zh-CN.md)

RP2040 firmware for **Agent DebugBoard**, a USB-controlled hardware bridge that
lets a PC-side Agent/AI operate target-board power, boot-mode, TF/SD routing,
current-monitor ADC channels, and a small safe GPIO surface.

![Agent Debugger promo](doc/marketing/agent-debugger-promo.png)

## Overview

Agent DebugBoard is designed for automated board bring-up, recovery, production
test, and remote debugging workflows. The firmware enumerates as a composite
USB device with a USB NCM network interface for the main control plane and a
USB CDC ACM serial port reserved for Zephyr cmdline and BOOTSEL fallback; the
primary host-side command path is the Rust CLI/TUI under `cmd-ng/`, which turns
board operations into scriptable commands.

This repository contains the Zephyr application, the primary Rust host CLI/TUI,
legacy Go host CLI sources kept for deprecated/reference use, unit tests,
schematic copy, and project documentation.

The active host-side development path is [`cmd-ng/`](cmd-ng/). The older Go
`cmd/agent-debugboardctl` + `internal/hostcli` stack is deprecated and kept only
as a legacy/reference implementation during migration.

## Features

| Area | Supported in this firmware |
| --- | --- |
| USB control | Composite USB device: NCM HTTP/WS control plane + CDC ACM fallback console |
| Host automation | Rust `cmd-ng` CLI/TUI with JSON output and `doctor` diagnostics |
| Live telemetry | Bidirectional WebSocket stream on `/api/v1/ws` |
| Power outputs | `12v_out`, `5v_out`, `5v_ws`, `20v_out` |
| ADC monitor | Current monitor reads for `5v_out`, `12v_out`, `20v_out` |
| Board self-monitoring | `/api/v1/status` and status WebSocket snapshots report board CPU/runtime/heap/temperature availability and values when Zephyr exposes reliable sources; the watchdog supervisor also prints periodic heap diagnostics for short-reset debugging |
| TF/SD routing | Switch route between `target` and `usb-reader` |
| GPIO | Safe allowlist: `GP4`, `GP7`, `GP8`, `GP13`-`GP24` |
| Autonomous watchdog recovery | Firmware-supervised watchdog resets into ROM BOOTSEL when core services stop reporting healthy liveness |
| Firmware update | USB command to reboot RP2040 into BOOTSEL |

`5V_FIN` is intentionally treated as a separate input/source power input. It is
not exposed as a controllable output.

## For AI Agents

AI agents should read [skills/agent-debugboard/SKILL.md](skills/agent-debugboard/SKILL.md)
before operating hardware through this project. The skill is the canonical
Agent-facing procedure for building/running the primary host CLI, diagnosing the
board connection, and using JSON commands safely.

Before making repository changes, AI agents should also read
[AGENTS.md](AGENTS.md). Repository-local rules:

- Any code change must update the related skill and documentation in the same change.
- Firmware or host CLI logic changes must update the related guidance and run the relevant tests.
- Firmware changes must verify and preserve the USB CDC ACM serial BOOTSEL fallback path before finishing.
- Skill changes must include a subagent validation/test run.
- When adding new functionality, add corresponding functional tests whenever practical.
- Prefer describing board hardware in Device Tree whenever Zephyr bindings and the board model can express it cleanly.
- Keep software implementation standard, consistent, and elegant; avoid ad hoc patterns that make maintenance, automation, or documentation harder to follow.
- Keep MCU-side output as close as practical to raw interface values; prefer host-side interpretation, calibration, and presentation when that preserves the raw firmware contract.

Recommended agent flow:

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- --version
cargo run --manifest-path cmd-ng/Cargo.toml -- --json doctor
cargo run --manifest-path cmd-ng/Cargo.toml -- --json status
```

If the host CLI is not built/installed yet, follow the commands in the skill
first. For automation, prefer `--json`; parse `schema`, `ok`, `command`, and
`error.code` instead of human-readable text.

## Install Host CLI

`agent-debugboardctl` is a native Go binary. Users do not need Python, pip, or a
virtual environment.

From a checkout, build/run the active Rust host CLI directly:

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- --help
cargo run --manifest-path cmd-ng/Cargo.toml -- --version
cargo run --manifest-path cmd-ng/Cargo.toml --
```

The legacy Go install scripts are retained only for compatibility/transition
workflows.

Install a specific legacy Go release version:

```sh
./skills/agent-debugboard/scripts/install.sh --version <tag>
```

For a private repository release download, export a GitHub token first and
request the release version explicitly. `gh auth token` works if the GitHub CLI
is logged in:

```sh
export GH_TOKEN="$(gh auth token)"
./skills/agent-debugboard/scripts/install.sh --version <tag>
```

Windows PowerShell:

```powershell
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\skills\agent-debugboard\scripts\install.ps1
```

Private repository PowerShell release download:

```powershell
$env:GH_TOKEN = gh auth token
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\skills\agent-debugboard\scripts\install.ps1 -Version <tag>
```

Manual downloads are also available from each GitHub Release:

| OS / CPU | Artifact |
| --- | --- |
| Windows x64 | `agent-debugboardctl-rust_windows_amd64.zip` |
| Windows arm64 | `agent-debugboardctl-rust_windows_arm64.zip` |
| Linux x64 | `agent-debugboardctl-rust_linux_amd64.tar.gz` |
| Linux arm64 | `agent-debugboardctl-rust_linux_arm64.tar.gz` |
| macOS Intel | `agent-debugboardctl-rust_darwin_amd64.tar.gz` |
| macOS Apple Silicon | `agent-debugboardctl-rust_darwin_arm64.tar.gz` |

Legacy Go compatibility archives remain available in GitHub Releases as
`agent-debugboardctl_<os>_<arch>.*` when you need the deprecated host CLI for
comparison or transition workflows. The skill installer and release download
examples prefer the primary Rust CLI/TUI archives.

On macOS, unsigned release binaries may trigger a Gatekeeper warning saying Apple
cannot verify the software. The installer verifies `SHA256SUMS.txt` first and
then removes the quarantine flag from the installed binary. If installing
manually, verify the checksum and run:

```sh
xattr -dr com.apple.quarantine ./skills/agent-debugboard/scripts/bin/agent-debugboardctl
```

After build/install:

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- --help
cargo run --manifest-path cmd-ng/Cargo.toml -- --version
cargo run --manifest-path cmd-ng/Cargo.toml -- doctor
cargo run --manifest-path cmd-ng/Cargo.toml --
```

Running the Rust host CLI without a subcommand starts the interactive TUI.
Use subcommands such as `status`, `adc read`, or `power set` when you want the
traditional command-line mode.

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
`metadata.requested_rate_hz`; if firmware telemetry includes device-side timing
fields, the recorder copies them into `metadata.device_timing`. Current ADC
telemetry includes `sequence` but no explicit device timestamp, so
`device_timing` may be absent.

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

Build the RP2040 firmware:

```sh
source .venv/bin/activate
west build -p always -b rpi_pico/rp2040 apps/agent_debugboard -d build/agent_debugboard
```

The generated UF2 is:

```text
build/agent_debugboard/zephyr/zephyr.uf2
```

For this repository, keep the firmware build/flash path fixed: always build
into `build/agent_debugboard/` and always flash
`build/agent_debugboard/zephyr/zephyr.uf2`. Do not switch to alternate build
directories or stale UF2 copies from temporary mount points.

## Flashing

If the board is already running this firmware, ask it to enter BOOTSEL and then
load the new UF2:

```sh
agent-debugboardctl bootloader
picotool load -v -x build/agent_debugboard/zephyr/zephyr.uf2
```

After firmware changes, treat this BOOTSEL flow and the RP2040 CDC ACM shell
fallback below as required validation paths; do not conclude the change until
you have verified the serial fallback path still works.

If HTTP/WS control is unavailable but the RP2040 CDC ACM shell is still up, you
can enter the same BOOTSEL path from the local Zephyr shell:

```text
debugboard:~$ bootloader
```

If the board is already mounted as `RPI-RP2`, only run:

```sh
picotool load -v -x build/agent_debugboard/zephyr/zephyr.uf2
```

If you use drag-and-drop flashing through the `RPI-RP2` volume instead of
`picotool`, copy this same canonical artifact:

```text
build/agent_debugboard/zephyr/zephyr.uf2
```

## GitHub Actions Artifacts

The `Build` workflow checks every push and pull request. Tagging `v*` triggers
the `Release` workflow, which builds firmware, packages the host CLI, creates a
GitHub Release, and uploads the fixed release assets.

- `agent-debugboard-rp2040.uf2`: RP2040 firmware for drag-and-drop or `picotool`.
- `agent-debugboard-rp2040.elf`: RP2040 ELF for debugging.
- `agent-debugboard-rp2040.map`: RP2040 linker map.
- `agent-debugboardctl-rust_windows_amd64.zip`: primary Rust CLI/TUI for Windows x64.
- `agent-debugboardctl-rust_windows_arm64.zip`: primary Rust CLI/TUI for Windows arm64.
- `agent-debugboardctl-rust_linux_amd64.tar.gz`: primary Rust CLI/TUI for Linux x64.
- `agent-debugboardctl-rust_linux_arm64.tar.gz`: primary Rust CLI/TUI for Linux arm64.
- `agent-debugboardctl-rust_darwin_amd64.tar.gz`: primary Rust CLI/TUI for macOS Intel.
- `agent-debugboardctl-rust_darwin_arm64.tar.gz`: primary Rust CLI/TUI for macOS Apple Silicon.
- `agent-debugboardctl_<os>_<arch>.*`: deprecated Go CLI compatibility archives.
- `skills-agent-debugboard.tar.gz`: Agent skill bundle for `skills/agent-debugboard/`.
- `SHA256SUMS.txt`: SHA256 checksums for all release assets.

Developers can build the primary host CLI from source:

```sh
cargo build --manifest-path cmd-ng/Cargo.toml
./cmd-ng/target/debug/agent-debugboardctl --help
```

The Rust `cmd-ng` version is now the primary development path. Build and run it directly:

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- --help
cargo run --manifest-path cmd-ng/Cargo.toml -- --json status
cargo run --manifest-path cmd-ng/Cargo.toml --
```

Running it without a subcommand starts the Rust TUI.

## Host Usage

Query board status:

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- status
cargo run --manifest-path cmd-ng/Cargo.toml -- doctor
```

Agent or automation code should prefer JSON output. JSON responses use
`schema: "agent-debugboard.v1"`, `ok`, `command`, and either command-specific
fields or `error: {code, message}`:

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- --json doctor
cargo run --manifest-path cmd-ng/Cargo.toml -- --json status
cargo run --manifest-path cmd-ng/Cargo.toml -- --json power list
cargo run --manifest-path cmd-ng/Cargo.toml -- --json adc read
cargo run --manifest-path cmd-ng/Cargo.toml -- --json gpio list
cargo run --manifest-path cmd-ng/Cargo.toml -- --json watchdog status
```

GPIO names such as `GP13` are derived from RP2040 pin numbers in firmware; each
allowlisted GPIO also carries a board-specific `note` such as `CON_MAS` or
`J17_PIN1`. CLI/TUI display both, and HTTP control accepts canonical `GPxx`,
raw numeric pins like `4`, or exact notes such as `CON_MAS`.

Status JSON also includes `board_monitoring`. Each category (`temperature`,
`heap`, `runtime`, and `cpu`) carries `available` plus a machine-readable
`reason`. Firmware only reports values from Zephyr devices or runtime-stat APIs
that are actually enabled; on the default RP2040 configuration this includes the
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
cargo run --manifest-path cmd-ng/Cargo.toml -- power set 12v_out on
cargo run --manifest-path cmd-ng/Cargo.toml -- power set 12v_out off
cargo run --manifest-path cmd-ng/Cargo.toml -- power set 5v_out on
cargo run --manifest-path cmd-ng/Cargo.toml -- power set 5v_out off
cargo run --manifest-path cmd-ng/Cargo.toml -- power set 5v_ws on
cargo run --manifest-path cmd-ng/Cargo.toml -- power set 20v_out on
```

Read current-monitor ADC channels:

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- adc read
cargo run --manifest-path cmd-ng/Cargo.toml -- adc read 5v_out
cargo run --manifest-path cmd-ng/Cargo.toml -- adc record /tmp/adc.ndjson 1000 --rate-hz 250
cargo run --manifest-path cmd-ng/Cargo.toml -- adc read -v 5v_out
cargo run --manifest-path cmd-ng/Cargo.toml -- adc read 12v_out
cargo run --manifest-path cmd-ng/Cargo.toml -- adc read 20v_out
```

Human-readable ADC output is concise by default, for example
`5v_out=0.540000A`. Use `-v` / `--verbose` when you need debug fields such as
`signal` and `mv`. `agent-debugboardctl --json adc read` carries the raw
diagnostic chain (`raw`, `mv`, `current_ua`, `sensor_value`) in addition
to the power-output state. The host CLI no longer applies host-side ADC
calibration tables or zero-point correction.

Switch routes:

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- switch list
cargo run --manifest-path cmd-ng/Cargo.toml -- switch get sd
cargo run --manifest-path cmd-ng/Cargo.toml -- switch get usb
cargo run --manifest-path cmd-ng/Cargo.toml -- switch route sd usb-reader
cargo run --manifest-path cmd-ng/Cargo.toml -- switch route usb target
```

For functional verification of mux/switch controls, run the commands strictly
sequentially: issue `switch route ...`, wait briefly for hardware settling
(typically a few seconds in local validation), then run `switch get ...`. Avoid
parallel or interleaved opposite-direction tests on the same board when you are
trying to validate stability.

Use safe GPIOs:

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- gpio list
cargo run --manifest-path cmd-ng/Cargo.toml -- gpio set GP13 1
cargo run --manifest-path cmd-ng/Cargo.toml -- gpio set CON_MAS 1
cargo run --manifest-path cmd-ng/Cargo.toml -- gpio input J17_PIN1
cargo run --manifest-path cmd-ng/Cargo.toml -- gpio input GP13
```

Use autonomous firmware watchdog recovery:

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- watchdog status
```

The watchdog is owned by firmware, not the host. Firmware automatically arms
the RP2040 hardware watchdog and only keeps feeding it while core firmware,
the HTTP/API service, and the CDC ACM cmdline fallback are still reporting
healthy local liveness. WebSocket session silence, subscription timeout, and
session expiration do not count as watchdog failures. If core firmware wedges,
the API service stops responding, or the CDC ACM cmdline fallback stops
reporting liveness, firmware stops feeding the watchdog, the RP2040 resets,
and the next earliest boot path enters the standard ROM BOOTSEL mode via the
retained recovery marker. The direct `bootloader` command and the CDC ACM
shell fallback remain separate and unchanged. Periodic memory diagnostics are
log-only debug output and do not add watchdog participants or change BOOTSEL
marker/reset behavior. The watchdog trace line is also log-only and does not
change feed policy.

## OpenOCD / JTAG

Agent DebugBoard can be used together with OpenOCD by using the DebugBoard for
target power and recovery control while the onboard CH347F path handles target
JTAG/SWD. The CH347F is wired directly to the target debug connector; RP2040
does not sit in that path and does not act as a CMSIS-DAP or JTAG probe.

Install OpenOCD, then verify it:

```sh
openocd --version
```

Power the target, then start OpenOCD with the CH347F interface script available
in your OpenOCD installation and the target configuration for the board under
test:

```sh
agent-debugboardctl --json power set 5v_out on
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

The firmware enumerates as a composite USB device. The host CLI connects to
the board via HTTP over the USB NCM interface. The default device URL is
`http://172.29.203.1:8080`. The board runs a DHCPv4 server on the NCM link so the
host can automatically obtain a compatible address; pass
`agent-debugboardctl --url ...` only if you intentionally override the default
addressing in your environment.

All control is performed through `agent-debugboardctl` using HTTP JSON
requests, or directly through `curl` / another HTTP client. The CDC ACM port is
kept intentionally as a secondary path for Zephyr cmdline access and recovery
workflows such as BOOTSEL fallback; it is not the primary automation/control
transport. When the CDC ACM shell is available, the local `bootloader` shell
command enters the same RP2040 ROM BOOTSEL path used by the HTTP API.

For long-lived telemetry and bidirectional control over a single socket, create
a live session over HTTP first and then connect to the returned dedicated
WebSocket URL under `/api/v1/ws/<slot>`. WebSocket clients can observe watchdog
status in status snapshots, but they do not feed the watchdog; firmware
supervision is autonomous.

mDNS is intentionally not part of the first-class workflow yet. DHCP already
solves the plug-and-play addressing problem across operating systems; mDNS is a
possible future enhancement for friendlier naming, not a requirement for
normal use.

## Hardware Mapping

| Function | Firmware name | Schematic signal |
| --- | --- | --- |
| 12 V output enable | `12v_out` | `GP02_12V_EN` |
| 5 V output enable | `5v_out` | `GP05_5V_EN` |
| 5 V WS enable | `5v_ws` | `GP09_5V_WS_EN` |
| 20 V output enable | `20v_out` | `GP10_20V_EN` |
| TF/SD route switch | `switch sd` | `GP06_TF_SW` |
| USB mux switch | `switch usb` | `GP03_USB_MUX` |
| 5 V current monitor | `adc read 5v_out` | `S_C_5V` |
| 12 V current monitor | `adc read 12v_out` | `S_C_12V` |
| 20 V current monitor | `adc read 20v_out` | `S_C_20V` |

The current monitor channels use INA139 with a 10 mOhm shunt, 51 kOhm output
load, and 1000 uA/V transconductance. The MCU reports raw ADC diagnostics plus
standard sensor current values from Zephyr's `current-sense-amplifier`
interface, and the host CLI now presents those values directly without any
host-side calibration table or zero-point correction.
See the public
[TI INA139 datasheet](https://www.ti.com/product/INA139) for the sensor
transfer function.

The current schematic copy is stored at
[doc/agent-debugboard-schematic.pdf](doc/agent-debugboard-schematic.pdf).

## Development

Run unit tests:

```sh
./apps/agent_debugboard/tests/run_unit_tests.sh
```

The test runner covers:

- host C tests for the shared board model.
- Go tests for the host CLI helper. The repo-local test script targets the real
  Go source trees (`./cmd/...` and `./internal/...`) instead of `go test ./...`
  from the repository root, so Zephyr/CMake build outputs under `build/` do not
  get swept into Go package discovery.

## Repository Layout

```text
apps/agent_debugboard/        Zephyr application
apps/agent_debugboard/src/    Firmware source and shared board model
apps/agent_debugboard/tests/  Unit tests
cmd/agent-debugboardctl/      Go host CLI entrypoint
internal/hostcli/             Go host CLI implementation
doc/                          Hardware documents, OpenOCD configs, and marketing assets
skills/agent-debugboard/      Agent-facing skill and operating guide
.goreleaser.yaml              GoReleaser host CLI packaging config
go.mod, go.sum                Go module for host CLI
west.yml                      Zephyr workspace manifest
```
