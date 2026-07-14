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

GitHub Pages serves the UI over HTTPS, while the board currently exposes its
REST and WebSocket APIs over HTTP on the USB-NCM network. Browsers can therefore
load and preview the hosted UI, but they may block direct hardware API access
because of mixed-content and CORS rules. Use the local Vite proxy for full
HTTP/WebSocket control until an HTTPS-capable local bridge or Web Serial control
transport is available.

## Target serial console

Chromium browsers can use Web Serial directly. If the OS driver owns the
CH347F port, start the local fallback bridge in a second terminal:

```sh
npm run serial-bridge
```

Then use the **Bridge** button in the serial console card.
