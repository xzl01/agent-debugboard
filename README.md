# radxa-linkr-debugger

[中文](README.zh-CN.md)

RP2350 firmware for **Radxa Linkr Debugger**, a USB-controlled hardware
bridge that lets a PC-side Agent/AI operate target-board power, boot-mode, TF/SD
routing, current-monitor ADC channels, and a small safe GPIO surface.

![Radxa Linkr Debugger promo](doc/marketing/radxa-linkr-debugger-promo.png)

## Overview

Radxa Linkr Debugger is designed for automated board bring-up, recovery, production
test, and remote debugging workflows. The firmware enumerates as a composite
USB device with a USB NCM network interface for the main control plane and a
USB CDC ACM serial port reserved for Zephyr cmdline and BOOTSEL fallback; normal
host-side workflows use the released Rust `radxa-linkr-debuggerctl` CLI/TUI,
whose source lives under `cmd-ng/`.

This repository contains the Zephyr application, the primary Rust host CLI/TUI,
unit tests, schematic copy, and project documentation.

The active host-side development path is [`cmd-ng/`](cmd-ng/). Released user
workflows should use the published Rust `radxa-linkr-debuggerctl` CLI, which
speaks the board HTTP API over USB NCM.

## Features

| Area | Supported in this firmware |
| --- | --- |
| USB control | Composite USB device: NCM HTTP/WS control plane + CDC ACM fallback console |
| Host automation | Rust `cmd-ng` CLI/TUI with JSON output and `doctor` diagnostics |
| Embedded Web UI | Gzip-compressed dashboard served directly from `http://172.29.203.1/` |
| Live telemetry | Bidirectional WebSocket stream on a live-session URL under `/api/v1/ws/<slot>` |
| Triggered power capture | Device-timestamped current capture with pre/post ring buffer, manual/current/GPIO/power-on triggers, and CSV/NDJSON export |
| Logic analyzer | RP2350 PIO2+DMA high-speed single-shot capture; 1-125MHz requested rates; 512-sample bursts; safe pins GP7-GP20/GP29; none/rising/falling/either PIO triggers; pre-trigger sampling (edge triggers, ≤25 MHz); continuous streaming mode (1-25 MHz) with live-session `logic-chunk` WebSocket delivery and a rolling in-browser live waveform; CSV/PulseView (.sr) export; actual rate and period in response metadata |
| Power outputs | `12v_out`, `5v_out`, `20v_out` |
| ADC monitor | Current monitor reads for `5v_out`, `12v_out`, `20v_out` |
| Board self-monitoring | `/api/v1/status` and status WebSocket snapshots report board CPU/runtime/heap/memory/temperature availability and values when Zephyr exposes reliable sources; memory reports additive `current_pressure` and `peak_pressure` objects using max-not-sum semantics across system heap, network packet slabs, and data buffer pools, with the legacy root `pressure_pct_x100` preserving Phase 1 max(heap, stack) backward compatibility; the watchdog supervisor also prints periodic heap diagnostics for short-reset debugging |
| TF/SD routing | Switch route between `target` and `usb-reader` |
| VIN control | Switch route between `1.8v` and `3.3v`; firmware uses a Device Tree VIO regulator |
| GPIO | `GP7`, `GP8`, `GP9`, `GP10`-`GP20`, `GP29` |
| Captive portal discovery | DHCPv4 option 114, wildcard A, AAAA NOERROR/NODATA on UDP 53, and HTTP port 80 listener maximize OS auto-open probability; port 80/DNS are compatibility helpers; auto-open is best-effort, not guaranteed |
| Autonomous watchdog recovery | Firmware-supervised watchdog resets into ROM BOOTSEL when core services stop reporting healthy liveness |
| Firmware update | USB command to reboot RP2350 into BOOTSEL |
| MCUboot OTA | Unsigned RP2350-only MCUboot OTA; SHA256 integrity check only; no signature, authentication, secure boot, or anti-rollback; test image auto-confirms after 16-second watchdog health gate; watchdog reset during unconfirmed test period allows MCUboot rollback instead of forcing ROM BOOTSEL |

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

## Install Host CLI

The normal host-side workflow uses the released `radxa-linkr-debuggerctl` CLI.
Download the matching archive from GitHub Releases, or from a checkout use the
repo-local installer scripts below with an explicit version so they fetch the
published release artifact instead of building from source.

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
Single-sample firmware telemetry keeps `sequence` and `uptime_us` and also emits
`sample_sequence` plus `device_t_mono_us`. Compact batch samples carry
`sequence` and `uptime_us`; the recorder normalizes those fields to the same
timing aliases while accepting explicit aliases from compatible firmware. It preserves device timing under
`metadata.device_timing`, uses `device_t_mono_us` for CSV when present, falls
back to `uptime_us` and then zero, and reports ring overruns as
`metadata.dropped_samples` on the first affected row.

Power-analyzer captures add a firmware ring buffer and device monotonic
timestamps. The RP2350 keeps 2048 samples. The Web UI can arm manual,
current-threshold, GPIO-edge, or power-on captures, overlay four runs, and
export CSV or NDJSON. See [doc/power-analyzer.md](doc/power-analyzer.md).

## Logic Analyzer

The firmware logic analyzer uses RP2350 PIO2+DMA for high-speed single-shot
capture. It is intended for short-burst diagnostics at PIO rates, not sustained
streaming. HTTP configuration accepts up to 16 GPIO channels from the safe pin
allowlist, a requested `sample_rate_hz` from 1,000,000 through 125,000,000
(1-125MHz), and `post_samples` from 1 through 512. The capture is capped at 512
samples.

Safe GPIO pins are: `GP7` (`CON_MAS`), `GP8` (`CON_REST`), `GP9` (`CON_USER`),
`GP10`-`GP20` (J16 header), and `GP29` (ADC3). No other pins are available for
logic analyzer capture.

Supported trigger modes are `none`, `rising`, `falling`, and `either`. PIO
triggers wait for the selected edge before capture starts. Pre-trigger
sampling is supported for edge triggers at ≤25 MHz: set `pre_samples > 0`
with an edge trigger to capture samples before and after the trigger edge
(capped at 512 total). `pre_samples > 0` with `trigger: "none"` or with
`sample_rate_hz > 25000000` is rejected with HTTP 400 `invalid_config`.
Repeated `POST /api/v1/logic-analyzer` while the analyzer is
`armed` or `capturing` returns HTTP 409 with `error.code: "already_armed"`.
Invalid JSON, invalid parameters, and unsupported configurations return HTTP 400
with `error.code: "invalid_config"`; true internal arm failures remain HTTP 500
with `error.code: "arm_failed"`.

The arm response includes `requestedSampleRateHz`, `actualSampleRateHz`,
`samplePeriodPs`, and `backend`. The capture response additionally includes
`sampleCount`, `triggerIndex`, and a `config` object with the full capture
configuration and samples. 50MHz and 125MHz are very short single-shot bursts;
the firmware does not claim sustained streaming at those rates.

The logic analyzer is available in the Web UI under the **Terminal workspace**,
alongside the serial terminal. Captured samples can be decoded in the browser using
the project-owned Rust/WASM logic decoder: the stable decoder URLs are
`/assets/decoder/logic-decoder.js` and `/assets/decoder/logic-decoder_bg.wasm`,
served with `application/wasm` MIME for the WASM asset and gzip-compressed.
The decoder supports UART, I2C, and SPI protocols only; it is not a
libsigrokdecode Python plugin compatibility layer. For PulseView compatibility,
export captures in .sr format and open them directly in PulseView with the
configured sample rate. See [doc/logic-analyzer.md](doc/logic-analyzer.md).

The canonical firmware build at the first integrated measurement used 605476 of
847832 available flash bytes (71.41%); no A/B repartition was needed.
Actual usage varies with configuration and build options.

## Build Firmware

Firmware builds include the production Web UI and board-hosted protocol decoder.
CMake runs the locked Web/WASM build and embeds its gzip-compressed output
automatically. The following tools are required:

| Tool | Version | Purpose |
|---|---|---|
| cmake, ninja, dtc, gperf | — | Zephyr build system |
| python3 + west | ≥1.5 | Zephyr meta-tool |
| python3 intelhex, click, cbor2 | — | MCUboot image tools |
| nodejs 22 + npm | 22.x | Web UI build |
| rustc + cargo | stable | Rust CLI + WASM decoder |
| wasm-bindgen-cli | 0.2.121 | WASM decoder glue |
| Zephyr SDK | 1.0.1 | ARM cross-compiler |

### Nix (recommended)

The repo provides a `shell.nix` that bundles all dependencies. Set
`ZEPHYR_SDK_INSTALL_DIR` to your local Zephyr SDK path before entering:

```sh
export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-1.0.1
nix-shell
```

Inside the shell, initialize the west workspace once:

```sh
scripts/setup-zephyr.sh
```

Then build:

```sh
west build -p always -b rpi_pico2/rp2350a/m33/mcuboot --sysbuild apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
```

### Manual (without Nix)

Create a Python environment and fetch Zephyr:

```sh
python3 -m venv .venv
source .venv/bin/activate
pip install -U pip west

west init -l .
west update
west zephyr-export
pip install -r zephyr/scripts/requirements.txt
pip install -r bootloader/mcuboot/scripts/requirements.txt
```

Install the Zephyr SDK if it is not already installed. The current local build
has been verified with Zephyr SDK `1.0.1`.

Build:

```sh
source .venv/bin/activate
west build -p always -b rpi_pico2/rp2350a/m33/mcuboot --sysbuild apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
```

For RP2350 sysbuild, the application artifacts are under:

```text
build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin
build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.hex
```

The `zephyr.signed.bin` filename is Zephyr/MCUboot's format name; with this
project configuration it is an unsigned MCUboot-format application binary for
OTA, not a cryptographically signed image.

For this repository, keep the firmware build/flash path fixed: always build
into `build/radxa_linkr_debugger/`. For RP2350 initial install or recovery, use
the combined MCUboot plus application UF2 published as
`radxa-linkr-debugger-rp2350.uf2`. Do not switch to alternate build directories
or stale UF2 copies from temporary mount points.

## Flashing

Firmware can be updated in two ways depending on the current state of the board.

### ROM BOOTSEL flashing (initial install or recovery)

If the board is already running this firmware, ask it to enter BOOTSEL and then
load the new combined RP2350 UF2:

```sh
radxa-linkr-debuggerctl bootloader
picotool load -v -x radxa-linkr-debugger-rp2350.uf2
```

After firmware changes, treat this BOOTSEL flow and the CDC ACM shell
fallback below as required validation paths; do not conclude the change until
you have verified the serial fallback path still works.

If HTTP/WS control is unavailable but the MCU CDC ACM shell is still up, you
can enter the same BOOTSEL path from the local Zephyr shell:

```text
linkr-debugger:~$ bootloader
```

If the board is already mounted as `RPI-RP2`, only run:

```sh
picotool load -v -x radxa-linkr-debugger-rp2350.uf2
```

On Linux you can also flash without root by using `udisksctl` to mount the
`RPI-RP2` volume and then copying the canonical UF2:

```sh
RPI_RP2=$(udisksctl mount -b /dev/sdX1 | awk -F" at " '{print $2}' | tr -d '[:space:]')
cp radxa-linkr-debugger-rp2350.uf2 "$RPI_RP2/"
```

Replace `/dev/sdX1` with the actual BOOTSEL block device path on your
system (use `lsblk -o NAME,SIZE,VENDOR,MOUNTPOINT` and look for the `RPI` vendor
entry). If you use drag-and-drop flashing through the `RPI-RP2` volume instead of
`picotool`, copy this same initial-install/recovery artifact:

```text
radxa-linkr-debugger-rp2350.uf2
```

### OTA flashing (after initial MCUboot install)

After the MCUboot-capable firmware is installed on RP2350, subsequent firmware
updates can be delivered via OTA using a MCUboot-format application binary.
Upload the firmware binary, trigger a test boot, and confirm:

```sh
radxa-linkr-debuggerctl ota upload /path/to/firmware.bin
radxa-linkr-debuggerctl ota test
# After verifying the test boot succeeded:
radxa-linkr-debuggerctl ota confirm
```

Or, after a successful test boot, wait for the 16-second watchdog health gate
to auto-confirm the image. If the test image is not confirmed and the watchdog
resets, the retained marker drives MCUboot rollback rather than ROM BOOTSEL.

Do not upload a `.uf2` or `.elf` file via OTA. OTA expects a MCUboot-format
application binary. Use the release asset `radxa-linkr-debugger-rp2350-ota.bin`,
which is copied from the sysbuild application output
`build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin`.
Despite the `signed.bin` build filename, this project config uses unsigned
MCUboot format.

## GitHub Actions Artifacts

The `Build` workflow checks every push and pull request. Tagging `v*` triggers
the `Release` workflow, which builds firmware, packages the host CLI, creates a
GitHub Release, and uploads the fixed release assets.

- `radxa-linkr-debugger-rp2350.uf2`: combined RP2350 MCUboot plus application firmware for initial install, recovery, drag-and-drop, or `picotool`.
- `radxa-linkr-debugger-rp2350-ota.bin`: RP2350 OTA application payload copied from sysbuild `zephyr.signed.bin`; unsigned MCUboot format under this project config.
- `radxa-linkr-debugger-rp2350.elf`: RP2350 ELF for debugging.
- `radxa-linkr-debugger-rp2350.map`: RP2350 linker map.
- `radxa-linkr-debuggerctl-rust_windows_amd64.zip`: primary Rust CLI/TUI for Windows x64.
- `radxa-linkr-debuggerctl-rust_linux_amd64.tar.gz`: primary Rust CLI/TUI for Linux x64.
- `radxa-linkr-debuggerctl-rust_darwin_arm64.tar.gz`: primary Rust CLI/TUI for macOS Apple Silicon.
- `skills-radxa-linkr-debugger.tar.gz`: Agent skill bundle for `skills/radxa-linkr-debugger/`.
- `SHA256SUMS.txt`: SHA256 checksums for all release assets.

Normal users should download one of the `radxa-linkr-debuggerctl-rust_*`
archives above. If you are developing `cmd-ng` itself from source:

```sh
cargo build --manifest-path cmd-ng/Cargo.toml
./cmd-ng/target/debug/radxa-linkr-debuggerctl --help
```

## CLI Usage

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
`heap`, `memory`, `runtime`, and `cpu`) carries `available` plus a machine-readable
`reason`. Firmware only reports values from Zephyr devices or runtime-stat APIs
that are actually enabled; on the default RP2350 configuration this includes the
internal CPU die temperature sensor, system heap runtime statistics, real board
uptime (`uptime_ms` / `uptime_seconds`), CPU utilization deltas, and the Phase 2
additive memory pressure objects. The first CPU sample can still report `insufficient_runtime_window`
until the board has accumulated enough runtime delta to derive a percentage.

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

Rust and Web clients prefer `current_pressure` when available, fall back to the legacy root `pressure_pct_x100` for Phase 1 compatibility, and fall back again to heap-only when `memory` is absent from the status response entirely. Old firmware without `memory` is handled by the Rust CLI falling back to heap-only display, and the Web UI falling back to heap free space. The `memory` source is `zephyr` when emitted.

WebSocket `snapshot/status` messages carry the same `board_monitoring` object as `GET /api/v1/status`.

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

VIN control:

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
radxa-linkr-debuggerctl gpio input J16_PIN1
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

On RP2350A boards, the blue status LED on GPIO25 acts as a watchdog
heartbeat. It blinks approximately once per second and advances only after a
successful hardware watchdog feed; skipped or failed feeds reset it to the
inactive state while firmware owns the GPIO.

## MCUboot OTA Firmware Update

The firmware supports unsigned MCUboot over-the-air firmware update.

**Important security facts**: This OTA path provides no signature verification,
no authentication, no secure boot, and no anti-rollback protection. Any host
with USB NCM access to the board can submit a firmware image. SHA256 is used
only to verify the integrity of the uploaded payload, not to authenticate the
sender.

### Initial installation

Initial installation of the MCUboot-capable firmware requires ROM BOOTSEL
flashing with a combined bootable artifact. After the first MCUboot install,
subsequent updates can be delivered via OTA using MCUboot-format application
binaries.

### OTA workflow

The OTA workflow has three steps:

1. Upload the MCUboot-format application binary to the board.
2. Request a test boot of the newly uploaded image.
3. Confirm the image after verifying it boots correctly, or let the 16-second
   watchdog health gate auto-confirm it.

If the test image is not confirmed and a watchdog reset occurs, the dedicated
retained marker allows MCUboot to perform a rollback to the previous confirmed
image instead of forcing entry into ROM BOOTSEL. Explicit `bootloader` commands
and ordinary non-OTA watchdog resets still enter ROM BOOTSEL normally.

### CLI commands

```sh
radxa-linkr-debuggerctl ota status
radxa-linkr-debuggerctl ota upload /path/to/firmware.bin
radxa-linkr-debuggerctl ota test
radxa-linkr-debuggerctl ota confirm
```

`ota status` reports the current OTA state (`idle`, `uploading`, `verified`,
`pending_test`, `rebooting`, `failed`), flash sizes, and MCUboot swap type.
`ota upload` sends a MCUboot-format `.bin` file; the CLI computes the SHA256
and sends it along with the byte size as headers. `ota test` requests a
test boot of the verified image; the board reboots after a short delay. If
the watchdog reports healthy after the test boot, the image auto-confirms
after a 16-second gate. `ota confirm` manually confirms the running image
immediately, clearing the auto-confirm timer.

Agent or automation code should prefer JSON output:

```sh
radxa-linkr-debuggerctl --json ota status
radxa-linkr-debuggerctl --json ota upload /path/to/firmware.bin
radxa-linkr-debuggerctl --json ota test
radxa-linkr-debuggerctl --json ota confirm
```

## OpenOCD / JTAG

Radxa Linkr Debugger can be used together with OpenOCD by using the Linkr Debugger for
target power and recovery control while the onboard CH347F path handles target
JTAG/SWD. The CH347F is wired directly to the target debug connector; RP2350
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
interface, with the default device URL `http://172.29.203.1`. The board
runs a DHCPv4 server on the NCM link so the host can automatically obtain a
compatible address; pass `radxa-linkr-debuggerctl --url ...` only if you
intentionally override the default addressing in your environment.

Normal user workflows should use the released CLI, which wraps the same HTTP
JSON API. Direct `curl` is mainly for raw API debugging or for the Agent skill,
which intentionally stays curl-first. The CDC ACM port is kept intentionally as
a secondary path for Zephyr cmdline access and recovery workflows such as
BOOTSEL fallback; it is not the primary automation/control transport. When the
CDC ACM shell is available, the local `bootloader` shell command enters the
same MCU ROM BOOTSEL path used by the HTTP API.

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

## Captive Portal Discovery

The firmware implements a multi-path captive portal detection helper that
maximizes the probability of an OS opening the board Web UI automatically when
a host connects to the NCM link.

**DHCP**: the DHCPv4 server running on the NCM interface advertises the router
and DNS address as `172.29.203.1`. It also sends DHCP option 114 (Captive Portal
URI) with the value `http://172.29.203.1/captive-portal/api`. This is a
compatibility-oriented HTTP endpoint, not a trusted HTTPS signal.

**DNS**: the firmware runs a lightweight DNS responder bound to the NCM interface
on UDP port 53. For any incoming query, it returns a wildcard A record
pointing to `172.29.203.1`. For AAAA queries it returns a NOERROR response with
zero answers (NODATA). There is no recursion, forwarding, or caching; responses
are served only for names queried against this NCM-local server. The DNS TTL is
set to 30 seconds to allow host caches to expire relatively quickly.

**HTTP port 80**: a single Zephyr HTTP service bound to the NCM-local
`172.29.203.1` address on port 80 routes by URL path:
`/`, `/assets/*`, `/api/v1/*`, `/api/v1/ws/*`, `/captive-portal/api`, and
legacy detection probes. When a GET request arrives at `/captive-portal/api`,
it responds with HTTP 200 and a JSON body carrying `Content-Type:
application/captive+json`. The body contains
`{"captive":true,"user-portal-url":"http://172.29.203.1/","venue-info-url":"http://172.29.203.1/"}`.
For any other GET path not matching the routed paths, the server returns
HTTP 302 with a `Location` header pointing to `http://172.29.203.1/`. Unknown API
paths return JSON 404. Other HTTP methods receive HTTP 405.

The pinned Zephyr HTTP/1 server handles `HEAD` for dynamic resources before the
application callback and returns its default headers-only HTTP 200 response.
Captive portal diagnostics and compatibility claims therefore use `GET`.

**Auto-open probability**: most modern operating systems check for captive
portals by performing a DNS lookup for a known detection name and then making
an HTTP request to the returned address. The combination of DHCP option 114,
wildcard DNS A records, and a port 80 listener that answers the expected
`/captive-portal/api` path is designed to maximize the chance that the OS opens
the board Web UI automatically. This is a best-effort mechanism, not a
guarantee. Results vary by operating system version, network configuration,
and whether the host has other active network interfaces; a host with a
preferred default route over a different adapter may not trigger the detection
sequence. Because the NCM lease advertises a default router and DNS server,
hosts may also change route or DNS priority while the board is connected;
multi-homed deployments should verify that their existing Internet path remains
preferred. Users who need the Web UI can always open `http://172.29.203.1/`
directly in a browser, use `curl`, or rely on the CLI/TUI.

## Hardware Mapping

### RP2350A Revision

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

GPIO25 is the blue status LED, active-low. It operates as a watchdog
heartbeat driven through Device Tree chosen properties rather than a Zephyr built-in
heartbeat driver or `CONFIG_LED`. The LED blinks at approximately 1 Hz (full on/off
cycle) and advances only after the hardware watchdog feed succeeds. Skipped or failed
feeds reset the LED to the inactive state while firmware owns the GPIO.

VIN defaults to 3.3V at boot. GPIO1 VDD_5V and its GPIO6 VDD_1V8 child rail are
always on in the Device Tree model. The selectable CH347 VIO level is modeled
as a standard `regulator-gpio` regulator with exact 1.8V and 3.3V states, and
firmware selects it through the Zephyr regulator API. Confirm your target
supports the selected voltage before applying it.

The current monitor channels use INA139 with a 10 mOhm shunt and 50 kOhm
output load. The MCU reports raw ADC diagnostics plus
standard sensor current values from Zephyr's `current-sense-amplifier`
interface, and the host CLI now presents those values directly without any
host-side calibration table or zero-point correction.
See the public
[TI INA139 datasheet](https://www.ti.com/product/INA139) for the sensor
transfer function.

The current schematic copy is stored at:
- [doc/radxa-linkr-debugger-schematic-x1.1.pdf](doc/radxa-linkr-debugger-schematic-x1.1.pdf)

The older G2 (RP2040) schematic at `doc/radxa-linkr-debugger-schematic.pdf` is
retained as archival reference only; G2 hardware is not supported by this firmware.

## Development

Run unit tests:

```sh
./apps/radxa_linkr_debugger/tests/run_unit_tests.sh
```

The test runner covers host C tests for the shared board model. Rust host CLI
checks live under `cmd-ng/` and are run with Cargo.

## Repository Layout

```text
apps/radxa_linkr_debugger/        Zephyr application
apps/radxa_linkr_debugger/src/    Firmware source and shared board model
apps/radxa_linkr_debugger/tests/  Unit tests
cmd-ng/                          Primary Rust host CLI/TUI
doc/                          Hardware documents, OpenOCD configs, and marketing assets
skills/radxa-linkr-debugger/      Agent-facing skill and operating guide
west.yml                      Zephyr workspace manifest
```
