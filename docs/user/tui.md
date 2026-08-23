# TUI Guide

[中文](tui.zh-CN.md)

Run without a subcommand to start the interactive terminal UI:

```sh
radxa-linkr-debuggerctl
```

## Layout

The normal screen is a borderless vertical stack: two status rows, an optional
seven-row three-channel current scope, the fixed page strip, a page-specific
table header, dense data rows, and a one-row htop-style keybar. The scope uses
one header plus six graph rows and scales each channel from its visible peak;
empty telemetry reserves no space.

The page strip exposes three functional pages:

- **Controls** — live Power, Switch, and GPIO controls
- **Saved Config** — select, save, or clear the firmware-owned snapshot
- **Status** — switch desired/actual routes, monitoring, and errors

Controls uses fixed columns (`TYPE`, `NAME`, `STATE-ROUTE`, `LIVE`, `MODE`, and
`DESCRIPTION`) for Power and Switch. GPIO uses compact connector rows with up
to two independently selectable cells. Type and ordering retain the three
logical groups:

- **Power** — every controllable output in the latest firmware `power_outputs`
  catalog, in firmware order
- **Switch** — SD (`target` / `usb-reader`), USB (`pc` / `target`), VIN (`1.8v` / `3.3v`)
- **GPIO** — Safe pins grouped only from firmware-provided
  `layoutGroup/layoutLabel/layoutRow/layoutColumn` metadata

Saved Config likewise renders one item per row with selection, ID, kind, current,
saved, risk, and apply columns. Status renders one switch or monitoring field per
row and keeps desired and actual routes in stable columns. Narrow terminals drop
whole trailing columns instead of wrapping one object onto another line.

Live color makes state changes visible without opening a detail pane: power is
green when on and dark gray when off; a switch route is cyan, pending is yellow,
and a desired/actual mismatch is red. GPIO state is written as `◌ IN LOW`,
`◌ IN HIGH`, `○ OUT LOW`, or `● OUT HIGH`; LOW is dark gray with no light
background and HIGH is bold red. A selected Power, Switch, or Saved Config row
uses a full-width light background. A selected GPIO paints only its own cell,
so the sibling remains independently readable and selectable.

At 48 columns or wider, pins from the same firmware physical row occupy at most
two cells. Narrower terminals show one pin per row. Incomplete layout metadata
falls back to firmware snapshot order; the host never invents connector names,
pin maps, mirroring, or missing row relationships.

Power discovery follows the same ownership rule. The TUI does not keep a host
rail list: newly advertised outputs appear automatically, outputs absent from a
later status snapshot disappear, and selection follows the same hardware name
when firmware reorders the catalog. `5V_FIN` is not shown because firmware
classifies it as an input/source rather than a controllable output.

## Navigation

| Key | Action |
| --- | --- |
| Tab / Shift+Tab | Move to the next / previous page |
| Up / Down | Move by visible Controls/Saved Config rows while preserving the GPIO side when possible; scroll Status |
| Left / Right | Move between sibling GPIO cells on the same projected row |
| PgUp / PgDn, Ctrl+U / Ctrl+D, `[` / `]` | Move three rendered rows/items |
| Space / Enter | Activate a control or toggle the current Saved Config item |
| Left click a Power/Switch row or GPIO cell | Select and activate exactly that control |
| `i` / right-click a GPIO | Restore that GPIO to input mode |
| `g` | Jump to the first GPIO |
| `c` | Toggle between Controls and Saved Config |
| `p` / `r` | Pause polling / request an immediate refresh |
| `s` / `x` on Saved Config | Save the selected items / clear the saved snapshot |
| `q` / Ctrl+C | Quit and restore the terminal |
| Esc | Return from Saved Config, or dismiss the active error/confirmation |

The fixed bottom row is a page-aware keybar. Each visible unit is a cyan key block
and an action label; a unit that does not fit is omitted as a whole rather than
clipped. Controls and Saved Config scroll automatically to keep the active row
visible; Status scrolls directly with the navigation keys, and each page retains
its own scroll offset. At 80x24 the Controls viewport exposes at least 14 hardware
objects when paired GPIO cells are counted individually; at 120x32 it exposes at
least 21 when the board advertises that many objects.

A direct power action or a firmware-marked switch such as VIO first opens
a centered red-bordered confirmation box. The first activation does not change
hardware: press Enter/Space or click **Confirm** within three seconds, or use Esc /
**Cancel**. Confirmation overlays are the only bordered regions in the TUI.

MASKROM and EDL are ordinary firmware-catalog built-in automation tasks, not
TUI controls. List and run them through the generic `task` command or the Web
Automation workspace; the TUI exposes none of its own recovery recipes.

GPIO follows the Web UI contract. Primary activation changes an input to output
HIGH; an output then toggles HIGH/LOW. Use `i` or right-click to return it to input.
Each half-row GPIO hit target belongs to one pin; an empty half is inert.

This presentation redesign does not change HTTP endpoints, the two-second poll
cadence, confirmation/action semantics, hardware defaults, or firmware behavior.

## Multiple instances

The TUI renders responsively and polls board state over HTTP every two seconds. You can run several instances at once without interference.

## High-rate capture

For high-rate ADC recording, use `adc record` from the CLI — it uses a separate websocket and doesn't go through the TUI. See [CLI Reference](cli.md#recording).
