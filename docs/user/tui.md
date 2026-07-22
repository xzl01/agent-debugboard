# TUI Guide

[中文](tui.zh-CN.md)

Run without a subcommand to start the interactive terminal UI:

```sh
radxa-linkr-debuggerctl
```

## Layout

Three control sections:

- **Power** — `12v_out`, `5v_out`, `20v_out` toggles
- **Switch** — SD (`target` / `usb-reader`), USB (`pc` / `target`), VIN (`1.8v` / `3.3v`)
- **GPIO** — Safe pins with output/input modes

The status block shows both `desired` (local target) and `actual` (backend readback) for each switch, so you can tell if a route change actually took effect.

## Navigation

| Key | Action |
| --- | --- |
| Arrow keys / Tab | Move selection |
| Space / Enter | Toggle current item |
| `i` | Set current GPIO to input mode |
| `t` | SD switch → `target` |
| `u` | SD switch → `usb-reader` |

VIN switching prompts for confirmation — voltage changes have hardware side effects.

## Multiple instances

The TUI polls over HTTP at 60 Hz. You can run several instances at once without interference.

## High-rate capture

For high-rate ADC recording, use `adc record` from the CLI — it uses a separate websocket and doesn't go through the TUI. See [CLI Reference](cli.md#recording).
