# radxa-linkr-debugger

[中文](README.zh-CN.md)

RP2350-series firmware for **Radxa Linkr Debugger**, a USB-controlled hardware
bridge that lets a PC-side Agent/AI operate target-board power, boot-mode,
TF/SD routing, current-monitor ADC channels, and a small safe GPIO surface.

The actively supported hardware is G3 with RP2350A. G2/RP2040 support is
retired: it receives no firmware builds, release artifacts, compatibility
fixes, or hardware validation. RP2354 hardware requires a dedicated board
definition and HIL validation before it can be declared supported.

![Radxa Linkr Debugger promo](doc/marketing/radxa-linkr-debugger-promo.png)

## Features

| Area | Supported |
| --- | --- |
| USB control | Composite USB: NCM HTTP/WS + CDC ACM fallback |
| Host automation | Rust CLI/TUI with JSON output |
| Web UI | Dashboard at http://172.29.203.1/ |
| Logic analyzer | PIO2+DMA, 100 kHz–125 MHz, WebSocket/raw-TCP Sigrok with a [common packed arena and current WIDE11 capture](doc/logic-analyzer.md) |
| Power analyzer | Triggered capture with ring buffer, CSV/NDJSON export |
| Power outputs | `12v_out`, `5v_out`, `20v_out`, `vdd_5v` |
| ADC monitor | Current readings for `5v_out`, `12v_out`, and `20v_out` plus GP29/ADC3 voltage telemetry; see [doc/adc-telemetry.md](doc/adc-telemetry.md) |
| Switch routes | Firmware-advertised TF/SD, USB hub mux, TF write-protect (`writable`/`protected`), VIN (1.8V/3.3V) |
| GPIO | `GP7`–`GP20`; `GP29` remains cataloged but is input-only while owned by `adc3` (see [GP29 ownership](doc/adc-telemetry.md#gp29-ownership)) |
| OTA update | MCUboot unsigned OTA |
| Watchdog | Autonomous recovery to BOOTSEL |
| Captive portal | DHCP option 114/HTTP auto-open for Web UI |

WIDE11 uses a 144184 B hardware slice and a 30720 B WS telemetry ring within the 149048 B total backing allocation.

`5V_FIN` is intentionally treated as a separate input/source power input. It is
not exposed as a controllable output.

## Quick Start

1. [Install the CLI](docs/user/install.md)
2. Connect the board via USB
3. Run `radxa-linkr-debuggerctl doctor`
4. Open http://172.29.203.1/ for the Web UI

```sh
radxa-linkr-debuggerctl --version
radxa-linkr-debuggerctl doctor
radxa-linkr-debuggerctl status
```

## Documentation

- **[User Guide](docs/user/)** — CLI, TUI, WebUI, OTA, OpenOCD, Logic Analyzer, Power Analyzer
- **[Developer Guide](docs/developer/)** — Build, flash, contribute, hardware mapping

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

## Repository Layout

```text
apps/radxa_linkr_debugger/        Zephyr application
apps/radxa_linkr_debugger/src/    Firmware source and shared board model
apps/radxa_linkr_debugger/tests/  Unit tests
cmd-ng/                          Primary Rust host CLI/TUI
web/                             Web UI and device bridge
doc/                          Hardware documents, schematics, and marketing assets
skills/radxa-linkr-debugger/      Agent-facing skill and operating guide
west.yml                      Zephyr workspace manifest
```
