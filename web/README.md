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

## Target serial console

Chromium browsers can use Web Serial directly. If the OS driver owns the
CH347F port, start the local fallback bridge in a second terminal:

```sh
npm run device-bridge
```

Then use the **Bridge** button in the serial console card.
