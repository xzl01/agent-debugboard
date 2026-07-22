# TUI Guide

[中文](tui.zh-CN.md)

## Launch

Run the CLI without a subcommand to start the interactive TUI:

```sh
radxa-linkr-debuggerctl
```

## Layout

The TUI control surface is organized into three sections:

- **Power** — `12v_out`, `5v_out`, `20v_out` on/off toggles
- **Switch** — SD switch (`target` / `usb-reader`), USB switch (`pc` / `target`), VIN (`1.8v` / `3.3v`)
- **GPIO** — Safe GPIO pins with output/input modes

The status block shows both the optimistic `desired` switch state (local target) and the backend-confirmed `actual` switch state, which helps diagnose transient or one-direction route failures.

## Navigation

| Key | Action |
| --- | --- |
| Arrow keys / Tab | Move selection |
| Space / Enter | Toggle the selected item |
| `i` | Return selected GPIO to input mode |
| `t` | SD switch → `target` |
| `u` | SD switch → `usb-reader` |

## VIN confirmation

VIN switching requires confirmation because voltage changes have hardware side effects. In the TUI, press Space/Enter on the VIN item and confirm when prompted.

## Multi-instance stability

The TUI maintains a modest 60 Hz redraw cadence and uses HTTP polling for status and ADC reads. This means multiple TUI instances can run simultaneously without interference.

## High-rate capture

For high-rate ADC capture, use `adc record` from the CLI — it creates a live websocket session independent of the TUI. See [CLI Reference](cli.md#recording) for details.
