# Radxa Linkr Debugger Web UI

React/Vite control interface for the debugger firmware HTTP and WebSocket APIs.
Production firmware embeds the gzip-compressed UI and serves it from
`http://172.29.203.1:8080/` over USB NCM. The page uses same-origin `/api/v1`
HTTP and WebSocket endpoints, so normal board controls do not require a host
proxy.

## Development

```sh
cd web
npm ci
npm run dev
```

Open <http://127.0.0.1:5173/>. The board must be connected over USB NCM and
reachable at `172.29.203.1`.

The Power & current card includes a triggered power analyzer. It supports
manual, current-threshold, GPIO-edge, and power-on triggers, keeps four captures
for overlay comparison, and exports CSV or NDJSON. Captures use firmware device
timestamps and a pre/post-trigger ring buffer instead of browser timing.

## Production build

```sh
npm run build
```

Generated files are written to `web/dist/` and are intentionally ignored by
Git.

The canonical firmware build runs `npm ci` and `npm run build:firmware`
automatically, verifies the fixed embedded asset set, and converts the three
production files into gzip-compressed Zephyr HTTP resources. Node.js 22 and npm
must therefore be available when building firmware from a clean checkout.
Run `npm run build:firmware` directly to reproduce that fixed-asset build from
the Web workspace; unlike the normal build, it disables JavaScript code splitting.

After flashing, connect the USB NCM interface and open:

```text
http://172.29.203.1:8080/
```

## GitHub Pages

Pushes to `dev` deploy the production build to
<https://xzl01.github.io/agent-debugboard/>. The Pages build sets the Vite base
path automatically; local development continues to use `/`.

GitHub Pages serves the UI over HTTPS, while the board exposes its REST and
WebSocket APIs over HTTP on the USB-NCM network. Start the loopback device
gateway before using hardware controls from the hosted page:

```sh
cd web
npm ci
npm run device-bridge
```

The Pages build connects to `http://127.0.0.1:8787/api/v1`. The gateway forwards
HTTP and WebSocket traffic to `http://172.29.203.1:8080` and supplies the CORS
and Private Network Access headers needed by the browser. It listens on loopback
only and does not expose board controls to the LAN. Browser requests are limited
to the official Pages origin and local development origins. Additional trusted
origins can be supplied as a comma-separated `LINKR_TRUSTED_ORIGINS` value.

The power analyzer arms firmware-side manual, current-threshold, power-on, or
GPIO-edge captures. It overlays the latest four runs and exports CSV/NDJSON with
device timestamps and the complete trigger configuration. The latest capture
also reports duration, mAh, and Wh for the selected rail using trapezoidal
integration over the device monotonic timestamps.

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

The board-hosted page runs at `http://172.29.203.1:8080/`, which is plain HTTP
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
http://172.29.203.1:8080
```

The board-hosted setup dialog surfaces both addresses as independent copy
buttons. Copy success confirms the text was copied to the clipboard; it does not
confirm that the serial connection works. The browser device chooser still
appears when the **Web Serial** button is clicked and must be accepted manually.

Because the board page is served over HTTP (not HTTPS), the Clipboard API may
not be available in all browser contexts. The copy buttons therefore try an
HTTP-compatible copy fallback; if copying still fails, the full address remains
visible for manual selection. Chooser acceptance and user gesture requirements
are never removed by the override.

If you do not enable the override, keep the board page open and use the host-side
bridge instead:

```sh
npm run device-bridge
```

Then use the **Bridge** button in either serial terminal. The bridge prefers the
CH347F `D1` device for UART0 and `D3` for UART1, with sorted device order as the
fallback when those suffixes are unavailable.

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
   trigger-aligned power curves.

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
