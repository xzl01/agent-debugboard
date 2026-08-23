# Web UI Design Contract

This document codifies the design system that already ships in the Radxa Linkr
Debugger dashboard. It is extracted from the current implementation, not a
redesign: `tailwind.config.js`, `src/index.css`, `src/components/ui.tsx`,
`src/App.tsx`, `src/components/StatusBar.tsx`, `src/components/SerialCard.tsx`,
`src/components/SwitchCard.tsx`, and the rendered dashboard itself.

Rules for any UI work in `web/`:

- Every color, spacing, radius, shadow, and type value must reference a token
  or primitive named here. No raw hex codes and no ad-hoc pixel values.
- If a needed token does not exist, add it to this document first, then use it.
- Section 10 is a forward contract for the persistent-config card; it defines
  states and token mappings only, not JSX.

## 1. Color tokens

Colors are CSS custom properties holding space-separated RGB triplets,
consumed through Tailwind as `rgb(var(--c-*) / <alpha-value>)`. The `.dark`
class on `<html>` swaps the ramp; `color-scheme` follows the active theme.

| Token | Light | Dark | Role |
| --- | --- | --- | --- |
| `--c-bg` | `246 248 251` | `9 12 18` | App background |
| `--c-panel` | `255 255 255` | `15 23 42` | Card/dialog surface |
| `--c-panel2` | `244 247 250` | `30 41 59` | Inset surface (stats, segmented controls, hover) |
| `--c-line` | `214 222 232` | `51 65 85` | Borders, dividers, toggle-off track |
| `--c-brand` | `37 99 235` | `96 165 250` | Primary accent, focus rings, active tabs |
| `--c-ok` | `5 150 105` | `52 211 153` | Success, toggle-on track, online state |
| `--c-warn` | `180 83 9` | `251 191 36` | Warning accents (VIO control, pending risk) |
| `--c-danger` | `220 38 38` | `248 113 113` | Errors, destructive actions, offline state |
| `--c-ink` | `15 23 42` | `241 245 249` | Primary text |
| `--c-ink-dim` | `71 85 105` | `148 163 184` | Secondary text, icons, hints |
| `--c-terminal` | `255 255 255` | `7 9 12` | Terminal surface and modal backdrop tint |
| `--c-terminal-ink` | `15 23 42` | `226 232 240` | Terminal foreground |
| `--c-scroll` | `148 163 184` | `71 85 105` | Scrollbar thumb (CSS only, no Tailwind alias) |
| `--c-overlay` | `15 23 42` | `0 0 0` | Drawer and modal backdrop |
| `--c-gpio-low` | `15 23 42` | `2 6 23` | Safe-GPIO pinout LOW live-level disc (CSS only, no Tailwind alias) |
| `--c-gpio-on-level` | `255 255 255` | `255 255 255` | Label text on live-level GPIO discs (CSS only, no Tailwind alias) |

Opacity modifiers are part of the language: surfaces and borders commonly use
`/70` or `/60` (cards `border-line/70`, header `border-line/60`, backdrop
`bg-terminal/70`), tint fills use `/10`-`/20` (`bg-brand/10` icon chips,
`bg-ok/15` badges, `bg-brand/15` selected-row fill with `bg-brand/20` hover),
and the `::selection` text highlight in `index.css` is
`rgb(var(--c-brand) / 0.25)` (text selection only; row selection uses the
Section 10 fill/stroke treatment, not a separate `/25` fill).

Semantic text tone helper: metrics escalate `text-ink-dim` -> `text-warn` at
75% -> `text-danger` at 90% (see `ramMetricTone` in `StatusBar.tsx`).

## 2. Typography

Font stacks (Tailwind `fontFamily`):

- `font-sans`: `ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Helvetica, Arial, sans-serif`
- `font-mono`: `ui-monospace, SFMono-Regular, Menlo, Consolas, monospace`

Scale in use:

| Class | Size | Usage |
| --- | --- | --- |
| `text-[11px]` | 11px | Badge text, stat labels (uppercase `tracking-wide`), hints, footer |
| `text-xs` | 12px | Subtitles, secondary labels, status metrics, dialog body |
| `text-sm` | 14px | Card titles (`font-semibold tracking-wide`), buttons, tabs, body |
| `text-lg` | 18px | Stat values (`font-semibold`) |

Weights: `font-medium` (buttons, badges, tabs), `font-semibold` (titles,
values). Mono is reserved for technical values: endpoints, GPIO names,
measurements. `truncate` guards every title/label that can overflow; body text
uses `-webkit-font-smoothing: antialiased`.

## 3. Spacing, radius, shadow, layout

Spacing follows the Tailwind 4px base scale. Recurring values: `gap-1.5`/`gap-2`
inline icon text, `gap-3` card headers and toolbar groups, `gap-4` card grids,
`p-3`/`px-4 py-3` card chrome, `p-4` card content, `px-4` page gutters.

Radius (config overrides `xl` to `0.9rem`):

| Token | Value | Usage |
| --- | --- | --- |
| `rounded-md` | 6px | Segmented control segments, small selects |
| `rounded-lg` | 8px | Icon chips in cards, tabs, inline banners |
| `rounded-xl` | 0.9rem | Buttons, icon buttons, stat blocks, dialogs, header chips |
| `rounded-2xl` | 16px | Cards, Advanced section shell |
| `rounded-full` | full | Badges, toggle track/thumb, step counters |

Shadows are reserved for overlays: cards and controls use a border or tonal
surface, never a border plus a decorative shadow. `shadow-2xl` is reserved for
modal dialogs and the hardware drawer; toggle thumbs keep their functional
`shadow` so they remain legible over the track.

Layout skeleton:

- Sticky system header `sticky top-0 z-30`; the workspace navigation is a
  separate 44px horizontal scan line and becomes sticky below the header on
  desktop.
- Page container `mx-auto max-w-[1440px] px-6`; main adds `pt-5` and bottom
  room for the fixed status strip.
- The selected workspace occupies the full content width. Frequent power,
  routing, recovery, GPIO, and watchdog controls live in the right-side
  hardware drawer rather than competing with the current task. The drawer is
  the only GPIO surface: the workspace tablist holds exactly five tabs
  (terminal, power analysis, logic analyzer, automation, configuration) with
  no GPIO workspace tab or panel. The drawer overlay closes on an exact
  backdrop click (`event.target === event.currentTarget`) in addition to
  Escape and the close button; clicks anywhere inside the dialog keep it
  open. The selected drawer section (`power` | `io`) persists under the
  `linkr-hardware-controls-section` localStorage key: it is read once
  lazily at mount (missing, invalid, or unreadable values fall back to
  `power`), written best-effort on every section switch (storage failures
  never block the switch), and preserved across drawer opens from both the
  global navigation and the automation focus flow — neither open path
  forces a section reset.
- Power and logic workspaces use an F-pattern mode header: a 220-280px identity
  block followed by a mode switch capped at 760px. Together they occupy roughly
  the first 75% of a desktop content row and preserve a strong shared left edge.
- Responsive rule: the mode header stacks before `lg`; drawer card grids and
  result grids collapse without moving the primary workspace behind hardware
  controls. The 375/768/1280px widths are the QA breakpoints.

## 4. Primitives (`src/components/ui.tsx`)

Composition pattern: small prop-driven components merged with `cn()`
(`clsx` + `tailwind-merge`); callers extend via `className`/`contentClassName`.

- `Card`: `<section>` shell `rounded-2xl border border-line/80 bg-panel
  transition-colors duration-200`, optional header (icon chip
  `h-8 w-8 rounded-lg bg-brand/10 text-brand`, title, subtitle, `right`
  actions slot), content `p-4`.
- `Button`: variants `default` (panel2 neutral), `primary` (brand fill, on-brand
  text), `danger` (danger fill, on-danger text), `ghost` (transparent, dim text,
  hover panel2). Shared chrome: `inline-flex min-h-10 items-center
  justify-center gap-2 rounded-xl px-3 py-2 text-sm font-medium transition-all
  duration-150 active:scale-[0.99]`, focus ring, disabled
  `opacity-50 cursor-not-allowed` with `active:scale-100`. Icon-only buttons
  use `h-10 w-10 rounded-xl p-0`.
- `Toggle`: native `<button role="switch" aria-checked>`, track `h-6 w-11
  rounded-full` (`bg-ok` on / `bg-line` off), thumb `h-5 w-5 bg-white shadow`
  sliding `translate-x-5`, disabled `opacity-40`.
- `Badge`: tones `neutral | ok | warn | danger | brand` as 15%-tint fill, 30%
  border, full-tone text; pill `rounded-full border px-2 py-0.5 text-[11px]
  font-medium leading-none`, leading 12-16px icon allowed.
- `Stat`: labeled value block `rounded-xl bg-panel2/70
  px-3 py-2.5`, uppercase 11px label, `text-lg font-semibold` value, optional
  hint.
- `WorkspaceModeHeader` + `WorkspaceModeTab`: shared identity and sub-function
  switch for analysis workspaces. The header follows the 280px + 760px scan
  grid; the tab keeps icon, title, and one-line purpose together. Selected
  state uses a restrained brand icon fill and inset ring, not elevation.

Composite patterns outside `ui.tsx` that are part of the language:

- Shared measurement sparkline (`PowerSparkline.tsx`): one reusable SVG/history
  implementation serves rail current/power and GPIO29/ADC3 voltage. It retains
  exactly 90 samples, uses the existing inset surface (`rounded-lg
  border-line/50 bg-panel2/35`), technical values in `font-mono`, token-only
  grid/trace colors, and a localized meaningful `aria-label`. Rail current and
  power preserve their existing auto-scale plus `brand`/`warn` traces. ADC3 is
  monitor-only, renders only when firmware reports `adc3`, uses a fixed
  `0..3,300,000 uV` (0..3.3 V) scale, and formats the latest value to three
  decimal volts. Its section sits below the current rows and above the Power
  analyzer, with no controls, trigger, probe scaling, or export affordance.
- Segmented control (switch routes, layout pickers): container `grid
  grid-cols-2 rounded-lg border border-line/70 bg-panel2/60 p-0.5`, segments
  `min-h-9 rounded-md px-3 py-1 text-xs font-medium transition-colors`, active
  segment `bg-panel text-ink ring-1 ring-inset ring-line/60`, inactive
  `text-ink-dim hover:text-ink`.
- Workspace tablist: full-width horizontal scan line below the system header;
  tabs are `min-h-9 rounded-lg px-3 text-[13px] font-medium`. The selected tab
  uses a light brand tint, inset ring, and 2px bottom marker, plus roving `tabIndex` with
  Arrow/Home/End keyboard navigation (`getNextWorkspaceTabIndex`), panels
  `role="tabpanel"` with `hidden` toggling and `aria-labelledby`.
- Unreachable banner: `rounded-xl border border-danger/30 bg-danger/10 px-4
  py-3`, danger icon + title, dim detail, retry `Button`.
- Inline error note: `rounded-lg border border-danger/30 bg-danger/10 px-3
  py-2 text-xs text-danger`.
- Modal dialog (SerialCard setup): backdrop `fixed inset-0 z-50 flex
  items-center justify-center bg-terminal/70 px-4 py-6` closing on outside
  click; dialog `role="dialog" aria-modal="true" aria-labelledby` `w-full
  max-w-lg rounded-xl border border-line/80 bg-panel shadow-2xl`, header row
  with ghost close icon button, body `px-4 py-4 text-xs text-ink-dim`. While
  open: focus moves into the dialog, Tab is trapped across
  `FOCUSABLE_SELECTOR`, Escape closes, `body` scroll locks, focus restores to
  the opener on close. Modal overlays render through a `document.body` portal
  so transformed page ancestors cannot establish their fixed-position
  containing block. Dialogs use `max-h-[calc(100dvh-3rem)] overflow-y-auto`,
  matching the overlay's `py-6`, so short dynamic viewports keep every section
  reachable without clipping.
- Blocking confirmations elsewhere (Boot, OTA test boot, VIN change, switch
  `requires_confirm`, task runs) currently use `window.confirm` with an
  interpolated message naming the target and consequence. Task-run
  confirmation binds to the already-resolved request snapshot (task ID,
  source, request count) and carries the non-transactional hardware warning;
  a cancelled run renders as partial/uncertain (`warn` tone), never as a
  rollback.

## 5. Interaction states

- Focus: `focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40`
  on every interactive primitive; warn-scoped controls (VIO select) use
  `focus-visible:ring-warn/40`. The Advanced `<summary>` uses the same ring.
- Hover: color or surface shifts only (`hover:bg-panel2`, `hover:text-ink`,
  `hover:bg-brand/85`); motion is limited to `active:scale-[0.97]` press
  feedback on buttons.
- Disabled: `opacity-50` (buttons/selects) or `opacity-40` (toggle) plus
  `cursor-not-allowed`.
- Busy/loading: `Loader2`/`RefreshCw` with `animate-spin text-brand`; skeleton
  is not used - the first load shows a centered spinner + `text-sm` label.
- Entry motion: `animate-fade-up` (opacity + `translateY(8px)`, 200ms
  `cubic-bezier(0.22, 1, 0.36, 1)`) on the workspace grid only. The hardware
  drawer enters in 200ms and theme swaps take 180ms. All animation is
  GPU-composited (`transform`, `opacity`) and operational feedback stays within
  the 150-200ms enterprise-tool range.

## 6. Accessibility contract

- Touch targets: interactive controls are at least 40px tall (`min-h-10`
  buttons/tabs, `h-10 w-10` icon buttons, 44px-wide toggle track); 44px is the
  target for any new row-level control.
- Every icon-only control carries `aria-label` (and matching `title`).
- Toggle state is exposed via `role="switch"` + `aria-checked`; segmented
  layout pickers use `aria-pressed`; tabs use the full tab/tablist/tabpanel
  pattern with keyboard navigation.
- Copy/status feedback uses `aria-live="polite"` regions.
- `prefers-reduced-motion: reduce` collapses all animation/transition
  durations to 0.01ms and disables smooth scrolling globally (`index.css`).
- CJK: the UI ships `en` and `zh` dictionaries; `document.documentElement.lang`
  switches to `zh-CN`. Never hardcode user-facing strings; add keys to both
  locales with `{placeholder}` interpolation.

## 7. Themes

Light and dark are complete ramps over the same tokens (Section 1). Theme
resolution order: `localStorage.theme` (`light`|`dark`) ->
`prefers-color-scheme` -> dark fallback. `index.html` applies the class before
first paint; `ThemeProvider` keeps `html.dark`, `data-theme`, and
`color-scheme` in sync. New UI must render correctly in both themes by only
referencing tokens, never fixed colors.

## 8. Development-only React diagnostics

`react-grab` and `react-scan` are devDependencies loaded from
`src/lib/devtools.ts` via dynamic `import()` gated on `import.meta.env.DEV`.
The production build statically folds the guard to `false` and eliminates both
packages; `scripts/check-dev-diagnostics.mjs` (wired into `npm test`) fails if
the wiring tokens disappear, a static import appears, or either package name
lands in `dist/`.

- Product previews keep diagnostics hidden by default. Set
  `VITE_DISABLE_REACT_DEVTOOLS=0` explicitly when the React diagnostics overlay
  is needed (`shouldEnableReactDiagnostics`).
- Loader failure is non-fatal: it logs one `console.warn` and the app boots
  normally. Diagnostics initialize before the first `createRoot().render()`.

## 9. Do / don't summary

- Do compose `Card`/`Button`/`Toggle`/`Badge`/`Stat`; extend with `className`.
- Do keep risk visible without relying on color alone (icon + text + tone).
- Don't add new hues; warn/danger/ok/brand cover all semantics.
- Don't introduce layout-property animations or new keyframes without a state
  change to communicate.
- Don't restyle existing cards while adding new ones; consistency beats local
  polish.

## 10. Forward contract: persistent-config states (for the Saved Config card)

Proposed states and token mappings for the future card. These define visual
and interaction behavior only; implementation lands with the card itself.

- Selectable config row: full-width `<button type="button" role="checkbox">`
  with `aria-checked`, localized `aria-label`, and `aria-disabled`; no native
  checkbox input or visible checkbox glyph renders. Native button activation
  provides click, Space, and Enter behavior. During loading or another busy
  state, the row remains focusable and keeps focus: use `aria-disabled` plus an
  activation guard, never native `disabled`. The row uses `min-h-11` (44px),
  `rounded-lg px-3 py-2`, and the standard brand focus ring. Item id remains
  `font-mono text-sm`; current/saved inset columns and trailing badges retain
  their established type and spacing. Unselected rows hover with
  `hover:bg-panel2/50`. Selected rows use a `bg-brand/15` fill with an inset
  `ring-1 ring-inset ring-brand/30` stroke, and hover deepens the fill to
  `hover:bg-brand/20` so the selection identity survives pointer hover and
  stays distinct from both the panel surface and the unselected hover state in
  either theme; `aria-checked` exposes the same state without relying on color. Rows group under domain headers styled like stat labels
  (`text-[11px] uppercase tracking-wide text-ink-dim`).
- Risk badge: `Badge` tone `warn` when `requires_confirm` is true and the
  value is non-energizing (e.g. USB route), tone `danger` when the value
  energizes hardware (power on, VIN 1.8V, GPIO output). The badge always
  carries an icon (`ShieldAlert`) plus text; color is never the only signal.
- Apply-state badge mapping: `not_saved` -> `neutral`, `applied` -> `ok`,
  `pending` -> `warn`, `failed` -> `danger`, each with a short text label.
- Danger confirmation (save or apply of dangerous ids): modal dialog reusing
  the Section 4 modal pattern. The body lists every dangerous firmware item id
  in `font-mono` inside an inline error-note block (`border-danger/30
  bg-danger/10`); actions are `Button variant="danger"` for confirm and
  `Button variant="default"` for cancel. Focus starts on cancel, Tab is
  trapped, Escape cancels, and confirming issues exactly one confirmed
  request - never a local-only state change.
- Clear confirmation: same modal, `warn` accent, with explicit copy that
  clearing removes the saved snapshot but does not change current hardware.
- Busy state: actions disabled with the standard `disabled` treatment and a
  spinner on the in-flight button; the card shows an inline note that another
  operation (capture/OTA) holds the board.
- Unavailable/old firmware: card renders a neutral `Badge` plus
  `text-xs text-ink-dim` explanation; no controls render.
- Partial apply failure: inline error note naming the failed item id, with
  applied items keeping `applied` badges and remaining items `pending`.
- Always-visible content: the card has no top-level collapse/expand
  disclosure. On the independent Configuration -> Saved tab the content
  renders immediately below the header with no parent toggle button, no
  `hidden` region, and no local expand state. The `Card` header `right`
  slot carries only the status `Badge`. Group-level organization stays
  with the nested `details` groups inside `PersistentConfigRows`: power
  and switch groups default to open and the GPIO group defaults to
  closed, each driven by mount-only React state with no persistence.
  Because the React subtree always stays mounted and visible, selection,
  loading, busy, notice, and confirmation model state is retained
  untouched. The danger/clear confirmation keeps its Section 4 portal
  modal behavior unchanged, including focus restore to its opener. No
  layout-property animation, no new keyframes, and no new token: the
  card composes only existing primitives and classes.

## 11. Safe-GPIO pinout live-level contract

The GPIO card (`GpioCard.tsx`) is a single compact pinout section: connector
SVG, a 2x2 four-swatch legend, and a localized gesture hint. The pinout
grouping is firmware-authoritative: `groupGpioLayout` (`gpioLayout.ts`)
derives every connector group, its row/column extents, and its ordering
solely from the `layoutGroup`/`layoutRow`/`layoutColumn` metadata in the
latest `SafeGpio` snapshot. The host owns no connector names, dimensions,
pin maps, or mirroring; group order follows first firmware occurrence and
extents come from the reported nonnegative integer coordinates. Pins with
missing, invalid, or duplicate-cell metadata are never dropped: they render
in a generic single-column fallback group (labeled `logicAnalyzer.fallbackGroup`)
in snapshot order, presented as a plain list that claims no board geometry.
The legend maps
level and direction independently: a LOW disc, a HIGH disc, a dashed ring
labeled input, and a solid ring labeled output, all reusing the existing
`gpio.low/high/input/output` strings and token colors. The hint is four
phrase-level `inline-block` chunks (short press, double press, hold, keys)
with separators between chunks, so CJK phrases such as `输出高电平` wrap
only as a unit at narrow widths instead of orphaning a character. There is
no selection
state, no selection ring, and no action row: pointers act on a pin directly
through a short/hold/double-press gesture, and the keyboard acts immediately.
GPIO pins bind no context-menu action. The pinout always reflects the latest
`SafeGpio` snapshot (HTTP poll or WebSocket live frame), never a locally
cached level or direction.

Pin visuals use concentric SVG layers so the live level can never bleed past
the direction boundary at any scale:

- Level disc (`r = 11.5`, no stroke): solid `rgb(var(--c-danger))` for HIGH
  (`value > 0`), solid `rgb(var(--c-gpio-low))` for LOW (`value === 0`).
  Reusing the danger ramp keeps "energized" semantics consistent with the
  risk-badge mapping in Section 10.
- Direction ring (`r = 14`, 2.5px `rgb(var(--c-ink-dim))` stroke,
  `fill="none"`): direction is never carried by color or fill; input uses
  `stroke-dasharray="3 2"`, output is solid. The neutral ink-dim ring is a
  separate element outside the level disc, so HIGH fill stays fully inside
  the dashed or solid boundary at every scale.
- Hold-progress arc (`r = 14`, 2.5px `rgb(var(--c-danger))` stroke,
  `fill="none"`): rendered only while a hold gesture is in progress on the
  pin. The danger ramp previews the energized HIGH action the hold will
  request. The arc shares the direction ring's exact `cx`/`cy`/`r`/stroke
  width, so hold progress sweeps directly over the dashed or solid direction
  boundary instead of floating on a separate radius; there is no neutral
  hold track and no hold-specific `r = 16` element — `r = 16` is reserved
  for the focus and pending rings, which never coexist with a pointer hold
  (keyboard activation is immediate and starts no hold). The arc is
  a separate element above the direction
  ring, driven purely by `pathLength="1"` with a `stroke-dasharray` of `1`
  and a `stroke-dashoffset` animating from `1` to `0`, so the sweep is
  paint-only and animates no layout property. It is transient feedback for
  an in-progress gesture, never a persistent state marker.
- Focus ring (`r = 16`, 1.5px `--c-brand` stroke, `fill="none"`): keyboard
  only, `opacity-0` by default and `group-focus-visible:opacity-100` — the SVG
  equivalent of the Section 5 brand focus ring. The pin group carries
  `focus:outline-none` so the native outline is suppressed for pointer and
  keyboard focus alike and the explicit circle is the only focus affordance.
  The logic-analyzer variant renders no such element and keeps its native
  class output.
- Direction ring, hold arc, and focus ring all carry
  `vector-effect="non-scaling-stroke"` so stroke widths survive `viewBox`
  scaling; all rings are concentric with the level disc and their radii
  account for centered strokes.
- The pin label on a live-level disc uses `rgb(var(--c-gpio-on-level))`
  (white), matching the existing white-on-danger `Button variant="danger"`
  precedent.
- The transparent hit-target circle (`r = 17`) stays the outermost interactive
  layer; in the supported card layout (SVG `max-width: 224px` over the
  firmware-reported connector groups viewBox) the rendered hit target is at
  least 44px.

Timing constants (GPIO variant only) — shared by every pointer path:

- `SHORT_PRESS_WINDOW_MS = 220`: after a short release, the output-LOW
  request waits this long for a possible second press before it fires.
- `LONG_PRESS_THRESHOLD_MS = 600`: a pointer still down at this threshold
  completes a hold.
- `POINTER_MOVE_TOLERANCE_PX = 8`: pointer movement beyond this distance
  (CSS pixels) from the `pointerdown` position cancels the gesture.

Interaction contract (GPIO variant only) — direct gestures, no selection:

- Pointer gestures are per-pin: gesture state is keyed by pin, a press on a
  second pin never combines with or cancels another pin's pending gesture,
  and state lives outside render so snapshot-driven re-renders never reset
  timers. Each pin captures the pointer on `pointerdown` so a deliberate
  stationary hold is not stolen, and the pin group sets the component-local
  `touch-action: manipulation` property — not `none` — so ordinary page
  scrolling stays available; when the browser takes over a scroll it fires
  `pointercancel`, which cancels the gesture without a write. Neither
  property is a theme or token change.
- Short press: `pointerdown` followed by release inside the hold threshold
  schedules an output-LOW request (`onSet(name, "output", 0)`) that fires
  only when the 220ms double window expires. Nothing is written on
  `pointerdown` or on the first release.
- Double press: a second `pointerdown` on the same pin inside the open window
  cancels the pending LOW; when that second press also releases short, the
  pin requests input acquisition (`onSet(name, "input")`, no value argument)
  exactly once. A hold as the second press follows the hold path instead.
- Hold: when the pointer stays down past the 600ms threshold (and movement
  stays within tolerance), any pending click candidate is cancelled and the
  pin requests output HIGH (`onSet(name, "output", 1)`) exactly once at the
  threshold. Releasing after a completed hold is inert — it writes nothing
  and schedules nothing.
- Cancellation without write: `pointercancel`, movement beyond 8 CSS px,
  losing an active pointer capture, Escape while a gesture is in progress,
  a pending transition (see below), and component unmount each cancel the
  in-progress gesture and any scheduled timer without issuing a request.
- Keyboard is immediate and inherits no pointer timing: Enter, Space, or `0`
  request output LOW; `1` requests output HIGH; `I`/`i` requests input
  acquisition. Each activation issues exactly one request, and key
  auto-repeat (`event.repeat === true`) is ignored. The pin group is a
  focusable `role="button"` whose accessible name is re-derived from the
  latest snapshot; it carries no `aria-pressed` or other selection state.
  Each pin exposes `aria-keyshortcuts="Enter Space 0 1 I"` and an
  `aria-describedby` reference to the card's static gesture hint, so screen
  reader users discover the same contract the hint shows sighted users.
- Pending gate: the card allows at most one in-flight GPIO request. A
  gesture that starts while a request is pending is inert, a timer that
  completes into a pending state is re-checked and dropped without writing,
  and a pending transition cancels any in-progress gesture. While any
  request is in flight every pin is gesture-blocked and exposes
  `aria-disabled="true"` with the not-allowed cursor, so no pin looks
  actionable while the card lock would drop its input; only the request's
  target pin additionally carries `aria-busy="true"`, the dimmed opacity
  treatment, and an unmistakable busy indicator: a warn-colored dashed ring
  at `r = 16` (1.5px `rgb(var(--c-warn))`, `stroke-dasharray="4 3"`,
  non-scaling, `aria-hidden`, no pointer events) that spins via the existing
  `animate-spin` transform motion. The ring renders immediately below the
  brand focus ring without replacing it, and the global reduced-motion rules
  freeze the spin into an equally visible static warn dashed ring. The
  gesture hint stays static text.
- Reduced motion: `prefers-reduced-motion` keeps every timing constant and
  every request rule unchanged; the hold-progress arc renders as an instant
  filled state instead of a sweep, so hold feedback remains meaningful state
  information rather than decorative motion.

Firmware authority and feedback:

- Mount and re-render never write GPIO state; the UI is non-optimistic and
  holds no local direction or level mirror. Pin name, direction, and level
  are re-derived from the latest `SafeGpio` snapshot on every render, and a
  request argument object always comes from that snapshot.
- The card holds no machine-readable per-pin output capability (GP29
  included): unsupported output requests are rejected by the firmware and
  surfaced through the standard inline `role="alert"` error. Successful
  actions announce a localized polite status (`role="status"`,
  `aria-live="polite"`).
- Async feedback (success status and error alert) renders after the stable
  pinout section in DOM order and expands below it with top spacing; it must
  never change the pin/SVG top-left geometry when mounted or cleared.
- The SVG root is `role="group"` (not `role="img"`) so pin children stay in
  the accessibility tree; the logic-analyzer variant keeps `role="img"`.

The shared `GpioPinoutSvg` keeps the logic-analyzer variant as its default:
selection/trigger colors, `role="img"`, three-swatch legend, and the existing
click/context-menu behavior are unchanged unless `variant="gpio"` is passed.
