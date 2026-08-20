# Radxa Linkr Debugger Web UI

React/Vite control interface for the debugger firmware HTTP and WebSocket APIs.
Production firmware embeds the gzip-compressed UI and serves it from
`http://172.29.203.1/` over USB NCM. The page uses same-origin `/api/v1`
HTTP and WebSocket endpoints, so normal board controls do not require a host
proxy.

## Development

```sh
cd web
npm ci
npm test
npm run dev
```

Open <http://127.0.0.1:5173/>. The board must be connected over USB NCM and
reachable at `172.29.203.1`.

On macOS, the operating system can allow `/usr/bin/curl` to reach the USB-NCM
interface while rejecting a direct Node.js socket with `EHOSTUNREACH`. The
`npm run dev` launcher detects the default NCM target and automatically starts
a loopback-only raw TCP forwarder, then points Vite at that local endpoint.
This covers both HTTP and WebSocket traffic and is stopped with Vite. The same
workaround is used by `npm run device-bridge`. Set
`LINKR_NCM_FORWARDER=off` only when Node already has working local-network
access; use `force` to exercise the forwarder on another platform.

The Power & current card includes a triggered power analyzer. It supports
manual, current-threshold, GPIO-edge, and power-on triggers, keeps four captures
for overlay comparison, and exports CSV or NDJSON. Captures use firmware device
timestamps and a pre/post-trigger ring buffer instead of browser timing.

The Power card consumes the four-descriptor ADC telemetry contract
documented in [doc/adc-telemetry.md](../doc/adc-telemetry.md):

- One row per controllable current rail (`5v_out`, `12v_out`, `20v_out`),
  each with a shared `MeasurementSparkline` (`mode="power"`) that runs at
  10 Hz and keeps a 90-sample rolling window (about 9 seconds of
  history). Current and power share the same SVG history; both
  auto-scale, the unit minimum is fixed per metric.
- A monitor-only `adc3` (`GP29`) section rendered between the current
  rows and the PowerAnalyzer, only when the firmware reports an `adc3`
  reading. It reuses the shared `MeasurementSparkline` in
  `mode="voltage"`, the same 90-sample window and the same 10 Hz
  cadence, but with a fixed 0..3,300,000 µV Y scale (nominal 0..3.3 V).
  The voltage channel has no `power_enabled`; clients must treat the
  newest `value` as the canonical signed integer microvolt reading and
  must not apply any host-side ADC calibration.
- PowerAnalyzer remains a current-only triggered capture story:
  2048 samples, three current channels, manual / current-threshold /
  GPIO-edge / power-on triggers, four-overlay comparison, CSV/NDJSON
  export.

GP29 is in the persisted/safe catalog but is input-only while owned by
the `adc3` voltage monitor; ordinary GPIO output commands against GP29
must fail at the firmware layer. HTTP `GET /api/v1/adc/read` stays the
rich read path; the live WebSocket frame is intentionally compact (no
`raw`/`mv`/`current_ua`/`sensor_value` on the WS wire). The compact
WebSocket shape is a deliberate atomic breaking change for any client
that previously consumed verbose telemetry over WebSocket; there is no
dual-emission shim.

## Production build

```sh
npm run build
```

Generated files are written to `web/dist/` and are intentionally ignored by
Git.

The canonical firmware build runs `npm ci` and `npm run build:firmware`
automatically, verifies the fixed embedded asset set, and converts the app plus
decoder files into gzip-compressed Zephyr HTTP resources. Node.js 22, npm,
Rust with the `wasm32-unknown-unknown` target, and `wasm-bindgen` must therefore
be available when building firmware from a clean checkout. Run
`npm run build:firmware` directly to reproduce that fixed-asset build from the
Web workspace; unlike the normal build, it disables JavaScript code splitting.
The board-hosted decoder public contract is stable for the future React loader:
import `/assets/decoder/logic-decoder.js`, which loads
`/assets/decoder/logic-decoder_bg.wasm` next to itself.

After flashing, connect the USB NCM interface and open:

```text
http://172.29.203.1/
```

## GitHub Pages

Pushes to `dev` deploy the production build to
<https://xzl01.github.io/agent-debugboard/>. The Pages build sets the Vite base
path automatically; local development continues to use `/`.

GitHub Pages serves the UI over HTTPS, while the board exposes its REST and
WebSocket APIs over HTTP on the USB-NCM network. Start the loopback device
gateway before using hardware controls from the hosted page:

```sh
npm --prefix web ci
npm --prefix web run build
npm --prefix web run host
```

The Pages build connects to `http://127.0.0.1:8787/api/v1`. By default, the
gateway forwards HTTP and WebSocket traffic directly to the firmware service at
`http://172.29.203.1` and supplies the CORS and Private Network Access
headers needed by the browser. Override the upstream with `LINKR_BOARD_URL`.
The Rust Host talks directly to the USB-NCM address on macOS and does not need
the Node/Ruby loopback workaround used by the legacy gateway.
The gateway listens on loopback only and does not expose board controls to the
LAN. Browser requests are limited to the official Pages origin and local
development origins. Additional trusted origins can be supplied as a
comma-separated `LINKR_TRUSTED_ORIGINS` value.

The power analyzer arms firmware-side manual, current-threshold, power-on, or
GPIO-edge captures. It overlays the latest four runs and exports CSV/NDJSON with
device timestamps and the complete trigger configuration. The latest capture
also reports duration, mAh, and Wh for the selected rail using trapezoidal
integration over the device monotonic timestamps.

The Web logic analyzer is **Sigrok-over-WebSocket only**. The browser creates a
live session through the same-origin `/api/v1/live-sessions` helper (or the
GitHub Pages loopback gateway) and then speaks the sigrok binary protocol on
the returned session WebSocket URL. Logic-analyzer sample data and control
frames remain on this binary WebSocket session.

Bounded **Arm** captures use the firmware sigrok session with requested sample
rates from 100,000 through 125,000,000 Hz (100kHz-125MHz) and a post-trigger
sample count from `1` through `65535` (`uint16`). Web UI bounded pre-trigger
supports `rising`, `falling`, and `either` only. The contract is
`pre_samples >= 1`, `post_samples >= 1`, and `pre_samples + post_samples <= 512`.
Requested rates are 1-25 MHz, and the selected physical plan must retain at
least `2 * ceil(actual_rate / 1000)` samples. SINGLE supports through 25 MHz,
FAST8 through 10 MHz and rejects 25 MHz, and WIDE11 through 5 MHz and rejects
10 MHz and 25 MHz. Before first connection, the UI permits local editing when
generic constraints pass, then uses real per-mode CAPS and rejects or disables
old firmware or a selected mode without mode flag bit 5 (`PRE_TRIGGER`, `1 << 5`).
HELLO server flags bit 0 (`CONFIG_V2`) and bit 1 (`GENERIC_PACKED_BURST`) are
separate capabilities. Completion is `pre_samples + post_samples`, with
`triggerIndex` equal to `pre_samples`. Stream still forces pre and post to zero;
trigger NONE, unsupported or high-rate generic packed burst, and ordinary deep
capture remain pre=0. See the [dated HIL report](../doc/testing/results/2026-07-28-logic-analyzer-pre-trigger-uart-hil.md).

Firmware reuses the prepared common packed ring/sink lifecycle. After prefill,
packed samples are the sole trigger authority, edge detection is performed in
software, and the exact `[T-pre, T+post)` window is frozen and drained. No new
IRQ pairing or buffer is introduced. The existing deep post behavior remains
when pre=0. At negotiated high rates with GENERIC_PACKED_BURST, post=0 uses
packed burst (exactly 100000 samples, then auto-STOP/drain); at lower non-packed
rates post=0 runs until the user stops.
HELLO server_flags bit 0 advertises CONFIG_V2 and bit 1 advertises GENERIC_PACKED_BURST;
the Web UI uses frame0x0b (CONFIG_V2_REQ, 16B) for bounded post above 65535.
With both flags, bounded pre=0 and post=65536..100000 uses the same common
packed pipeline at every otherwise supported rate and pin plan. WIDE11 at
requested 125 MHz remains invalid. The 100/125 MHz FAST8 and 100 MHz WIDE11
matrix applies specifically to high-rate post=0 auto-stop capacity bursts.

The v1 frame0x05 (12B) remains for bounded captures with post <= 65535
and the post=0 stream sentinel.

Validated Web results on the final WIDE11 freeze build: WS SINGLE rising/falling
post=512 at 5, 25, 50, 100 MHz; WS SINGLE EITHER post=512 at 5 and 100 MHz;
WIDE11 100 MHz / post=100000 exact capture. The full 54/54 TCP+WS matrix and
the 62/62 high-rate matrix are HIL-confirmed on the same build. The 54-case
matrix contains 36 bounded NONE cases and 18 five-second continuous NONE cases
across SINGLE/FAST8/WIDE11, 1/5/25 MHz, and both transports. All 18 continuous
cases ended via explicit capacity OVERRUN (4 before the first DATA frame); `pass` means the
lossless-or-stop contract held at the rate actually delivered, not that every
requested rate was sustained. The multi-wire mapping HIL drove only GP10 with
a UART-style stimulus: 8192 samples checked, 9 transitions decoded, GP11-GP20
remained low. Independent high-state mapping for GP11-GP20 and lane B is not
validated by this run and would require an external pattern generator. When an
OVERRUN or other error terminates the capture, the firmware stops the sampler
SMs and the endless DMA channels before draining committed data, so the
consumer never reads samples that hardware could overwrite after the terminal
event. CDC ACM shell BOOTSEL and combined-UF2 HTTP BOOTSEL recovery were
also confirmed on the same freeze build. Historical WIDE12 baseline
(100 MHz / post=100000): `doc/testing/results/2026-07-26-logic-analyzer-wide12-100k-hil.md`.

**Stream** mode also uses the sigrok live session. At negotiated high rates with
GENERIC_PACKED_BURST, post=0 captures exactly 100000 samples losslessly then
auto-STOP/drain (one capture, not continuous streaming). At lower non-packed rates
(1-25 MHz in browser), Stream mode sends `post_samples=0` and runs until stopped
by the user. Web Sigrok pin selection is limited to `GP10-GP20`; GP29 is excluded from LA
and remains input-only while used by ADC3; `GP7-GP9` are shown but disabled.

Completed captures can be previewed in the waveform view and exported as CSV or
PulseView `.sr` files. The browser decoder still operates on completed bounded
captures only, not on stream data. Native raw-TCP integrations for PulseView /
sigrok-cli are documented elsewhere in the repository and are separate from the
browser WebSocket path.

The logic analyzer lives in the **Terminal workspace** alongside the serial
terminal, not under Advanced & recovery. The browser-based Rust/WASM decoder
serves the stable URLs `/assets/decoder/logic-decoder.js` and
`/assets/decoder/logic-decoder_bg.wasm` with `application/wasm` MIME for the
binary and gzip compression. It decodes UART, I2C, and SPI protocols only and
is not a libsigrokdecode Python plugin compatibility layer. The capture and
decode workflow: arm capture, wait for done, then decode in-browser using the
WASM decoder with annotations rendered in the waveform view.

The serial card keeps independent UART0 and UART1 sessions. Tab mode switches
the visible terminal without disconnecting either channel; split mode shows both
terminals and routes keyboard input only to the focused terminal. Both channels
share the CH347F UART VIO level shown in the card header. Changing between 3.3V
and 1.8V requires an explicit risk confirmation; both active serial connections
are closed before the firmware VIN route is changed.
The console uses an xterm-compatible terminal surface: click the terminal and
type directly, without a separate command input. Terminal writes are serialized
so fast typing cannot contend for the Web Serial writer lock. The terminal only
renders data returned by the target; successful host writes are reflected by the
TX counter. Enter sends CRLF by default, matching the previous line-input
console; CR and LF modes are also available from the terminal toolbar.

## Target serial console

Edge and Chrome/Chromium can use Web Serial directly from secure origins such as
the local Vite server or GitHub Pages. Users must still click the **Web
Serial** button and accept the browser device chooser.

The board-hosted page runs at `http://172.29.203.1/`, which is plain HTTP
and therefore not a secure context by default. Ordinary web pages cannot
navigate to browser-internal `chrome://` URLs; the address must be copied and
pasted into the address bar manually. Edge also accepts this Chromium address.

To enable direct CH347 Web Serial from the board-hosted page:

1. Copy the Chromium flag-page address below.
2. Copy the exact origin address.
3. Enable the flag and relaunch the browser.

```text
chrome://flags/#unsafely-treat-insecure-origin-as-secure
```

Add exactly:

```text
http://172.29.203.1
```

The board-hosted setup dialog surfaces both addresses as independent copy
buttons. Copy success confirms the text was copied to the clipboard; it does not
confirm that the serial connection works. The browser device chooser still
appears when the **Web Serial** button beside **Bridge** is clicked and must be
accepted manually.

Because the board page is served over HTTP (not HTTPS), the Clipboard API may
not be available in all browser contexts. The copy buttons therefore try an
HTTP-compatible copy fallback; if copying still fails, the full address remains
visible for manual selection. Chooser acceptance and user gesture requirements
are never removed by the override.

If you do not enable the override, keep the board page open and use the host-side
bridge instead:

```sh
npm run build
npm run host
```

Then use the **Bridge** button in either serial terminal. The bridge prefers the
CH347F `D1` device for UART0 and `D3` for UART1, with sorted device order as the
fallback when those suffixes are unavailable. Bridge mode uses the versioned
`linkr-serial-broker.v1` protocol: one host-owned connection per UART is shared
by Web, automation, and MCP clients; receive data is broadcast, writes
are ordered and acknowledged, and an automation client can claim exclusive
write access. The old unversioned/raw-text WebSocket protocol is intentionally
unsupported. See [the Serial Broker protocol](../doc/serial-broker.md).

## Local MCP server

Agents can use the same gateway and Serial Broker through a local stdio MCP
server:

```sh
npm run build
npm run --silent mcp
```

The npm command invokes the Rust `linkr-host mcp` adapter. It exposes bounded
status, ADC, confirmed power/route controls, and cursor-based UART tools without
opening a second CH347F handle. MCP automatically starts the loopback Host when
needed; tool listing does not touch the board. Configuration, tool contracts
and safety exclusions are documented in
[the local MCP server guide](../doc/mcp-server.md).

The gateway exposes a host-only `/healthz` readiness endpoint, so MCP does not
issue an extra firmware status request before every operation. Board status is
compact by default; dedicated serial status and login tools avoid repeated
full-log reads and hand-written prompt sequences.

## Startup power analysis

The infrequent startup workflow lives under **Advanced & recovery** rather than
on the primary dashboard. It requires the selected UART0 or UART1 connection and
an idle power-capture session. After one explicit confirmation it:

1. clears and records the target serial console;
2. turns the selected rail off and waits for the configured discharge delay;
3. arms a firmware `power_on` capture before restoring the rail;
4. timestamps the first post-power UART byte, U-Boot or UEFI, kernel, and login
   markers from returned serial data;
5. reports peak current, average power, integrated energy, and the latest two
   trigger-aligned power curves for the selected rail, retained independently
   per rail.

The boot firmware selector defaults to automatic detection and can be pinned to
U-Boot or UEFI for boards such as Radxa O6N and Q6A. The task can be cancelled
before power-on, in which case the UI attempts to restore the selected rail.
Serial logs can be downloaded after completion. Downloads preserve the raw
post-power text stream without inserting host timestamps into target lines.

The power curve uses the firmware monotonic sample clock; serial milestones use
the browser monotonic clock and are therefore intended as host-observed timings.
Bytes drained while the rail is off are counted as stale input and excluded from
both matching and export. A timeout without Login, a disconnected console, or a
silent post-power UART produces a partial result instead of a successful one.
The displayed energy covers only the firmware capture window shown in the UI,
not serial activity beyond that window.

## Web OTA

The OTA card lives in **Advanced & recovery** alongside the startup power
tools. It delivers RP2350 firmware updates over the same USB NCM
HTTP API used by the rest of the UI, with no separate host tooling required.

**Accepted artifact**: MCUboot-format application `.bin` only. The release
payload `radxa-linkr-debugger-rp2350-ota.bin` is copied from the sysbuild
application output `zephyr.signed.bin`; despite the filename, this project
config uses unsigned MCUboot format. Never upload `.uf2` or `.elf` via OTA.

**Local SHA-256**: the browser computes SHA-256 locally using the Web Crypto
API (with a pure-JavaScript fallback) and sends the hex digest as the
`X-Linkr-Ota-Sha256` header alongside the raw binary. SHA-256 verifies
integrity only, not authenticity or signing. The firmware recalculates the hash
on the device and rejects mismatches.

**Upload / test / reconnect workflow**: the card accepts a `.bin` file,
computes SHA-256 locally, uploads it via `POST /api/v1/ota/upload`, then lets
you trigger `POST /api/v1/ota/test` to reboot into the test image. During the
test boot the browser polls for the board to return; brief polling failures are
expected and the UI shows a reconnecting state. After a successful test boot you
can manually confirm with `POST /api/v1/ota/confirm`, or you can wait for the
firmware to auto-confirm.

**Auto-confirm gate**: the browser never calls confirm automatically. Firmware
owns the ~16-second watchdog health-gated auto-confirm. If the test image runs
healthily for ~16 seconds without a watchdog reset, firmware marks the image as
confirmed. Firmware is designed to request MCUboot rollback after a watchdog
reset during the unconfirmed window, but fault-injection HIL for that recovery
path is still blocked; keep ROM BOOTSEL recovery available.

**Same-origin vs GitHub Pages**: when the UI is served from the board at
`http://172.29.203.1/` it talks to the OTA endpoints directly (same-origin).
When the UI is served from GitHub Pages over HTTPS, Linkr Host
(`npm run host`) is required to reach the board at
`http://172.29.203.1`. The gateway now permits the OTA-specific headers
(`X-Linkr-Ota-Size`, `X-Linkr-Ota-Sha256`) in CORS responses, so upload
progress and SHA-256 verification work end-to-end from Pages. Start the bridge
before opening the OTA card from the hosted page.

### Browser HIL runner

The browser-based OTA HIL runner at `scripts/ota-hil.mjs` automates the OTA
card through a real Chromium/Chromium-browser instance via Playwright. It is
HIL tooling, not a production OTA delivery mechanism.

The runner defaults to dry-run mode. It prints a JSON plan describing the
browser actions without touching the board:

```sh
node scripts/ota-hil.mjs --dry-run
```

To execute against real hardware:

```sh
node scripts/ota-hil.mjs --execute --flow both
node scripts/ota-hil.mjs --execute --flow auto
node scripts/ota-hil.mjs --execute --flow manual
```

The runner accepts `--playwright-module PATH_OR_NAME` and
`--chromium-executable PATH` to control Playwright loading and the browser
binary. It does not require a global Playwright installation; playwright-core is
loaded dynamically using Node's module resolution. For Nix users, a temporary
nix environment or explicit Nix store paths can supply the Chromium dependency
without claiming an exact unverified package attribute.

The runner uses bounded timeouts (5-second short operations, 45-second reboot
wait, 120-second upload timeout) and polls for OTA state with diagnostics
truncation. It validates the `.bin` extension and non-empty file before upload.
Watchdog rollback is BLOCKED because no safe fault-injection path exists; the
result object reports this explicitly rather than attempting the operation.
