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

Adjacent live current channels in the scope are separated by exactly one
unstyled blank gutter cell in the scope header and in all six graph rows.
Three live channels therefore render side by side without a border, with
canonical content widths and zero-based gutter columns fixed at four widths:
W=47 splits to 15/15/15 with gutters at columns 15 and 31, W=48 to 16/15/15
with gutters at 16 and 32, W=80 to 26/26/26 with gutters at 26 and 53, and
W=120 to 40/39/39 with gutters at 40 and 80. A single live channel keeps the
full width with no gutter. When the terminal is too narrow to keep every
channel plus the gutters, the gutters collapse and the channels fill the row.
The full geometric contract, including the no-wrap and exact-width rules,
lives in [cmd-ng/DESIGN.md](../../cmd-ng/DESIGN.md) (section 7).

The page strip exposes three functional pages:

- **Controls** — live Power, Switch, and GPIO controls
- **Saved Config** — select, save, or clear the firmware-owned snapshot
- **Status** — switch desired/actual routes, monitoring, and errors

Every visible page label is its own mouse hit region. An active label's hit
region includes the one-cell reversed padding on either side, an inactive
label's hit region is just its label cells, and the two-space gap between
adjacent labels is never part of either hit region. Only a left-button Down
on an inactive label switches the page; the same gesture on the active label,
on either two-space gap, or on any other mouse event is a no-op. Tab and
Shift+Tab keep the same precedence on the keyboard side.

Controls uses fixed columns (`TYPE`, `NAME`, `STATE-ROUTE`, `LIVE`, `MODE`, and
`DESCRIPTION`) for Power and Switch. GPIO uses compact connector rows with up
to two independently selectable cells. MODE shows the effective TUI policy:
every firmware-advertised switch reads `confirm`. Type and ordering retain the
three logical groups:

- **Power** — every controllable output in the latest firmware `power_outputs`
  catalog, in firmware order
- **Switch** — firmware-advertised routes including SD (`target` / `usb-reader`),
  TF write-protect (`writable` / `protected`), USB (`pc` / `target`), and VIN
  (`1.8v` / `3.3v`)
- **GPIO** — Safe pins grouped only from firmware-provided
  `layoutGroup/layoutLabel/layoutRow/layoutColumn` metadata

Saved Config likewise renders one item per row with selection, ID, kind, current,
saved, risk, and apply columns. Status renders one switch or monitoring field per
row and keeps desired and actual routes in stable columns. Narrow terminals drop
whole trailing columns instead of wrapping one object onto another line.

On the Saved Config page, each visible item row owns one full-width hit
rectangle whose target carries the item's stable firmware ID, never its list
index. Header, badge, loading, unavailable, error, `(none)`, and blank rows
never register row hits. A left-button Down on a visible item re-resolves
that ID against the current authoritative items, focuses the page, moves the
cursor to the resolved row, and toggles the local checkbox selection exactly
once. Mouse Up, left Drag, Moved, scrolling, and middle or right-button
events are inert on item rows; so is a Down on a stale row whose ID is no
longer present. While a Saved Config confirmation modal is open, only its
own `[ Confirm ]` and `[ Cancel ]` buttons accept left-button Down, and every
underlying row, tab, and hardware-modal hit is inert under the modal.

Live color makes state changes visible without opening a detail pane: power is
green when on and dark gray when off; a switch route is cyan, pending is yellow,
and a desired/actual mismatch is red. GPIO state is written as `◌ IN LOW`,
`◌ IN HIGH`, `○ OUT LOW`, or `● OUT HIGH`; LOW is dark gray with no light
background and HIGH is bold red. A selected Power, Switch, or Saved Config row
uses a full-width light background. A selected GPIO paints only its own cell,
so the sibling remains independently readable and selectable. While a GPIO
request is in flight, its cell appends `[LOW…]`, `[HIGH…]`, or `[INPUT…]` in
bold yellow, and a held left-button press appends `[HOLD…]`. The direction and
level remain the latest firmware-reported state until the post-request status
refresh completes.

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
| Left-button Down on a visible Controls / Saved Config / Status tab label | Switch to that page; only the exact label cells (or label plus one-cell reversed padding for the active label) accept the click, and only on an inactive page |
| Left-button Down on an active tab label or on a two-space tab gap | Inert; does not switch pages |
| Mouse Up / Drag / Moved / scrolling / middle / right buttons on the tab strip | Inert |
| Up / Down | Move by visible Controls/Saved Config rows while preserving the GPIO side when possible; scroll Status |
| Left / Right | Move between sibling GPIO cells on the same projected row |
| PgUp / PgDn, Ctrl+U / Ctrl+D, `[` / `]` | Move three rendered rows/items |
| Space / Enter | Activate the selected control or toggle the current Saved Config item; inert on a selected GPIO |
| `l` / `L` on a GPIO | Drive the selected GPIO LOW |
| `o` / `O` on a GPIO | Drive the selected GPIO HIGH |
| `i` / `I` on a GPIO | Restore the selected GPIO to input mode |
| `0` / `1` on a GPIO | Inert; not alternate action bindings |
| Left click a Power/Switch row | Select and activate exactly that control |
| Left-button Down on a visible Saved Config item row | Focus Saved Config, move the cursor to that row, and toggle the local checkbox selection exactly once |
| Mouse Up / Drag / Moved / scrolling / middle / right buttons on a Saved Config row | Inert |
| Left-button Down on a row whose ID is no longer in the authoritative items | Inert; cannot retarget the item now occupying that list index |
| Left click a GPIO | Select that pin; releasing before 600 ms opens a 220 ms second-click window, whose expiry drives LOW; holding to 600 ms drives HIGH once; a second press on the same pin before the window expires restores input with no transient LOW |
| Middle / right click a GPIO | Inert |
| `g` | Jump to the first GPIO |
| `c` | Toggle between Controls and Saved Config |
| `p` / `r` | Pause polling / request an immediate refresh |
| `s` / `x` on Saved Config | Save the selected items / clear the saved snapshot |
| `q` / Ctrl+C | Quit and restore the terminal |
| Esc | Return from Saved Config, or dismiss the active error/confirmation |

Tab switching responds only to left-button Down on the visible label span.
Mouse Up, Drag, movement, scrolling, middle/right buttons, and tab input behind
a Saved Config confirmation/error or hardware confirmation are inert.

The fixed bottom row is a page-aware keybar. Each visible unit is a cyan key block
and an action label; a unit that does not fit is omitted as a whole rather than
clipped. Controls and Saved Config scroll automatically to keep the active row
visible; Status scrolls directly with the navigation keys, and each page retains
its own scroll offset. At 80x24 the Controls viewport exposes at least 14 hardware
objects when paired GPIO cells are counted individually; at 120x32 it exposes at
least 21 when the board advertises that many objects.

A direct power action or any firmware-advertised switch first opens a centered
red-bordered confirmation box. The TUI applies this existing three-second
gate to every firmware-advertised switch regardless of the firmware
`requires_confirm` flag, so a switch that the firmware classifies as safe
still routes through the modal in the TUI. A switch click selects its full
row before the modal opens, and that full-row selection keeps its composed
`accent.select` background on every visible cell not covered by the modal
until the confirmation resolves. The first activation sends no request:
press Enter/Space or click **Confirm** strictly before the three-second
deadline (i.e. `now < started + CONFIRM_TIMEOUT`), or use Esc / **Cancel**.
A confirmation key or click that arrives at or after the three-second mark
is treated as expired and the command emits its `timeout_message` instead
of running; the deadline check is evaluated on the confirmation event
itself and does not depend on the next render or poll tick having observed
expiry first. Confirmation overlays are the only bordered regions in the
TUI. One fresh confirmation routes the captured switch exactly once;
cancellation and timeout route zero times. Firmware `requires_confirm`
data remains authoritative for Saved Config, API, and CLI behavior, and
the TUI does not hardcode any production switch name, route, or pin in
this policy.

This change is local to the TUI's activation policy and rendering. The
Web UI, firmware, HTTP API, persistent-configuration wire contract, and
the non-TUI CLI commands (`power`, `switch`, `gpio`, `config`) all keep
their existing semantics.

MASKROM and EDL are ordinary firmware-catalog built-in automation tasks, not
TUI controls. List and run them through the generic `task` command or the Web
Automation workspace; the TUI exposes none of its own recovery recipes.

GPIO uses explicit direct actions rather than a toggle: `l`/`L` drives LOW,
`o`/`O` drives HIGH, and `i`/`I` restores input; Enter, Space, `0`, and `1`
are inert on a GPIO. The direct-key decoder accepts only lowercase
`l`/`o`/`i` with no modifiers and uppercase `L`/`O`/`I` with exactly Shift;
any other modifier (Ctrl, Alt, Super) and any mismatched case/shift form
returns no intent, leaves any active gesture state untouched, and is fully
inert. The left mouse button follows a deterministic gesture:
releasing before 600 ms opens a 220 ms await-second window, and LOW is driven
only when that window expires without another press. Holding to 600 ms drives
HIGH exactly once; a second press on the same pin strictly before the 220 ms
await-second deadline restores input with no transient LOW. Middle and right
buttons are always inert. A matching Up at or after 600 ms dispatches HIGH immediately.
A second Down at or after the 220 ms deadline dispatches the expired LOW for
the original pin and is consumed; the next fresh Down starts a new gesture.
While a press is held, the pin's cell shows a bold yellow `[HOLD…]` tag until
the gesture resolves. A Moved or left Drag report remains active within the
same terminal cell; only a report that crosses to a different pin, terminal
column, or terminal row cancels the gesture. A terminal `Resize` is the
canonical TUI redraw boundary: it cancels any in-flight GPIO gesture (both
the initial left-button Down and AwaitSecond states), invalidates the entire
hit-map so no old rectangle is reused for the new geometry, and forces a
redraw before the next queued input event is consumed. A mouse event queued
after the Resize is therefore evaluated against the new geometry instead of
acting on stale hit rectangles. Esc, page switches, pause, any
modal or error state, a GPIO job becoming pending, or the pin disappearing
from the firmware snapshot also cancel silently. GPIO actions do not require
confirmation and do not update the display optimistically; the TUI shows the
pending action, then refreshes the authoritative firmware state. Each half-row
GPIO hit target belongs to one pin; an empty half is inert.

## Multiple instances

The TUI renders responsively and polls board state over HTTP every two seconds. You can run several instances at once without interference.

## High-rate capture

For high-rate ADC recording, use `adc record` from the CLI — it uses a separate websocket and doesn't go through the TUI. See [CLI Reference](cli.md#recording).
