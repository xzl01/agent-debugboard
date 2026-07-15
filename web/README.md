# Radxa Linkr Debugger Web UI

React/Vite control interface for the debugger firmware HTTP and WebSocket APIs.
The UI is hosted locally and proxies `/api` to the board at
`http://172.29.203.1:8080`; web assets are not embedded in firmware.

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
device timestamps and the complete trigger configuration.

The serial card shows the current CH347F UART VIO level in its upper-right
corner. Changing between 3.3V and 1.8V requires an explicit risk confirmation;
an active serial connection is closed before the firmware VIN route is changed.
The console uses an xterm-compatible terminal surface: click the terminal and
type directly, without a separate command input. Terminal writes are serialized
so fast typing cannot contend for the Web Serial writer lock. The terminal only
renders data returned by the target; successful host writes are reflected by the
TX counter. Enter sends CRLF by default, matching the previous line-input
console; CR and LF modes are also available from the terminal toolbar.

## Target serial console

Chromium browsers can use Web Serial directly. If the OS driver owns the
CH347F port, start the local fallback bridge in a second terminal:

```sh
npm run device-bridge
```

Then use the **Bridge** button in the serial console card.

## Startup power analysis

The infrequent startup workflow lives under **Advanced & recovery** rather than
on the primary dashboard. It requires an active target serial connection and an
idle power-capture session. After one explicit confirmation it:

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
