# agent-debugboard

[中文](README.zh-CN.md)

RP2040 firmware for **Agent DebugBoard**, a USB-controlled hardware bridge that
lets a PC-side Agent/AI operate target-board power, boot-mode, TF/SD routing,
current-monitor ADC channels, and a small safe GPIO surface.

![Agent Debugger promo](doc/marketing/agent-debugger-promo.png)

## Overview

Agent DebugBoard is designed for automated board bring-up, recovery, production
test, and remote debugging workflows. The firmware exposes a USB CDC ACM shell
named `Agent DebugBoard`, plus a host-side native Go CLI that turns board
operations into scriptable commands.

This repository contains the Zephyr application, host helper, unit tests,
schematic copy, and project documentation.

## Features

| Area | Supported in this firmware |
| --- | --- |
| USB control | CDC ACM shell with `debugboard` commands |
| Host automation | `agent-debugboardctl` CLI with JSON output and `doctor` diagnostics |
| Power rails | `12v_out`, `5v_out`, `5v_ws`, `20v_out` |
| ADC monitor | Current monitor reads for `5v_out`, `12v_out`, `20v_out` |
| TF/SD routing | Switch route between `target` and `usb-reader` |
| GPIO | Safe allowlist: `GP13`, `GP14`, `GP15`, `GP22`, `GP23`, `GP24` |
| Firmware update | USB command to reboot RP2040 into BOOTSEL |

`5V_FIN` is intentionally treated as a separate input/source rail. It is not
exposed as a controllable output rail.

## For AI Agents

AI agents should read [skills/agent-debugboard/SKILL.md](skills/agent-debugboard/SKILL.md)
before operating hardware through this project. The skill is the canonical
Agent-facing procedure for installing `agent-debugboardctl`, diagnosing the
board connection, and using JSON commands safely.

Recommended agent flow:

```sh
agent-debugboardctl --version
agent-debugboardctl --json doctor
agent-debugboardctl --json status
```

If `agent-debugboardctl` is not installed, follow the install commands in the
skill first. For automation, prefer `agent-debugboardctl --json ...`; parse
`schema`, `ok`, `command`, and `error.code` instead of human-readable text.

## Install Host CLI

`agent-debugboardctl` is a native Go binary. Users do not need Python, pip, or a
virtual environment.

From a checkout, install the host CLI into the skill-local `scripts/bin`
directory on macOS or Linux:

```sh
./skills/agent-debugboard/scripts/install.sh
```

Install a specific release version:

```sh
./skills/agent-debugboard/scripts/install.sh --version <tag>
```

For a private repository release fallback, export a GitHub token first.
`gh auth token` works if the GitHub CLI is logged in:

```sh
export GH_TOKEN="$(gh auth token)"
./skills/agent-debugboard/scripts/install.sh
```

Windows PowerShell:

```powershell
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\skills\agent-debugboard\scripts\install.ps1
```

Private repository PowerShell release fallback:

```powershell
$env:GH_TOKEN = gh auth token
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\skills\agent-debugboard\scripts\install.ps1
```

Manual downloads are also available from each GitHub Release:

| OS / CPU | Artifact |
| --- | --- |
| Windows x64 | `agent-debugboardctl_windows_amd64.zip` |
| Windows arm64 | `agent-debugboardctl_windows_arm64.zip` |
| Linux x64 | `agent-debugboardctl_linux_amd64.tar.gz` |
| Linux arm64 | `agent-debugboardctl_linux_arm64.tar.gz` |
| macOS Intel | `agent-debugboardctl_darwin_amd64.tar.gz` |
| macOS Apple Silicon | `agent-debugboardctl_darwin_arm64.tar.gz` |

On macOS, unsigned release binaries may trigger a Gatekeeper warning saying Apple
cannot verify the software. The installer verifies `SHA256SUMS.txt` first and
then removes the quarantine flag from the installed binary. If installing
manually, verify the checksum and run:

```sh
xattr -dr com.apple.quarantine ./agent-debugboardctl
```

After installation:

```sh
./skills/agent-debugboard/scripts/bin/agent-debugboardctl --help
./skills/agent-debugboard/scripts/bin/agent-debugboardctl --version
./skills/agent-debugboard/scripts/bin/agent-debugboardctl doctor
```

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

## Flashing

If the board is already running this firmware, ask it to enter BOOTSEL and then
load the new UF2:

```sh
agent-debugboardctl bootloader
picotool load -v -x build/agent_debugboard/zephyr/zephyr.uf2
```

If the board is already mounted as `RPI-RP2`, only run:

```sh
picotool load -v -x build/agent_debugboard/zephyr/zephyr.uf2
```

## GitHub Actions Artifacts

The `Build` workflow checks every push and pull request. Tagging `v*` triggers
the `Release` workflow, which builds firmware, packages the host CLI, creates a
GitHub Release, and uploads the fixed release assets.

- `agent-debugboard-rp2040.uf2`: RP2040 firmware for drag-and-drop or `picotool`.
- `agent-debugboard-rp2040.elf`: RP2040 ELF for debugging.
- `agent-debugboard-rp2040.map`: RP2040 linker map.
- `agent-debugboardctl_windows_amd64.zip`: native Windows x64 CLI.
- `agent-debugboardctl_windows_arm64.zip`: native Windows arm64 CLI.
- `agent-debugboardctl_linux_amd64.tar.gz`: native Linux x64 CLI.
- `agent-debugboardctl_linux_arm64.tar.gz`: native Linux arm64 CLI.
- `agent-debugboardctl_darwin_amd64.tar.gz`: native macOS Intel CLI.
- `agent-debugboardctl_darwin_arm64.tar.gz`: native macOS Apple Silicon CLI.
- `SHA256SUMS.txt`: SHA256 checksums for all release assets.

Developers can build the host CLI from source:

```sh
go build -o agent-debugboardctl ./cmd/agent-debugboardctl
./agent-debugboardctl --help
```

## Host Usage

Query board status:

```sh
agent-debugboardctl status
agent-debugboardctl doctor
```

Agent or automation code should prefer JSON output. JSON responses use
`schema: "agent-debugboard.v1"`, `ok`, `command`, and either command-specific
fields or `error: {code, message}`:

```sh
agent-debugboardctl --json doctor
agent-debugboardctl --json status
agent-debugboardctl --json rail list
agent-debugboardctl --json adc read
agent-debugboardctl --json gpio list
```

Control rails:

```sh
agent-debugboardctl rail set 12v_out on
agent-debugboardctl rail set 12v_out off
agent-debugboardctl rail set 5v_out on
agent-debugboardctl rail set 5v_out off
agent-debugboardctl rail set 5v_ws on
agent-debugboardctl rail set 20v_out on
```

Read current-monitor ADC channels:

```sh
agent-debugboardctl adc read
agent-debugboardctl adc read 5v_out
agent-debugboardctl adc read -v 5v_out
agent-debugboardctl adc read 12v_out
agent-debugboardctl adc read 20v_out
```

Human-readable ADC output is concise by default, for example
`5v_out=540mA`. Use `-v` / `--verbose` when you need debug fields such as
`signal`, `raw`, and `mv`. JSON output keeps the full structured data.

Switch TF/SD route:

```sh
agent-debugboardctl sd route target
agent-debugboardctl sd route usb-reader
```

Use safe GPIOs:

```sh
agent-debugboardctl gpio list
agent-debugboardctl gpio set GP13 1
agent-debugboardctl gpio input GP13
```

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
agent-debugboardctl --json rail set 5v_out on
openocd -f interface/<ch347-interface>.cfg -f target/<target>.cfg
```

CH347F support depends on the OpenOCD build. If the system OpenOCD package does
not include a CH347F interface script, use the WCH/vendor OpenOCD build or add
the matching interface script.

OpenOCD normally exposes GDB on TCP `3333` and telnet control on TCP `4444`.
Prefer OpenOCD reset commands such as `reset halt` or the target OS reboot path
first. Use rail power-cycling only as a hard-restart fallback.

See [doc/openocd/README.md](doc/openocd/README.md) for the full workflow.

## Direct Shell

Open the CDC serial device and use the `debugboard` shell command:

```text
debugboard status
debugboard status --json
debugboard rail list
debugboard rail list --json
debugboard adc read
debugboard adc read -v 5v_out
debugboard adc read --json
debugboard sd get
debugboard sd get --json
debugboard gpio list
debugboard gpio list --json
debugboard bootloader
```

## Hardware Mapping

| Function | Firmware name | Schematic signal |
| --- | --- | --- |
| 12 V output enable | `12v_out` | `GP02_12V_EN` |
| 5 V output enable | `5v_out` | `GP05_5V_EN` |
| 5 V WS enable | `5v_ws` | `GP09_5V_WS_EN` |
| 20 V output enable | `20v_out` | `GP10_20V_EN` |
| TF/SD route switch | `sd route` | `GP06_TF_SW` |
| 5 V current monitor | `adc read 5v_out` | `S_C_5V` |
| 12 V current monitor | `adc read 12v_out` | `S_C_12V` |
| 20 V current monitor | `adc read 20v_out` | `S_C_20V` |

The current monitor channels use INA139 with a 0.2 mOhm shunt, 100 kOhm output
load, and 1000 uA/V transconductance. The ideal transfer is 20 mV/A at the
RP2040 ADC input, so `1 mV = 50 mA` and `20 mV = 1 A`. The `5v_out` channel uses
a piecewise-linear table from local 0.1 A step measurements, with `mv <= 11`
treated as 0 mA; `12v_out` and `20v_out` keep the ideal model until calibrated.
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
- Go tests for the host CLI helper.

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
