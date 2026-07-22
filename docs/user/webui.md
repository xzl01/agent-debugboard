[中文](webui.zh-CN.md)

# Web UI

The board serves a React/Vite dashboard at `http://172.29.203.1/` over USB NCM. No proxy or extra software needed — the page talks directly to the board's `/api/v1` endpoints.

## Access

Plug in the USB cable and open:

```text
http://172.29.203.1/
```

On most operating systems, the board's captive portal detection will prompt the browser to open this page automatically. If it doesn't, navigate to the URL manually.

## How auto-open works

The board uses three mechanisms to trigger the OS captive portal prompt:

- **DHCP**: the DHCPv4 server on the NCM interface advertises the router and
  DNS address as `172.29.203.1` and sends DHCP option 114 (Captive Portal URI)
  with the value `http://172.29.203.1/captive-portal/api`. This is a
  compatibility-oriented HTTP endpoint, not a trusted HTTPS signal.
- **DNS**: a lightweight DNS responder bound to the NCM interface on UDP port 53
  returns a wildcard A record pointing to `172.29.203.1` for any incoming query.
  AAAA queries return a NOERROR response with zero answers (NODATA). DNS TTL is
  30 seconds.
- **HTTP port 80**: a single Zephyr HTTP service on `172.29.203.1:80` routes by
  URL path. When a GET request arrives at `/captive-portal/api`, it responds
  with HTTP 200 and a JSON body:
  `{"captive":true,"user-portal-url":"http://172.29.203.1/","venue-info-url":"http://172.29.203.1/"}`.
  Other unmatched GET paths return HTTP 302 with `Location: http://172.29.203.1/`.
- **Auto-open is best-effort, not guaranteed.** Results vary by operating system
  version, network configuration, and whether the host has other active network
  interfaces. Users who need the Web UI can always open
  `http://172.29.203.1/` directly in a browser, use `curl`, or rely on the
  CLI/TUI.

## Dashboard Overview

The main dashboard provides cards for:

- **Power controls** — turn `12v_out`, `5v_out`, `20v_out` on/off.
- **ADC monitoring** — live current readings for each power rail.
- **Switch routes** — TF/SD routing between `target` and `usb-reader`; USB hub
  mux between `pc` and `target`.
- **GPIO** — read/write safe GPIOs (`GP7`-`GP20`, `GP29`).

The Power & current card includes a triggered power analyzer. It supports
manual, current-threshold, GPIO-edge, and power-on triggers, keeps four captures
for overlay comparison, and exports CSV or NDJSON. Captures use firmware device
timestamps and a pre/post-trigger ring buffer.

## Terminal Workspace

The Terminal workspace lives alongside the primary dashboard and contains:

- **Logic analyzer** — RP2350 PIO2+DMA high-speed single-shot capture. Supports
  1-16 channels from the safe pin allowlist (GP7-GP9, GP10-GP20, GP29), trigger
  modes `none`, `rising`, `falling`, `either`, requested sample rates 1-125 MHz,
  and up to 512 exported samples. Completed captures can be previewed in the
  waveform view and exported as CSV or PulseView `.sr` files. A continuous
  streaming mode (1-25 MHz) is also available. The browser-based Rust/WASM
  decoder supports UART, I2C, and SPI protocol decoding.
- **Serial terminal** — independent UART0 and UART1 sessions via CH347F. Tab
  mode switches the visible terminal without disconnecting either channel; split
  mode shows both. Uses an xterm-compatible terminal surface: click and type
  directly. Enter sends CRLF by default; CR and LF modes are available from the
  toolbar.

## Advanced & Recovery

- **OTA card** — delivers RP2350 firmware updates over the same USB NCM HTTP
  API. See [ota.md](ota.md) for the full OTA workflow.
- **Startup power analysis** — see the dedicated section below.

## Connecting Target Serial

### Web Serial (secure origins)

Edge and Chrome/Chromium can use Web Serial directly from secure origins such as
the local Vite dev server or GitHub Pages. Users must click the **Web Serial**
button and accept the browser device chooser.

### Board-hosted page

The board-hosted page runs at `http://172.29.203.1/`, which is plain HTTP and
therefore not a secure context by default. To enable direct CH347 Web Serial
from the board page:

1. Copy the Chromium flag-page address below and paste it into the address bar
   manually (ordinary web pages cannot navigate to `chrome://` URLs):

   ```text
   chrome://flags/#unsafely-treat-insecure-origin-as-secure
   ```

2. Add the exact origin:

   ```text
   http://172.29.203.1
   ```

3. Enable the flag and relaunch the browser.

Edge also accepts this Chromium address.

### Device-bridge fallback

If you do not enable the override, keep the board page open and use the
host-side bridge instead:

```sh
cd web
npm ci
npm run device-bridge
```

Then use the **Bridge** button in either serial terminal. The bridge prefers the
CH347F `D1` device for UART0 and `D3` for UART1, with sorted device order as
the fallback when those suffixes are unavailable.

## Startup Power Analysis

The startup workflow lives under **Advanced & recovery**. It requires the
selected UART0 or UART1 connection and an idle power-capture session. After one
explicit confirmation it:

1. Clears and records the target serial console.
2. Turns the selected rail off and waits for the configured discharge delay.
3. Arms a firmware `power_on` capture before restoring the rail.
4. Timestamps the first post-power UART byte, U-Boot or UEFI, kernel, and
   login markers from returned serial data.
5. Reports peak current, average power, integrated energy, and the latest two
   trigger-aligned power curves.

The boot firmware selector defaults to automatic detection and can be pinned to
U-Boot or UEFI. The task can be cancelled before power-on. Serial logs can be
downloaded after completion.

## GitHub Pages Deployment

Pushes to `dev` deploy the production build to
<https://xzl01.github.io/agent-debugboard/>. GitHub Pages serves the UI over
HTTPS, while the board exposes its APIs over HTTP on the USB-NCM network. Start
the device-bridge gateway before using hardware controls from the hosted page:

```sh
cd web
npm ci
npm run device-bridge
```

The Pages build connects to `http://127.0.0.1:8787/api/v1`. The gateway
forwards HTTP and WebSocket traffic to `http://172.29.203.1` and supplies the
CORS and Private Network Access headers needed by the browser.

## Related

- [OTA firmware update](ota.md)
- [OpenOCD / JTAG](openocd.md)
