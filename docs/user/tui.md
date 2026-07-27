# TUI Guide

[中文](tui.zh-CN.md)

Run without a subcommand to start the interactive terminal UI:

```sh
radxa-linkr-debuggerctl
```

## Layout

Four control sections:

- **Power** — `12v_out`, `5v_out`, `20v_out` toggles
- **Switch** — SD (`target` / `usb-reader`), USB (`pc` / `target`), VIN (`1.8v` / `3.3v`)
- **Target recovery** — select Qualcomm EDL or Rockchip MASKROM, select the target power rail, then run the firmware-owned recovery sequence
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

VIN switching prompts for confirmation — voltage changes have hardware side effects. Target recovery also requires a second Enter/Space within three seconds because it power-cycles the target device. Qualcomm EDL drives `CON_MAS` high during startup; Rockchip MASKROM drives it low. Firmware releases the signal to input after the sequence. `CON_MAS` is intentionally omitted from the generic GPIO controls.

## Multiple instances

The TUI renders responsively and polls board state over HTTP every two seconds. You can run several instances at once without interference.

## High-rate capture

For high-rate ADC recording, use `adc record` from the CLI — it uses a separate websocket and doesn't go through the TUI. See [CLI Reference](cli.md#recording).
