# cmd-ng TUI Design Contract (htop-inspired redesign)

This document is the authoritative visual and interaction contract for the
htop-inspired Ratatui redesign of the `cmd-ng` TUI. It is decision-complete:
implementation and `TestBackend` tests must be writable from this document
alone, without guessing. Where a behavior is marked FROZEN, the redesign
reimplements the current behavior of the cited symbol exactly; only layout
and presentation change.

Current flow being replaced: the legacy bordered header/sparkline/chip-grid
layout was already removed. This revision replaces the two remaining wrong
decisions: (1) the one-row fixed-5000 sparkline telemetry band becomes a real
7-row borderless three-channel adaptive current oscilloscope (section 7);
(2) the one-GPIO-object-per-row data model becomes a firmware-metadata-driven
pinout projection with up to two individually selectable GPIO cells per
visual row (sections 5.4, 6.1, 10), matching the WebUI physical
pinout/control model. Sections 3.3 and 10 freeze the direct TUI GPIO action
bindings; board/API semantics, model ownership, confirmation state machines,
and event routing precedence remain unchanged. The TUI activation policy now
routes every firmware-advertised switch through that existing confirmation
state machine.

## 1. Goals

1. Borderless, dense, htop-style control surface: one power/switch object per
   row, GPIO pins projected from firmware layout metadata at up to two cells
   per visual row, no decorative boxes in normal regions.
2. A real current oscilloscope: 7 rows (header + six graph rows) whenever any
   monitored channel has data, live channels in near-equal content columns
   separated by one blank cell, per-channel adaptive scale; zero height with
   no data.
3. Fixed, testable vertical budgets at the two canonical sizes 80x24 and
   120x32, with a deterministic general rule for all other sizes.
4. Semantic color tokens and non-color cues (markers, text) for every
   hardware state; no state is conveyed by color alone.
5. Deterministic responsive behavior: column clipping, cell pairing, and
   segment dropping are pure functions of terminal width; no row ever wraps.
6. Keep requests, polling, confirmation state machines, and hardware-state
   ownership unchanged; require their existing 3 s hardware confirmation for
   every switch activation. GPIO input bindings and gesture semantics are
   explicitly revised by sections 3.3 and 10.

## 2. Non-goals

1. No new hardware capabilities, endpoints, F-key bindings, board maps,
   routes, defaults, fallback paths, or compatibility shims.
2. No changes to firmware, Device Tree, API schemas, the web UI, or the
   non-TUI CLI commands.
3. No changes to firmware-owned state, the Saved Config state machines in
   `config_state.rs`/`config_result.rs`, or modal input precedence. The GPIO
   action mapping and gesture routing in `actions.rs`/`events.rs` are in scope.
4. No hardcoded connector or pin maps in the TUI: connector names, row/column
   counts, labels, and any mirroring (such as the WebUI's J16 column mirror)
   are never reimplemented host-side; the TUI consumes the firmware's
   `layoutGroup`/`layoutLabel`/`layoutRow`/`layoutColumn` metadata generically.
5. No separate GPIO page and no bordered normal regions.
6. No mouse-wheel or hover behavior (none exists today; none is added).
7. No persistence of UI state across runs.

## 3. Frozen behavior (do not change)

The following are contracts with the board and with existing tests. The
redesign must preserve them exactly.

### 3.1 HTTP API surface (frozen)

Grounded in `cmd-ng/src/tui/board_io.rs` and `cmd-ng/src/client.rs`:

| Operation        | Method | Path                        | Body                                          |
|------------------|--------|-----------------------------|-----------------------------------------------|
| Power toggle     | PUT    | `/api/v1/power/{output}`    | `{"state":"on"\|"off"}`                       |
| Switch route     | PUT    | `/api/v1/switch/{name}`     | `{"route":"<route>"}`                         |
| GPIO set output  | PUT    | `/api/v1/gpio/{gpio}`       | `{"direction":"output","value":0\|1}`         |
| GPIO set input   | PUT    | `/api/v1/gpio/{gpio}`       | `{"direction":"input"}`                       |
| Status poll      | GET    | `/api/v1/status`            | -                                             |
| ADC poll         | GET    | `/api/v1/adc/read`          | -                                             |
| Config show      | GET    | `/api/v1/config`            | -                                             |
| Config save      | PUT    | `/api/v1/config`            | `{"items":[...],"confirm":bool}`              |
| Config clear     | DELETE | `/api/v1/config`            | -                                             |

### 3.2 Polling and tick (frozen)

Grounded in `cmd-ng/src/tui.rs`:

- Render/tick interval: `TUI_POLL_INTERVAL` (~16.7 ms).
- HTTP poll interval: `HTTP_POLL_INTERVAL` = 2 s; each poll issues GET
  `/api/v1/status` then GET `/api/v1/adc/read`.
- `p` toggles `model.paused`; while paused, HTTP polling stops but rendering,
  input, and config-worker completion continue.
- `r` forces an immediate poll and requests a Saved Config refresh when
  supported.
- ADC history is capped at `TUI_HISTORY_LIMIT` = 240 samples per channel.

### 3.3 Action semantics (frozen)

Grounded in `cmd-ng/src/tui/actions.rs` (`resolve_activation`):

- Power primary action ALWAYS requires confirmation
  (`Activation::Confirm(ConfirmableCommand::SetPower { next_state: !current })`).
- Switch primary action cycles to `next_switch_route` in firmware route order
  and ALWAYS returns
  `Activation::Confirm(ConfirmableCommand::RouteSwitch { ... })` when the
  current switch state advertises a next route. The first activation never
  sends a request. A missing current switch state is ignored; a switch with no
  advertised routes is rejected with a status message and no request.
- GPIO `Primary` is ignored. Direct keyboard actions on a selected GPIO are
  `l`/`L` for output LOW, `o`/`O` for output HIGH, and `i`/`I` for input.
  Enter, Space, `0`, and `1` are inert on a GPIO; power and switch primary
  behavior is unchanged. The decoder in `direct_gpio_key.rs` accepts only
  lowercase `l`/`o`/`i` with `KeyModifiers::NONE` and uppercase `L`/`O`/`I`
  with exactly `KeyModifiers::SHIFT`; any other modifier (Ctrl, Alt, Super,
  or a mismatched shift form) returns `None`, leaves the gesture state
  untouched, and is fully inert. Mouse actions use the deterministic gesture
  contract in section 10. A GPIO cell keeps rendering the last authoritative
  direction/level until the next status snapshot; neither gesture transitions
  nor dispatched actions optimistically modify GPIO state maps.
- After a switch route PUT, the model sets `pending_route` and
  `pending_until = now + 2 s`; pending/mismatch presentation derives from
  `TuiSwitchState` and must not be recomputed differently.
- Every GPIO pin remains individually addressable: selection, primary, and
  secondary actions always apply to exactly one pin.

### 3.4 Confirmations (frozen)

Grounded in `cmd-ng/src/tui/confirm.rs`, `render_modal.rs`,
`config_render.rs`, `config_state.rs`:

- Hardware confirmation: 3 s timeout (`CONFIRM_TIMEOUT`); Enter/Space or the
  Confirm button executes only when the press arrives strictly before the
  3 s deadline (i.e. `now < started + CONFIRM_TIMEOUT`). A confirmation press
  arriving at or after the 3 s deadline is rejected as expired and emits the
  command's `timeout_message` instead of executing; the deadline check is
  evaluated on the confirmation event itself and does not depend on the
  render/poll tick having observed expiry first. Esc or the Cancel button
  cancels with the command's `cancel_message`; expiry emits the command's
  `timeout_message`.
- Every switch uses this hardware confirmation regardless of the firmware
  `requires_confirm` flag. One fresh confirm routes the captured switch target
  exactly once; cancel or timeout routes zero times. The firmware flag remains
  authoritative data for Saved Config risk and the API/CLI surfaces and is not
  rewritten by the TUI policy.
- Saved Config confirmation: only for saves whose selection includes
  `requires_confirm` items; the modal lists the server-reported dangerous IDs.
  Enter or left-button Down on the complete `[ Confirm ]` button confirms
  (resubmits with `confirm: true` exactly once); Esc or left-button Down on the
  complete `[ Cancel ]` button cancels without starting a request.
- Input precedence in `handle_key` (frozen order): Saved Config confirmation
  modal, then Saved Config error dismiss (Esc), then hardware confirmation
  modal, then normal keys. While any confirmation or error modal state is
  active, all other keys and all mouse input outside the modal are inert.
- Confirmation overlays are the ONLY bordered regions in the UI.

### 3.5 Board facts and model ownership (frozen)

- Power outputs and row order come from the latest firmware `power_outputs`
  catalog through `control_targets()`; the TUI keeps no host-side rail list.
- Scope channels come from `model.channel_ids`: `5v_out`, `12v_out`,
  `20v_out`.
- GPIOs are named and enumerated by the firmware status snapshot; every
  reported GPIO remains represented, including entries whose note is `CON_MAS`.
- GPIO layout metadata is firmware-owned: each status GPIO entry may carry
  `layoutGroup`, `layoutLabel`, `layoutRow`, and `layoutColumn` alongside
  `pin`/`name`/`note`/`direction`/`value`. The TUI consumes these fields
  generically and never substitutes its own connector maps.
- Firmware-ordered Power rows then switch (BTreeMap key order) row ordering is
  frozen. GPIO
  visual rows are produced by the projection in section 6.1; that
  projection is the only ordering authority for GPIO pins.
- Boot-time defaults, rail maps, and pin facts remain owned by firmware /
  Device Tree. The TUI reflects polled state; it never imposes its own
  defaults.

## 4. Frame layout and vertical budgets

The frame is a single vertical stack of six bands. All bands are borderless.
No blank separator rows exist anywhere in normal regions.

```
+----------------------------------------------------------+  row 1
| STATUS band (2 rows, borderless)                         |  row 2
+----------------------------------------------------------+
| SCOPE band (0 or 7 rows, borderless: 1 header + 6 graph  |  ...
| rows; zero height when no channel has data)              |
+----------------------------------------------------------+
| TAB bar (1 row): Controls | Saved Config | Status        |
+----------------------------------------------------------+
| TABLE HEADER (1 row, power/switch column titles)         |
+----------------------------------------------------------+
| DATA region (all remaining rows; mixed power/switch rows |  ...
| and projected GPIO cell rows)                            |
+----------------------------------------------------------+
| KEYBAR (1 row)                                           |  last row
+----------------------------------------------------------+
```

### 4.1 Canonical budget: 80x24 (scope populated)

```
band            rows    row range (1-based)
STATUS          2       1-2
SCOPE           7       3-9    (1 header + 6 graph)
TABS            1       10
TABLE HEADER    1       11
DATA            12      12-23
KEYBAR          1       24
```

### 4.2 Canonical budget: 120x32 (scope populated)

```
band            rows    row range (1-based)
STATUS          2       1-2
SCOPE           7       3-9    (1 header + 6 graph)
TABS            1       10
TABLE HEADER    1       11
DATA            20      12-31
KEYBAR          1       32
```

When no scope channel has data, the SCOPE band occupies zero rows and every
band below it moves up: at 80x24 the tabs land on row 3, the table header on
row 4, and DATA starts on row 5.

### 4.3 General rule (all other sizes)

Let `H` = terminal height, `W` = terminal width.

- scope_rows = 7 when at least one scope channel has data (section 7) and
  H >= 24; otherwise 0. The scope never takes a partial height: it is either
  the full 7-row form or absent.
- Shrink order when H < 24: the scope shrinks to 0 first; then DATA shrinks
  below its canonical size. STATUS (2), TABS (1), TABLE HEADER (1), and
  KEYBAR (1) never shrink. Minimum renderable height is 6 rows; below that
  the UI draws nothing but must not panic.
- data_rows = H - 2 - scope_rows - 1 (tabs) - 1 (table header) - 1 (keybar)
  = H - 5 - scope_rows.
- Width has no effect on the vertical budget. Width only affects scope column
  split (section 7), table column clipping (section 6), GPIO cell pairing
  (section 6.1), and keybar segmentation (section 8).

## 5. Band schemas

### 5.1 STATUS band (2 rows, borderless)

- Row 1: left-aligned app title `Radxa Linkr Debugger TUI` (bold), followed
  by `url=<base_url>`; right-aligned connection/poll state derived from
  `model.status` when it reports a mode (e.g. `HTTP mode`) plus `paused`
  when `model.paused` is true.
- Row 2: left-aligned last action status (`model.status`). When
  `model.err` is set, row 2 shows the error text in the `state.error` token.
- When content exceeds `W`, clip deterministically: the right-aligned field
  is dropped first, then the url, then the title is hard-clipped. Row 2 is
  hard-clipped at `W`.

### 5.2 TAB bar (1 row)

- Three tabs in fixed order: `Controls`, `Saved Config`, `Status`
  (the `ActivePage` enum order in `pages.rs`).
- Active tab: `accent.tab.active` (bold + reversed). Inactive: `fg.muted`.
- Tab cycle order is frozen: Tab = `next_page` (Controls -> Saved Config ->
  Status -> Controls), Shift+Tab = `prev_page`.
- Each rendered tab label span is a mouse hit region. The region is exactly the
  visible span: an active label includes its one-cell leading and trailing
  reversed padding, while an inactive label includes only its label cells.
  The two-space separators are never part of either neighboring hit region.
  At W in {47, 48, 80, 120}, relative half-open ranges are: active Controls
  `[0,10)`, `[12,24)`, `[26,32)`; active Saved Config `[0,8)`, `[10,24)`,
  `[26,32)`; active Status `[0,8)`, `[10,22)`, `[24,32)`.
- Only left-button Down on an inactive tab changes page. It calls
  `TuiModel::set_page`, preserving canonical Saved Config focus/blur, GPIO
  gesture cancellation, and independent page scroll offsets. Left Down on the
  active tab, either separator, or outside a label is a no-op. Mouse Up, Drag,
  Moved, Scroll, and middle/right-button events on a tab are inert. The active
  tab keeps the same bold + reversed presentation; mouse support adds no
  hover, pressed, or alternate style.
- The tab bar may append a right-aligned GPIO output counter of the exact
  form `GPIO OUT n/total`, where `n` is the number of projected GPIO pins
  whose direction is output and `total` is the number of projected GPIO pins.
  The counter is all-or-nothing: it is rendered only if the whole counter
  text fits after the tab labels plus a two-space gap; otherwise it is
  dropped entirely. It is never partially drawn.

### 5.3 TABLE HEADER (1 row)

- Power/switch column titles (section 6.1), bold, `fg.muted`; exactly one row
  on every page, including Status. The header row is pinned: it never
  scrolls with DATA. GPIO cell rows (section 6.1) do not align to these
  columns and have no header of their own.

### 5.4 DATA region (mixed row model)

- Power and switch rows: one hardware object per row, in the frozen order
  (4 power, then switches in BTreeMap key order), rendered with the column
  table in section 6.1. No row ever wraps; no blank separators; no section
  headers.
- GPIO rows: below the switch rows, GPIO pins appear as projected visual
  rows of one or two individually selectable cells (section 6.1). No group
  header rows and no blank separators exist between groups; grouping is
  expressed through cell order and cell text only.
- Selection background: a selected power/switch row paints the
  `accent.select` background across the full row width. A selected GPIO cell
  paints `accent.select` only over its own hit region: the full row in
  single-cell mode, exactly its half of the row in paired mode; the sibling
  cell keeps its normal styling.
- Opening a confirmation modal never changes the Controls selection. The
  clicked power/switch row remains selected beneath the modal; every visible
  cell of that full-width row not covered by the modal keeps the composed
  `accent.select` style.
- Scrolling: each page owns an independent scroll offset
  (`controls_scroll`, `config_scroll`, `status_scroll` in `model.rs`,
  switched by `page_scroll`/`set_page_scroll`). Scroll offsets count
  projected DATA lines (one visual row = one line). `clamp_scroll` clamps
  to `content_height - viewport`; `ensure_visible` scrolls the minimum
  amount to keep the selected projected row visible. Switching pages
  restores that page's own offset.

### 5.5 KEYBAR (1 row)

See section 8.

## 6. Page schemas and column behavior

Column widths below are the canonical values at W >= 120. Clipping is always
deterministic: hard clip at the column boundary, no ellipsis, no wrap.

### 6.1 Controls page

Power/switch columns, left to right:

| Column      | Min width | Content by row type                                                                 |
|-------------|-----------|-------------------------------------------------------------------------------------|
| TYPE        | 6         | `power` / `switch`                                                                  |
| NAME        | 12        | output / switch name                                                                |
| STATE-ROUTE | 14        | power: `on`\|`off`; switch: `<desired>` or `<desired>(-><actual>)` on mismatch, suffixed ` (pending)` while `pending_route` is set |
| LIVE        | 9         | power: measured current from `model.latest` (e.g. `0.042000A`) or `-` when no reading; switch: `-` |
| MODE        | 7         | power: `-`; switch: effective TUI policy `confirm` for every advertised switch     |
| DESCRIPTION | rest      | power: ADC channel id when monitored else `-`; switch: advertised routes joined with `/` |

Column drop order as width shrinks (deterministic, applied right to left):
DESCRIPTION shrinks to its 8-column minimum, then columns are dropped in the
order DESCRIPTION, LIVE, MODE. STATE-ROUTE is then clipped to fit. TYPE and
NAME are always present; NAME absorbs the remaining width after TYPE and is
hard-clipped last. Minimum power/switch row: `TYPE` + `NAME`. LIVE values
are right-aligned.

#### GPIO pinout projection

GPIO pins are laid out from firmware metadata only. For each GPIO the TUI
consumes `pin`, `name`, `note`, `direction`, `value`, and the optional
layout fields `layoutGroup`, `layoutLabel`, `layoutRow`, `layoutColumn`.

- A pin's metadata is COMPLETE when all four layout fields are present.
  Groups are ordered by their first appearance in the firmware snapshot.
- Coordinate collisions: when two complete-metadata pins in one group share
  the same (`layoutRow`, `layoutColumn`), the first pin in snapshot order
  keeps the coordinate; every later duplicate is demoted to the fallback
  group.
- Complete-metadata projection: within each group, pins are grouped by
  distinct `layoutRow`; those firmware rows are ordered numerically. Within
  one firmware row, pins sort by (`layoutColumn`, then snapshot order) and
  fill the visual row's cells left to right in that order. If a single
  firmware row holds more than two entries, only that row is chunked into
  consecutive visual rows of at most two cells, preserving the
  (`layoutColumn`, snapshot) order. Cells are never paired across different
  firmware `layoutRow` values: a firmware row with one entry produces a
  visual row with a single cell and an empty other half.
- The fallback group is named exactly `GPIO`. It collects, in firmware
  snapshot order, every pin with incomplete metadata and every demoted
  duplicate. It is appended after the metadata groups, and its pins form
  consecutive visual rows of at most two cells in snapshot order.
- Pairing mode: at W >= 48 each visual row holds up to 2 cells; the left
  cell occupies columns 0..floor(W/2)-1 and the right cell columns
  floor(W/2)..W-1; a visual row with one entry leaves the right half empty.
  At W < 48 every cell occupies its own full-width visual row: the ordered
  cells (metadata groups firmware row by firmware row, then the fallback in
  snapshot order) become consecutive single-cell visual rows.
- Cell text, left to right: `<group> <label> <marker> <DIR> <LEVEL>`
  followed by the optional `<note>`/`<pin>` detail. `group` is the firmware
  `layoutGroup` (or `GPIO` for the fallback); `label` is `layoutLabel`
  (fallback: the firmware `name`). The marker and words are exactly:

  | Direction | Level | Rendered form  |
  |-----------|-------|----------------|
  | input     | LOW   | `◌ IN LOW`     |
  | input     | HIGH  | `◌ IN HIGH`    |
  | output    | LOW   | `○ OUT LOW`    |
  | output    | HIGH  | `● OUT HIGH`   |

  HIGH marker and words use `state.gpio.high` (Red + bold); LOW uses
  `state.gpio.low` (DarkGray on terminal default). The marker and the
  LOW/HIGH and IN/OUT words are always present: direction and level are
  never conveyed by color or glyph alone.
- Pending action tag: while `model.gpio_pending` targets the pin, the cell
  appends the exact tag `[LOW…]`, `[HIGH…]`, or `[INPUT…]` after the state
  suffix, separated by one space. The tag uses `state.gpio.pending`
  (Yellow + bold) on an unselected cell; on a selected cell the
  `accent.select` background still covers the whole hit region and the tag
  stays visible and bold. Clipping priority is: state suffix, tag,
  `<group> <label>`, note. When the cell is too narrow for the tag, the tag
  drops whole and the status line keeps carrying the in-flight
  `gpio <name>=…` text.
- Hold tag: while the GPIO gesture is in its initial left-button Down state
  (section 10), the tracked pin's cell appends the exact tag `[HOLD…]` after
  the state suffix with the same `state.gpio.pending` styling, selection
  composition, and drop-whole clipping rule as the pending action tag. The
  cell carries at most one tag: the pending action tag wins when both would
  apply. The await-second state renders no tag, and gesture state never
  changes the optimistic direction/level rendering.
- Narrow cells: the optional note/pin detail drops first. The fixed state
  suffix -- the marker plus the `IN`/`OUT` and `LOW`/`HIGH` words, at most
  10 columns (`● OUT HIGH`) -- has priority: whenever the cell width is at
  least the suffix width, the suffix is always rendered whole at the right
  edge of the cell, and the remaining width belongs to `<group> <label>`,
  which is hard-clipped to fit. Only a cell inherently narrower than the
  suffix width may hard-clip the suffix itself (tail cut), which can happen
  only on an inherently narrow terminal.
- Hit regions: one rectangle per selectable element. Power/switch rows span
  the full row width. A paired GPIO cell spans exactly its half of the row;
  a single-cell GPIO row spans the full row. Left-button input on a GPIO cell
  selects it and follows the gesture timing in section 10. Middle and right
  button input is inert. Power and switch left-button primary behavior remains
  unchanged when no GPIO gesture is active.

### 6.2 Saved Config page

One item per row. Columns, left to right:

| Column  | Min width | Content                                                        |
|---------|-----------|----------------------------------------------------------------|
| SEL     | 4         | `[x]` selected / `[ ]` unselected                              |
| ID      | 20        | config item id (`ConfigItemId`)                                |
| KIND    | 8         | item kind (power/switch/gpio)                                  |
| CURRENT | 12        | current live value                                             |
| SAVED   | 12        | saved value                                                    |
| RISK    | 6         | `danger` when `requires_confirm`, else `safe`                  |
| APPLY   | 9         | apply state; width derives from the longest token `not_saved`  |

Column drop order as width shrinks: APPLY, RISK, SAVED, CURRENT, KIND; then
ID is hard-clipped. SEL and ID are always present. No supported apply-state
token is ever truncated while the APPLY column is present.

Page state line: the heading badges `[unsupported]`/`[loading]`/
`[unavailable]`/`[ready]`/`[pending:N]`/`[busy:<kind>]`/`[error]` are
rendered as the first DATA row when the page cannot list items
(unsupported / not loaded / backend unavailable / empty) or when a
pending/busy/error badge exists. The error line `error=<text> [Esc]` is a
DATA row in `state.error` styling. These state rows are the only non-object
rows allowed in DATA, and only on this page.

Cursor: `saved_config.cursor` with `focused` gating, `move_cursor`,
`toggle_current` semantics frozen. The cursor row gets the full-width
`accent.select` background when the page is focused.

Mouse rows: every visible item row owns one full-width hit rectangle after the
page scroll offset is applied. Its typed target carries the item's stable
`ConfigItemId`, never its list index; header, badge, loading, unavailable,
error, `(none)`, and blank rows never register row hits. Left-button Down on a
row re-resolves that ID in the current authoritative `saved_config.items`, then
focuses Saved Config, sets `saved_config.cursor` to the resolved index, and
calls `toggle_current` exactly once. A stale target whose ID is no longer
present is inert and cannot retarget the item now occupying the old index.
Mouse Up, left Drag, Moved, Scroll, and middle/right-button events are inert.

### 6.3 Status page

Header: `SWITCH  DESIRED  ACTUAL  STATE`. One row per switch: desired route,
actual route, and derived state (`ready` / `pending` / `mismatch`, same
derivation as the Controls page). Below the switch rows, one monitoring
field per row as `key: value` pairs decomposed from
`format_monitoring_summary` (temperature, memory/heap, runtime, cpu) --
one pair per row, hard-clipped, never wrapped. When `model.err` is set, a
final `error: <text>` row in `state.error` styling. The Status page has no
selection; Up/Down scroll only (frozen from `handle_key`).

## 7. Scope band (current oscilloscope)

The scope replaces the legacy one-row sparkline strip. It is a borderless
7-row band: 1 header row + 6 graph rows.

- Live channels: channels from `model.channel_ids` (`5v_out`, `12v_out`,
  `20v_out`) that have at least one ADC reading in `model.latest` or a
  non-empty series in `model.history`. At most 3 channels today.
- Zero-height rule: when no channel has data, the band occupies 0 rows. It
  never renders placeholder boxes or `(no data)` lines.
- Columns: the live channels are laid out left to right in `channel_ids`
  order with one blank gutter cell between adjacent channels. For N live
  channels, reserve N - 1 gutter cells when W >= 2N - 1 (enough room for
  every channel to retain at least one content cell); otherwise reserve no
  gutters. Split the remaining content width C deterministically: base =
  floor(C / N), remainder R = C - N * base, and the first R live channels
  receive base + 1 content cells while the rest receive base. A single live
  channel therefore keeps the full width. Each reserved gutter is exactly
  one unstyled blank cell in the header and in all six graph rows; it is not
  a border and adds no row. Canonical three-channel content splits and
  zero-based gutter columns are: W=47 -> 15/15/15 with gutters 15 and 31;
  W=48 -> 16/15/15 with gutters 16 and 32; W=80 -> 26/26/26 with gutters 26
  and 53; W=120 -> 40/39/39 with gutters 40 and 80. Content plus gutters
  always occupies exactly W display columns and never wraps. Dead channels
  leave no gap.
- Header row: each channel column shows `<channel> <on|off|?> <current-text>
  max=<scale>mA`, hard-clipped to its column. The power state and current
  text derive from the same `AdcReading` fields the legacy sparkline titles
  used (`power_enabled`, `current_ua` / `current_milliamp_estimate`); the
  power-state word uses the `state.power.*` tokens.
- Graph rows: each channel column renders its history as a 6-row-tall
  oscilloscope trace. The visible window is the last samples that fit the
  column width (same windowing rule as the legacy sparkline). Every sample
  occupies one horizontal position; its amplitude maps linearly from
  0..scale onto 0..6 rows using the block glyph family (space, `▁`, `▂`,
  `▃`, `▄`, `▅`, `▆`, `▇`, `█`) with the partial glyph at the sample's top
  cell.
- Per-channel adaptive scale (no fixed 5000 anywhere): negative samples are
  clamped to 0; the visible peak is the maximum of all clamped visible
  samples, the clamped latest reading, and 1; the scale is the saturating
  integer `ceil(peak * 5 / 4)` = `(peak * 5 + 3) / 4` with saturating
  arithmetic. The scale is recomputed per render from the visible window.

## 8. Keybar segmentation

The keybar is exactly one row, `keybar` token (white on dark gray). It is
state- and page-aware; the ordered segment sets below are the contract
implemented by `render_keybar.rs`:

1. Hardware confirmation active: `Enter/Space confirm <target>`, `Esc cancel`.
2. Saved Config confirmation active: `Enter confirm Saved Config save`,
   `Esc cancel`.
3. Saved Config error visible: `Esc dismiss Saved Config error`.
4. Otherwise, the page segment set:
   - Controls with a GPIO cell selected: `l LOW`, `o HIGH`, `i INPUT`,
     `Mouse click/hold/2x`, `Tab/Shift+Tab page`, `g GPIO`, `p pause`,
     `r refresh`, `PgUp/PgDn Move`, `q quit`. The four GPIO-leading segments
     occupy exactly 43 columns and all fit at 47; the Mouse segment is the
     first to drop whole as width shrinks below that. Left-button gesture
     timing is described in section 10; Enter/Space/`0`/`1` and middle/right
     mouse actions are never advertised.
   - Controls: `q quit`, `Tab/Shift+Tab page`, `Enter/click activate`,
     `g GPIO`, `p pause`, `r refresh`, `PgUp/PgDn Move`.
   - Saved Config: `Saved Config`, `Up/Down item`, `Space select`,
     `s save`, `x clear`, `Tab/Shift+Tab page`, `Esc back`.
   - Status: `Status`, `Up/Down scroll`, `Tab/Shift+Tab page`, `q quit`.

Each segment is a styled key block (inverse accent: Black on Cyan, bold)
plus its action label (White on DarkGray); a page marker segment such as
`Saved Config` or `Status` has no key block and keeps the label style.
Segmentation rule (deterministic): build the ordered segment list for the
current state; render segments left to right separated by two spaces; a
segment is rendered only if it fits COMPLETE within the remaining width;
the first segment that does not fit whole, and every segment after it, is
dropped. Segments never wrap and are never partially drawn.

## 9. Semantic color tokens

Tokens map to Ratatui `Color` values. The mapping preserves the tones of the
legacy chip/modal/footer styles, re-expressed as named tokens, with one
deliberate deviation: the legacy `state.gpio.low` tone (black on white)
collided with the white selection background, so LOW is redefined as a
low-emphasis style with no light/background fill:

| Token                 | Value                                   | Used for                                             |
|-----------------------|-----------------------------------------|------------------------------------------------------|
| `fg.default`          | terminal default                        | normal row text, scope trace                         |
| `fg.muted`            | `DarkGray`                              | table header, inactive tabs, secondary text, scale text |
| `state.power.on`      | `Green`                                 | power on state, scope `on`                           |
| `state.power.off`     | `DarkGray`                              | power off state, scope `off`                         |
| `state.switch.ready`  | `Cyan`                                  | switch desired == actual, no pending route           |
| `state.switch.pending`| `Yellow`                                | `pending_route` set                                  |
| `state.switch.mismatch`| `Red`                                  | desired != actual                                    |
| `state.gpio.high`     | `Red` + bold                            | GPIO HIGH marker + words (`●`/`◌ ... HIGH`)          |
| `state.gpio.low`      | `DarkGray` on terminal default          | GPIO LOW marker + words (`○`/`◌ ... LOW`)            |
| `state.gpio.pending`  | `Yellow` + bold                         | GPIO in-flight action tag (`[LOW…]`/`[HIGH…]`/`[INPUT…]`) and hold tag (`[HOLD…]`) |
| `state.error`         | `Red`                                   | error rows, error status line                        |
| `accent.select`       | fg preserved (default `Black`) on `White`, bold | selected row or GPIO cell hit region     |
| `accent.tab.active`   | bold + reversed                         | active tab                                           |
| `keybar`              | `White` on `DarkGray`                   | the keybar row and action labels                     |
| `keybar.key`          | `Black` on `Cyan` + bold                | key blocks inside keybar segments                    |
| `modal.border`        | `Red`                                   | confirmation overlay borders (only bordered region)  |
| `modal.emphasis`      | `Yellow` + bold                         | confirmation title and warning line                  |

Selection composes last: when a row or GPIO cell is selected, `accent.select`
overrides the per-cell state colors across the element's full hit region
(full row for power/switch and single-cell GPIO rows, one half of the row
for a paired GPIO cell).

## 10. Interaction contract (frozen bindings, projection-aware navigation)

Grounded in `events.rs`. Confirmation and Saved Config precedence is unchanged;
the GPIO-specific bindings below replace only the former GPIO direct-action
aliases.

- Quit: `q`, Ctrl-C.
- Confirm-modal keys: Enter/Space confirm, Esc cancel (both modal types).
- Esc on Saved Config page returns to Controls; Esc dismisses a Saved Config
  error.
- `p` pause/resume, `r` refresh, `c` toggle Controls <-> Saved Config,
  `s` save selected config items, `x` clear saved config.
- Tab / Shift+Tab cycle pages (frozen order).

Controls navigation uses a shared visual projection. The projected line list
is: 4 power rows, switch rows in BTreeMap order, then the GPIO visual rows
from section 6.1 in order. A selection address is (projected line, side)
where side is `Left` or `Right`; power/switch rows and single-cell GPIO rows
always have side `Left`. The projection is recomputed from the polled
firmware metadata on every status merge and whenever W crosses the pairing
threshold.

- Up/`k`, Down/`j`: move one projected line, preserving side: a right-half
  selection lands on the right cell of the target line when that line has
  one, otherwise on its left (or only) cell. Moving between the GPIO area
  and the power/switch area always lands on the target row itself (side
  `Left`). Saved Config moves the cursor by one item; Status scrolls by one
  row.
- Left/`h`, Right arrow only: GPIO paired mode only; moves the selection to the
  sibling cell of the same visual line. Left/Right is inert on
  power/switch rows, on single-cell GPIO rows, and at a row edge that has
  no sibling. It never crosses lines, groups, or sections.
- PageUp / Ctrl-U / `[` and PageDown / Ctrl-D / `]`: step by 3 projected
  lines (Saved Config `move_cursor(3)`, Status scroll delta 3) -- frozen
  from `page_step`.
- Enter/Space: primary action on a selected power or switch (Controls), toggle
  selection (Saved Config), no-op (Status), and inert on a selected GPIO.
- `g`: jump selection to the first projected GPIO line (left cell).
- `l`/`L`: drive the selected GPIO output LOW. `l` is not a navigation alias.
- `o`/`O`: drive the selected GPIO output HIGH.
- `i`/`I`: restore the selected GPIO to input.
- `0` and `1` are inert on GPIO and are not alternate direct-action bindings.
- Direct GPIO keys are inert outside Controls, on a non-GPIO selection, while
  a modal or GPIO job is pending, and for repeat/release events. A press that
  reaches a direct GPIO key first cancels any active GPIO gesture; it only
  dispatches when the current selection is an eligible GPIO.
- Resize (`Event::Resize`) is the canonical TUI redraw boundary. On
  Resize the runtime cancels any active GPIO gesture (clearing both the
  initial left-button Down state and the AwaitSecond state), clears the
  entire `hit_map` so no previously computed hit rectangle is reused for
  the new terminal geometry, and forces a redraw before the next queued
  input event is consumed; the ready-event drain also stops at Resize so
  a mouse event queued after a Resize is preserved for the new geometry
  rather than acting on the stale one.
- Projection transitions: when W crosses 48 columns or the firmware metadata
  changes, the selection stays on the same pin; its (line, side) address is
  recomputed from the new projection, and `ensure_visible` adjusts the
  scroll offset to the new projected line.
- Any keyboard selection movement, `g`, Escape, page transition, modal entry,
  GPIO job becoming pending, or removal of the tracked pin cancels an active
  GPIO gesture silently. No cancellation writes hardware state.

Mouse (frozen from `handle_mouse` and `hit.rs`):

- Hit regions: one rectangle per selectable DATA element on the Controls
  page: full-row for power/switch rows, half-row for each paired GPIO cell,
  full-row for each single-cell GPIO row. Modal Confirm/Cancel button
  rectangles are registered only when the modal fits, as today.
- GPIO identity is its firmware `name`, not a projected coordinate. A left
  Down on a GPIO selects that pin and arms a gesture only. The first left Up
  before the 600 ms HIGH deadline enters an await-second state with a LOW
  deadline exactly 220 ms later; a tick or input event at or after the
  deadline dispatches LOW through the existing GPIO worker path. Holding the
  initial or second left Down until its exact 600 ms deadline dispatches HIGH
  once; a matching left Up at or after that deadline also dispatches HIGH, and
  no LOW follows. A second left Down/Up on the same GPIO strictly before the
  LOW deadline dispatches input with no transient LOW. At or after the LOW
  deadline, the late Down dispatches the expired LOW for the original pin and
  is consumed; the next fresh Down starts a new gesture.
- A second Down on a different GPIO or a non-GPIO while awaiting the second
  press cancels and consumes that input. A Moved or left Drag report remains
  active when it resolves to the original GPIO cell; a report resolving to a
  different cell, pin, terminal column, or terminal row cancels the gesture.
  Middle and right mouse buttons are always inert. While a GPIO gesture is
  active, its cancellation conditions consume the input rather than activating
  another control.
- With no active GPIO gesture, left Down on a power or switch first calls
  `select_item`, then starts its primary activation. Power and every advertised
  switch therefore open hardware confirmation with no request while the
  clicked full-width row stays selected beneath the modal. Modal input and
  clicks outside a hit region remain inert as before.
- While any confirmation modal is active, only the modal's buttons respond;
  all other clicks are ignored. Clicks outside any hit region are ignored.
- Global mouse precedence is Saved Config confirmation, Saved Config error,
  hardware confirmation, tabs, then the active page. A Saved Config
  confirmation therefore consumes outside clicks plus underlying row, tab,
  and hardware-modal hits. Only left-button Down on its typed `[ Confirm ]` or
  `[ Cancel ]` hit is actionable; every other mouse event is inert.
- Tab mouse routing follows the same precedence: Saved Config confirmation,
  Saved Config error, and hardware confirmation all block tab hits. With no
  blocking state, only left-button Down on an exact tab label hit is routed to
  `set_page`; active-tab hits and the two-space inter-tab gaps remain inert.

## 11. Confirmation modal exception

The only bordered regions in the UI are the two confirmation overlays:

1. Hardware confirmation (`render_modal.rs`): centered, max width 64,
   height 7, `modal.border` red border, `modal.emphasis` title, Confirm and
   Cancel buttons. Rendered over a cleared rect.
2. Saved Config confirmation (`config_render.rs`): centered, max width 72,
   max height 7, `modal.border` red border, `modal.emphasis` title and
   danger message, lists the dangerous item IDs and the auto-restore
   warning. It renders the complete labels `[ Confirm ]` and `[ Cancel ]`
   without clipping and registers separate typed hit rectangles only when the
   complete button row fits inside the modal; otherwise neither partial button
   nor button hit is emitted. Enter / `[ Confirm ]` confirms, Esc /
   `[ Cancel ]` cancels.

Everything else -- status band, scope, tabs, table header, data rows,
keybar -- is borderless. No other overlay, popup, or bordered panel may be
introduced.

## 12. Test hooks (TestBackend contract)

Tests render through `ratatui::backend::TestBackend` at the canonical sizes
and assert against the buffer:

1. 80x24 with populated scope: STATUS at rows 1-2, SCOPE header at row 3
    and graph rows 4-9, TABS at row 10, TABLE HEADER at row 11, KEYBAR at
    row 24, and 12 DATA rows at rows 12-23. 120x32: same band rows through
    TABLE HEADER, KEYBAR at row 32, 20 DATA rows at rows 12-31.
2. With no ADC readings in the model, the scope occupies zero rows: TABS at
    row 3, TABLE HEADER at row 4, DATA starting at row 5.
3. No border-drawing characters anywhere in the buffer when no confirmation
    modal is active.
4. Scope header shows channel, power state, current, and `max=<scale>mA`;
    the scale equals `ceil(peak * 5 / 4)` for the visible window and is
    never a fixed 5000. Graph rows contain block glyphs when samples exist.
5. Three live channels reserve one blank gutter cell in the header and all six
    graph rows. Content splits and zero-based gutter columns are 15/15/15 at
    W=47 (15, 31), 16/15/15 at W=48 (16, 32), 26/26/26 at W=80 (26, 53), and
    40/39/39 at W=120 (40, 80). A single live channel keeps all W columns.
6. A selected power or switch row paints the `accent.select` background on
    every column of its line. A selected paired GPIO cell paints it only on
    its own half; the sibling half keeps its normal style. A selected
    single-cell GPIO row paints the full row.
7. GPIO cells render the exact marker+word forms `◌ IN LOW`, `◌ IN HIGH`,
    `○ OUT LOW`, `● OUT HIGH`; HIGH cells are Red + bold, LOW cells are
    DarkGray; an unfocused LOW cell has no White background.
8. At W >= 48 two GPIO cells share a visual row with independent half-row
    hit rectangles; below 48 each GPIO cell owns a full row with a full-row
    hit rectangle.
9. Column titles `TYPE`, `NAME`, `STATE-ROUTE`, `LIVE`, `MODE`,
    `DESCRIPTION` appear in the table header row in that order.
10. Keybar: the last rendered segment is always complete; key blocks are
    visually distinct from their labels.
11. No DATA row content wraps: every content line is at most W columns.
12. With a hardware confirmation active, the modal border characters appear
    in `modal.border` red, the keybar shows the confirm/cancel segment set,
    and control-row hit regions are inert.
13. Determinism: same model state plus same terminal size produces the same
    buffer, across repeated renders.
14. Pending: a GPIO cell targeted by `gpio_pending` appends the exact tag
    `[LOW…]`/`[HIGH…]`/`[INPUT…]` after the state suffix (Yellow + bold
    unselected; bold and fully visible under `accent.select` when
    selected); completion or failure of the job removes the tag on the next
    render. A cell too narrow for the tag drops the tag whole and keeps the
    state suffix whole.
15. Keybar: a selected GPIO cell leads with exactly `l LOW`, `o HIGH`,
    `i INPUT`, `Mouse click/hold/2x` in that order, all four complete at 47
    columns, followed by the trailing page segments in their frozen order;
    the Mouse segment drops whole below 43 columns. Enter/Space/`0`/`1` and
    middle/right mouse actions never appear. The general Controls keybar
    does not advertise any GPIO-only binding. Hold: an active left-button
    Down renders `[HOLD…]` on the tracked pin only (Yellow + bold, selection
    background composed last, pending action tag wins, tag drops whole
    before the state suffix); await-second renders no tag.
16. Display columns: every DATA line fills exactly W display columns at W
    in {47, 48, 80, 120}, measured with `unicode-width`; CJK glyphs are
    never split at a cell boundary and no line wraps.
17. GPIO gesture tests use explicit `Instant` values, never wall-clock sleeps.
    They cover the exact 220 ms LOW and 600 ms HIGH boundaries when an event
    arrives before the next tick, exactly-once dispatch, Up after hold,
    same-name double input without a transient LOW, and cancellation by
    different/non-GPIO second Down, cross-cell Moved/left Drag while same-cell
    reports remain active, navigation, Escape, page/modal/pending state, and
    pin removal.
18. Tab mouse tests render W in {47, 48, 80, 120} and assert the exact visible
    half-open rectangles from section 5.2, including active-label padding and
    excluding both two-space separators. Left Down switches Controls, Saved
    Config, and Status through `set_page`; active-tab hits, separators,
    non-left-Down events, Saved Config confirmation/error, and hardware
    confirmation are no-ops for page selection.
19. Saved Config row mouse tests assert that only visible item rows register
    full-width typed `ConfigItemId` hits after scrolling; badges, loading,
    unavailable, error, `(none)`, header, and blank rows never register hits.
    Left Down re-resolves the ID against the current authoritative items,
    focuses the page, moves the cursor, and toggles once; stale IDs and every
    non-left-Down mouse event are inert.
20. Saved Config confirmation tests assert complete `[ Confirm ]` and
    `[ Cancel ]` labels and typed rectangles only when both fit. Confirm
    consumes the confirmation and starts exactly one confirmed Save request;
    Cancel starts none; outside, underlying-row, tab, non-left-Down, and stale
    modal-hit events remain inert under modal precedence.
21. Switch action tests use both firmware `requires_confirm` values and assert
    the same `Activation::Confirm` result while preserving missing-state ignore,
    no-route rejection, and firmware route ordering. Dedicated switch mouse and
    render tests assert no PUT on first activation, exactly one PUT on a fresh
    confirm, zero PUTs on cancel/timeout, click-before-activate selection, MODE
    `confirm`, and at 80x24 a full-width White + reversed selected row in every
    visible cell not covered by the hardware modal.

Existing behavior suites remain authoritative for the frozen semantics:
`events_tests.rs`, `actions_tests.rs`, `mouse_tests.rs`,
`config_state_tests.rs`, `config_result_tests.rs`, `confirm_tests.rs`,
`pages_tests.rs`, `navigation_tests.rs`. Render-side suites
(`render_tests.rs`, `render_chrome_tests.rs`, `page_tabs_tests.rs`,
`config_render_tests.rs`, `dense_layout_tests.rs`) are rewritten against
this contract; they may change what they assert about layout, but not about
action, confirmation, or model semantics. Navigation and mouse suites gain
projection-aware cases (sibling moves, side preservation, half-row hits)
without weakening the frozen scroll/paging arithmetic.

## 13. Explicit freeze list

The redesign must not alter:

1. The request methods, paths, and bodies in section 3.1.
2. Poll intervals, tick interval, pause semantics, history cap (3.2).
3. `resolve_activation` outcomes: power and every switch with an advertised
   next route always confirm in the TUI; missing switch state is ignored and
   no-route rejection remains unchanged. Firmware `requires_confirm` remains
   untouched for Saved Config/API/CLI consumers. GPIO `Primary` is ignored and
   direct keyboard actions are `l`/`L` LOW, `o`/`O` HIGH, `i`/`I` input.
   Enter/Space/`0`/`1` are inert on GPIO. GPIO gestures use the existing worker
   path with no optimistic state-map write; middle/right mouse input is inert
   (3.3, 10).
4. Both confirmation state machines, the 3 s hardware timeout, dangerous-ID
   listing, and the input-precedence order (3.4).
5. Board facts: power output list and order, scope channel list, and firmware
   ownership of GPIO enumeration and layout metadata.
   The TUI must not hardcode connector names, row/column counts, pin maps,
   or any mirroring (3.5, 6.1).
6. `TuiModel` field ownership, `apply_status_snapshot` /
   `apply_adc_response` merge logic, pending/mismatch derivation. The one
   GPIO gesture field is transient local interaction state, cancels on a
   missing tracked pin, and never changes authoritative GPIO maps.
7. Per-page independent scroll offsets and the `clamp_scroll` /
   `ensure_visible` arithmetic, now counting projected DATA lines.
8. Firmware behavior, firmware boot defaults, and all hardware state
   ownership rules in the repository AGENTS.md.
