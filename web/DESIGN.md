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
| `--c-bg` | `248 250 252` | `9 12 18` | App background |
| `--c-panel` | `255 255 255` | `15 23 42` | Card/dialog surface |
| `--c-panel2` | `241 245 249` | `30 41 59` | Inset surface (stats, segmented controls, hover) |
| `--c-line` | `203 213 225` | `51 65 85` | Borders, dividers, toggle-off track |
| `--c-brand` | `37 99 235` | `96 165 250` | Primary accent, focus rings, active tabs |
| `--c-ok` | `5 150 105` | `52 211 153` | Success, toggle-on track, online state |
| `--c-warn` | `180 83 9` | `251 191 36` | Warning accents (VIO control, pending risk) |
| `--c-danger` | `220 38 38` | `248 113 113` | Errors, destructive actions, offline state |
| `--c-ink` | `15 23 42` | `241 245 249` | Primary text |
| `--c-ink-dim` | `71 85 105` | `148 163 184` | Secondary text, icons, hints |
| `--c-terminal` | `255 255 255` | `7 9 12` | Terminal surface and modal backdrop tint |
| `--c-terminal-ink` | `15 23 42` | `226 232 240` | Terminal foreground |
| `--c-scroll` | `148 163 184` | `71 85 105` | Scrollbar thumb (CSS only, no Tailwind alias) |

Opacity modifiers are part of the language: surfaces and borders commonly use
`/70` or `/60` (cards `border-line/70`, header `border-line/60`, backdrop
`bg-terminal/70`), tint fills use `/10`-`/15` (`bg-brand/10` icon chips,
`bg-ok/15` badges), and the selection highlight is `rgb(var(--c-brand) / 0.25)`.

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

Shadows: `shadow-sm` on cards and the sticky header; `shadow-2xl` reserved for
modal dialogs; `shadow-brand/20`/`shadow-danger/20` tint primary/danger
buttons.

Layout skeleton:

- Sticky header `sticky top-0 z-20` with `bg-bg/90 backdrop-blur-md`.
- Page container `mx-auto max-w-[1400px] px-4`; main adds `py-5`.
- Dashboard grid: `grid items-start gap-4 xl:grid-cols-[minmax(340px,400px)_minmax(0,1fr)]`.
- Left rail: `grid min-w-0 gap-4 sm:grid-cols-2 xl:grid-cols-1`; full-width
  items opt out with `sm:col-span-2 xl:col-span-1`.
- Advanced & recovery is a `<details>` disclosure (`group`, summary `min-h-16`,
  chevron rotates via `group-open:rotate-180`), content grid
  `lg:grid-cols-2 xl:grid-cols-1` with `border-t border-line/60 p-3`.
- Right workspace column is `xl:sticky xl:top-[116px]` and owns the tablist.
- Responsive rule: columns collapse to a single stack below `xl` (1280px);
  cards pair two-across between `sm` and `xl`. The 375/768/1280px widths are
  the QA breakpoints.

## 4. Primitives (`src/components/ui.tsx`)

Composition pattern: small prop-driven components merged with `cn()`
(`clsx` + `tailwind-merge`); callers extend via `className`/`contentClassName`.

- `Card`: `<section>` shell `rounded-2xl border border-line/70 bg-panel
  shadow-sm transition-colors duration-200`, optional header (icon chip
  `h-8 w-8 rounded-lg bg-brand/10 text-brand`, title, subtitle, `right`
  actions slot), content `p-4`.
- `Button`: variants `default` (panel2 neutral), `primary` (brand fill, white
  text), `danger` (danger fill, white text), `ghost` (transparent, dim text,
  hover panel2). Shared chrome: `inline-flex min-h-10 items-center
  justify-center gap-2 rounded-xl px-3 py-2 text-sm font-medium transition-all
  duration-150 active:scale-[0.97]`, focus ring, disabled
  `opacity-50 cursor-not-allowed` with `active:scale-100`. Icon-only buttons
  use `h-10 w-10 rounded-xl p-0`.
- `Toggle`: native `<button role="switch" aria-checked>`, track `h-6 w-11
  rounded-full` (`bg-ok` on / `bg-line` off), thumb `h-5 w-5 bg-white shadow`
  sliding `translate-x-5`, disabled `opacity-40`.
- `Badge`: tones `neutral | ok | warn | danger | brand` as 15%-tint fill, 30%
  border, full-tone text; pill `rounded-full border px-2 py-0.5 text-[11px]
  font-medium leading-none`, leading 12-16px icon allowed.
- `Stat`: labeled value block `rounded-xl border border-line/60 bg-panel2/50
  px-3 py-2.5`, uppercase 11px label, `text-lg font-semibold` value, optional
  hint.

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
  segment `bg-panel text-ink shadow-sm`, inactive `text-ink-dim hover:text-ink`.
- Workspace tablist: `role="tablist"` pill `rounded-xl border border-line/70
  bg-panel p-1 shadow-sm`, tabs `min-h-10 rounded-lg px-3 text-sm font-medium`,
  selected `bg-brand/12 text-brand shadow-sm`, roving `tabIndex` with
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
  `requires_confirm`) currently use `window.confirm` with an interpolated
  message naming the target and consequence.

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
- Entry motion: `animate-fade-up` (opacity + `translateY(8px)`, 0.32s
  `cubic-bezier(0.22, 1, 0.36, 1)`) on the dashboard grid only. All animation
  is GPU-composited (`transform`, `opacity`); theme swap is a 160ms
  background/color transition on `body`.

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

- `VITE_DISABLE_REACT_DEVTOOLS=1` suppresses loading in development; unset,
  empty, `0`, or any other value keeps diagnostics enabled
  (`shouldEnableReactDiagnostics`).
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
  `rounded-lg px-3 py-2`, `hover:bg-panel2/50`, and the standard brand focus
  ring. Item id remains `font-mono text-sm`; current/saved inset columns and
  trailing badges retain their established type and spacing. Selected rows use
  `bg-brand/10` fill while `aria-checked` exposes the same state without relying
  on color. Rows group under domain headers styled like stat labels
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
