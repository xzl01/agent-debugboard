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
| `--c-identity` | `116 188 31` | `116 188 31` | Fixed Radxa/Linkr product identity |
| `--c-ok` | `5 150 105` | `52 211 153` | Success, toggle-on track, online state |
| `--c-warn` | `180 83 9` | `251 191 36` | Warning accents (VIO control, pending risk) |
| `--c-danger` | `220 38 38` | `248 113 113` | Errors, destructive actions, offline state |
| `--c-ink` | `15 23 42` | `241 245 249` | Primary text |
| `--c-ink-dim` | `71 85 105` | `148 163 184` | Secondary text, icons, hints |
| `--c-terminal` | `255 255 255` | `7 9 12` | Terminal surface and modal backdrop tint |
| `--c-terminal-ink` | `15 23 42` | `226 232 240` | Terminal foreground |
| `--c-scroll` | `148 163 184` | `71 85 105` | Scrollbar thumb (CSS only, no Tailwind alias) |
| `--c-overlay` | `15 23 42` | `0 0 0` | Drawer and modal backdrop |
| `--c-gpio-low` | `15 23 42` | `148 163 184` | Neutral Safe-GPIO LOW indicator; contrasts with connector and panel surfaces in both themes (CSS only, no Tailwind alias) |
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

- Product identity lockup (`public/linkr-logo.svg`) and compact mark
  (`public/linkr-mark.svg`): the Radxa wordmark supplied as the brand
  reference contributes its rounded geometric construction, open terminals,
  fixed `#74BC1F` identity green, and the four-path `x` mark. Linkr uses those
  four original wedge paths unchanged inside a padded vector crop
  (`viewBox="292 94 122 113"`) rather than reinterpreting or raster-cropping
  their geometry. The full lockup places that original mark before the custom
  `linkr` wordmark; the browser favicon reuses the
  compact mark. The header renders the lockup as a dimensioned decorative SVG
  while retaining the localized `app.title` in an `h1.sr-only`. These
  self-contained external SVGs keep their fixed fill; the animated inline
  lockup uses the fixed `--c-identity` token. Neither replaces the semantic
  `brand`, `ok`, `warn`, or `danger` UI ramps.
- Shared measurement sparkline (`PowerSparkline.tsx`): one reusable SVG/history
  implementation serves rail current/power and firmware-reported ADC voltage
  channels. It retains up to 600 browser-observed samples per channel, previews
  the latest 90, uses the existing inset surface (`rounded-lg
  border-line/50 bg-panel2/35`), technical values in `font-mono`, token-only
  grid/trace colors, and a localized meaningful `aria-label`. The four current
  channels preserve their current/voltage/power overlay and independent scales.
  Power rows enumerate every output in firmware order, then current descriptors
  without an output. Missing instrumentation is a plain, neutral `text-[11px]`
  full-row note, not three zero-valued metrics or a fabricated sparkline. State,
  control and timers remain available independently of ADC coverage. Timers use
  reported names and reset on off/removal, with ≥ for first-observed on state.
  No parent/child toggle or client boot default is imposed.
  `PowerRailCatalogProvider` shares current output/ADC descriptors with power
  analysis and nested workflow inspectors. Power actions offer controllable
  outputs; capture/current-read choices require current descriptors; startup
  requires both. Missing draft names remain visibly unavailable instead of
  being silently retargeted to another rail. Historical comparison sources come
  from recorded descriptors rather than the current board's rail list.
  The PD panel owns `role="pd_input"` ADC0 and displays its canonical
  source-side DC20V value; `raw`/`mv` stay in diagnostics. ADC1-3 render as
  three native cards above the switch routes using
  `grid grid-cols-1 gap-3 sm:grid-cols-3`. Each native card uses
  `rounded-lg border-line/60 bg-panel2/40 p-3`, displays the firmware name,
  signal and optional note verbatim, and has its own chart and latest value.
  Channel histories are keyed by role, name and signal, never by array position.
  Native ADC charts use a fixed `0..3,300,000 uV` (0..3.3 V) scale; ADC0
  autoscales with a minimum 1 V span and is capable of the approximately 25.3 V
  source range. No host divider conversion, calibration table, or pin map is
  applied. ADC0 is not rendered as a 3.3 V native card.
  Unreported or invalid readings are never fabricated or replaced with zero;
  missing samples break history instead of joining across the gap. Compact mode keeps identities and latest values but no charts.
  Voltage channels have no controls, trigger, probe scaling, capture or export
  affordance; the Power analyzer capture stays current-only.
- Watchdog status badges use a wrapping `flex` row with `gap-2`; long localized badges move to the next line instead of overflowing the card.
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
- J4 terminal: one xterm.js surface, with a visible `xterm.js · MCU UART`
  renderer identity even while disconnected, and no
  dual-UART tabs, split layout, external-device chooser, Host archive drawer,
  or browser-security setup modal. Connect/disconnect stays directly in the
  wrapping toolbar. UART settings expand inline with `aria-expanded`, actual
  firmware values and generation, explicit Apply, pending state, and a visible
  USB ACM/DTR ownership note. No host, noVNC, Telnet, or SSH transport controls
  appear. Icon controls use 40px targets and localized labels/titles; the
  screen-reader-enabled terminal retains keyboard focus, font controls,
  ResizeObserver fit, theme updates, and fullscreen fit on mobile/desktop.
- J4 baud is a custom numeric field constrained to the firmware range
  4800..115200. Five recommended presets are convenience choices, not a
  whitelist. The page confirms Apply through authoritative GET/WS generation
  state; CDC SET_LINE_CODING cannot report a later application rejection.
  Write deadlines derive from active framing and the bounded eight-slot queue,
  with a 2-second scheduling margin.
- J4 owns a separate MCU live-session WS, never the ADC connection. Binary RX
  goes straight to xterm's streaming byte decoder. Binary TX is <=480 bytes per
  message with <=8 outstanding and one MCU in-flight message; browser-buffer pressure is visible and never
  silently queues an unlimited paste. UART-complete events, not accepted/socket
  sends, advance TX. Read-only, suspended, faulted, and configuring states stop
  input. The App attempts the shared session once after board availability,
  and once per explicit availability/restart recovery epoch; uptime rollback
  alone is never an epoch signal. A failed/unavailable/closed connection or manual Disconnect stays
  quiet through later status ticks; there are no retry timers. Manual Connect
  remains available. Every new connection uses a fresh independent session,
  deletes only its own old one and never replays old TX. StrictMode's effect
  probe does not consume or duplicate the single automatic attempt. Missing
  serial firmware is a calm Unavailable state. Drops are warn-toned, errors inline, and
  current settings remain the authority after a conflict. A per-instance 1 MiB
  raw RX tail supports byte-exact download; it is not a persistent Host archive.
- Explicit UART TX is multiwriter. Only automatic terminal replies select one
  responder: HTTP `terminal_responder` is a global descriptor/null, while WS
  `terminal_responder` is per-client boolean plus a `responder` descriptor.
  Explicit true authorizes this page's automatic replies; descriptors never
  infer the page's slot. DTR defensively disables Web automatic replies, not
  manual TX. The toolbar names the automatic responder and shows **Auto-reply
  observer** / **Manual input allowed** for nonresponders, not read-only.
- Nonresponders set xterm `disableStdin=true` to suppress DSR/DA/CPR inside its
  CoreService. Explicit keyboard input uses public `onKey` (before suppression
  in the pinned xterm); capture-phase paste sends once without reaching xterm's
  default handler. Active responders use only `onData`/`onBinary`, so keys and
  paste cannot double-send. Input mode synchronizes in the session subscription
  before the next RX frame. No escape-sequence filtering, owner acquisition or
  ownership-induced full-input lock is used. Actual UART/queue/configuration/
  automation safety gates, Ctrl-C, and generation renderer boundaries remain.
- Ordinary write rejection remains nonfatal: reject unaccepted/local bytes,
  retain accepted event IDs, keep RX connected and permit explicit new input.
  Never replay rejected bytes or reconnect merely because another terminal is
  selected for automatic replies. Default/manual connection policy is unchanged.
- The App-owned J4 session is shared by the terminal renderer, startup analysis,
  and automation through `useJ4Serial`/`J4SerialSession`, not terminal DOM state.
  Unmounting the renderer does not close an automation connection. The workflow
  schema, conditions, defaults, import/export, and state badges use only `j4`;
  there are no unpublished-channel aliases or browser-draft migrations. Serial
  preflight reads this backend's actual MCU state and refuses read-only,
  suspended, configuring, faulted, disconnected, or pending-write states before
  hardware operations. Automation ownership disables manual xterm input.
- The workflow library owns two IndexedDB stores: `workflows[workflow_id]`
  and `deployments[[task_store_id,task_id]]`. One workflow may deploy to many
  boards; local-only sources over 4000 bytes are browser-only and deploy IR only.
  Board-backed source reopens from `/tasks/slots/{slot}/source`; IR-only and
  built-in rows never claim lossless source. Workflow import/export and task-IR
  import/export are separate, and task import never runs hardware. The editor
  draft is separate from the saved library and is guarded across opens.
- Blocking confirmations elsewhere (Boot, OTA test boot, switch
  `requires_confirm`, task runs) currently use `window.confirm` with an
  interpolated message naming the target and consequence. Task-run
  confirmation binds to the already-resolved request snapshot (task ID,
  source, request count) and carries the non-transactional hardware warning;
  a cancelled run renders as partial/uncertain (`warn` tone), never as a
  rollback.
- PD requested voltage starts unknown (`voltage_mv: 0` is a client-side
  unknown sentinel, never a 0 V request). The initial draft is empty and its
  controls are disabled until an authoritative status/PD GET reports voltage.
  Neither 5 V nor the first advertised profile is a client default. A reported
  20 V is shown directly, including while capability discovery is pending;
  an older GET cannot replace a newer status voltage. Mounting/rendering never
  applies PD settings or saves persistent configuration.
- Routing controls and footer chips enumerate firmware-advertised switch
  entries, with raw-name fallback for future descriptors. The x1.2 catalog has
  `sd` and `tf_wp`; neither the status bar nor workflow options invent a USB
  routing switch. J8 USB D+/D− signals remain inert physical contacts, not
  routing controls. Synthetic `aux` tests retain generic route confirmation.

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
- Header identity state follows the existing board connection boundary and
  adapts the state-bound pulse mechanism from beui.dev `animated-badge`. The
  rounded backplate is reserved for low-frequency availability, so it never
  implies that power is enabled: Ready uses the neutral `panel2`/`line`
  treatment, Starting/Reconnecting uses `warn`, and Offline uses `danger`.
  `loading` keeps the four original `x` wedges chasing in 300ms steps (1.2s
  cycle), while Offline flashes all four together on the same 1.2s cycle.
- In Ready, the original wedge positions have fixed meanings. Upper-left is
  `5v_out`, upper-right is `12v_out`, lower-left is `20v_out`, and lower-right
  is reserved. A main-rail wedge is identity green only while the
  latest firmware snapshot reports that rail on; otherwise it stays dim
  `ink-dim`. The reserved wedge stays dim. J4 connection/readiness is reported
  by the workbench status badge using the shared MCU session state, not host
  process activity or a second transport's heartbeat.
- The original X leaves a central diamond; that existing negative-space shape
  is filled with `ok` and runs a 0.8s double heartbeat while the board is
  Ready. It is static under reduced motion and absent while Starting or
  Offline. No circle or raster overlay is introduced.
- An active logic-analyzer capture/stream temporarily overrides the four
  wedge meanings and rotates the complete original four-path rotor at one
  revolution per 480ms. Web activity is taken directly from the analyzer
  state machine; the resident Host counts binary Sigrok WebSocket sessions so
  the tray observes the same Host-routed activity without a firmware protocol
  extension. Connection state outranks analyzer state, and analyzer state
  outranks the steady rail/UART mapping. Under `prefers-reduced-motion:
  reduce`, the rotor stays static and fully identity green. The `linkr`
  wordmark never animates, and the adjacent localized Online/Offline text
  remains the connection-state authority.

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
  value is non-energizing (e.g. a routing change), tone `danger` when the value
  energizes hardware (power on or GPIO output). The badge always
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

The IO drawer has one GPIO surface: the complete physical connector diagram
inside `GpioCard.tsx`, directly above Watchdog on desktop and mobile. There is
no separate control list, Safe GPIO heading, or GPIO title/subtitle.
The eleven safe physical circles themselves carry the original gestures and live
level/direction. There are no static pin-key tables, duplicate connector lists,
visible level legend or long gesture paragraphs between the circles and
Watchdog. Complete instructions remain linked through aria-describedby as
sr-only text; only the quiet orientation line and mobile scroll tooltip remain
visible in the diagram card. All thirteen other physical
contacts remain inert, without button roles, tab stops or event handlers.

The sole physical geometry authority is GET
`/api/v1/gpio/layout`: `view: component-top`, ordered groups, explicit matrix
dimensions and every physical cell. The host has no pin map, connector-name
special cases, inferred cells, mirroring, compaction, or fallback diagram.
The final display contract is explicit and supersedes rotation/view wording:
**solder pins facing down; J13 on the left; J8 on the right; upright text;
no mirroring**. The localized caption reads **焊接引脚朝下** / **Solder pins
facing down**. `placePhysicalAssembly` places headers side by side in reverse
metadata order, with one matrix-unit gap. Within each header, source column
becomes display row and source rows run right-to-left. J8 is two rows of ten:
upper 1..10, lower 11..20, both left-to-right. J13 is 1/2 above 4/3.
Tests assert J13-left/J8-right/pins-down directly, every adjacency and the
signed orientation to prevent a mirrored implementation. No board-specific
mapping is copied into production code.

All glyphs remain upright: no SVG/CSS rotate transform is applied to text.
Signal purpose sits **inside** the double-ring circles: ADC0–ADC3, SDA/SCL,
USB D+/D− or hub purpose, power/GND, or a generic safe GPIO name. These are
formatted from firmware label/role metadata, not MCU-number mappings. Small
physical pin numbers sit outside the circle. Generic future dedicated contacts
may have brand-tinted double rings, their firmware-provided purpose inside, and a
tooltip/accessible name explaining that route controls own them. Raw signals
for these contacts are explicitly labeled **net name**, never interpreted as
MCU GPIO numbers or converted into gpioName. Full identity, signal and role
remain available in accessible names/tooltips instead of duplicate static
tables beneath the diagram.
Only `edge: right` opens the posed frame downward and adds downward side-exit
contacts; unknown edges retain a neutral frame. Decoration is non-interactive
and aria-hidden. The diagram stays at one CSS pixel per SVG unit so its
44px targets never shrink or overlap. Desktop shows the complete assembly.
On narrow phones only the pin diagram scrolls horizontally, with a visible
swipe hint; the page itself never overflows. Watchdog follows below in normal
vertical document order.

`gpioPhysicalLayout.ts` validates the response envelope, unique group names
and orders, bounded integer dimensions, exact rows×columns coverage, unique
coordinates and physical pin numbers within each connector, and consistent
control/output capability flags. Controllable cells require unique gpioName.
Every live SafeGpio must match exactly one controllable cell by **gpioName**,
and vice versa. Physical numbers are never MCU GPIO numbers. Unknown roles
are retained as neutral text, not promoted to controls.

`useBoardGpioLayout` reads once after the first successful status read per
connection epoch, including live mode. Disconnect invalidates metadata and
aborts its request; a reconnect reads again. Identity and AbortController
checks prevent late results from an earlier epoch or unmount replacing the
current layout. A layout failure does not make the board offline and does not
retry on every poll. The GPIO card instead shows a localized read-only error,
with no guessed host-side layout. A live board without the endpoint may keep
showing that error until its firmware is updated and it reconnects.

The current firmware reports full J8 2×10 and J13 2×2 matrices: 24 positions,
eleven interactive physical circles and thirteen inert sockets. GP17/GP18 are
absent. Every gesture is joined to the live GPIO by the exact gpioName and
guarded again at GpioCard's request boundary; physical numbers and raw net
names never become request identifiers. Inert cells have purpose/identity,
no fabricated level/direction, and no invisible hit circles.
The test-only
`gpioPhysicalLayout.testFixtures.ts` contains explicitly illustrative inert
signals and synthetic future connectors, never production fallback data.

LogicAnalyzerCard keeps the separate dynamic `GpioPinoutSvg` /
`groupGpioLayout` path and its current selectable safe-GPIO subset. Physical
inert connector cells are never logic-analyzer selection targets.

Level and direction remain independent: a LOW disc, a HIGH disc, a dashed
input ring and a solid output ring use the existing tokens and accessible
state labels. The complete short/double/hold/keyboard instructions are sr-only,
localized and connected to every interactive pin. Error/success feedback
still appears after the stable diagram when needed. There is no selection
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
  paint-only and animates no layout property. The animation carries no fill
  persistence (`forwards` is not used): the arc element unmounts when the
  hold completes or cancels, so no painted residue can outlive the gesture.
  It is transient feedback for
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
- GpioCard now renders the original concentric layers at 1 CSS px per SVG
  unit instead of `22 / 17`. Visible circle diameter is about 23% smaller,
  with the same radii, separate level disc, dashed/solid direction ring,
  hold arc, focus and pending rings. Its transparent hit circle is explicitly
  `r = 22`, preserving a 44px target without making the visible control
  larger. Physical columns are 46px apart, so targets never overlap or invade
  another control. The eleven physical targets have keyboard access
  and each control includes its connector/pin number in the accessible name.
  `GpioPin`'s legacy default `r = 17` remains unchanged for the independent
  dynamic pinout/logic-analyzer consumers.

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
  treatment, and an unmistakable busy indicator: a solid static warn-colored
  ring at `r = 16` (1.5px `rgb(var(--c-warn))`, no dash pattern, non-scaling,
  `aria-hidden`, no pointer events, no motion). The ring renders immediately
  below the brand focus ring without replacing it, and its static solid
  geometry reads identically under reduced motion. The
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
- Physical outputCapable metadata gates both GpioPin gesture dispatch and
  the GpioCard request entry. Input-only cells (including GP29) never request
  output LOW/HIGH through taps, holds or keys, and show no HIGH hold arc.
  The pre-existing double-tap and I input actions remain available; the
  input-only exception is included in the sr-only instructions and accessible pin name.
  Successful
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
