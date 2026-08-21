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

The board uses DHCP option 114 and HTTP detection endpoints to trigger the OS
captive portal prompt without becoming the host's Internet gateway:

- **DHCP**: the DHCPv4 server assigns a local NCM address and subnet route, but
  deliberately does not advertise a default router or DNS server. It sends
  DHCP option 114 (Captive Portal URI) with the value
  `http://172.29.203.1/captive-portal/api`. This prevents the debugger from
  taking priority over Ethernet, Wi-Fi, or VPN Internet access.
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
- **Switch routes** — TF/SD routing between `target` and `usb-reader`; the J12
  lower-port USB device can be switched between the PC (`pc`) and the target
  connected to J12's upper port (`target`).
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
- **Automated testing** — build command sequences, select consecutive commands
  into a visible loop frame, set 1-1000 rounds, and follow each loop iteration
  independently in the run view and exported report.

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

### Linux serial device permissions

Direct Web Serial and the device bridge both open serial devices as the current
desktop user. On Linux, CH347 ports normally appear as `/dev/ttyUSB0` and
`/dev/ttyUSB1`; the board CDC ACM fallback may appear as `/dev/ttyACM0`.

If the browser reports that it failed to open the serial port, or the bridge
reports `EACCES`, `EPERM`, `Permission denied`, or `Access denied`, inspect the
device ownership:

```sh
ls -l /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
id -nG
```

On Debian and Ubuntu, serial devices are normally assigned to the `dialout`
group. Add the current account to that group:

```sh
sudo usermod -aG dialout "$(id -un)"
```

Then sign out and back in, or reboot. Reconnect the board, restart the browser
or `npm run device-bridge`, and try again. The new group membership does not
apply to browser or terminal processes that were already running.

If the device is owned by a different serial-access group, use the group shown
by `ls -l` and follow that distribution's device-access policy. If access is
already correct, check whether another serial monitor or service owns the port:

```sh
fuser /dev/ttyACM0
```

Replace the path with the device that failed to open. Avoid running the browser
or bridge as root, and do not rely on `chmod` as a permanent fix: device
permissions are recreated when USB reconnects.

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

The Pages build connects to `http://127.0.0.1:8787/api/v1`. By default, the
gateway forwards HTTP and WebSocket traffic directly to the firmware service
at `http://172.29.203.1` and supplies the CORS and Private Network Access
headers needed by the browser. Override the upstream address when necessary:

```sh
LINKR_BOARD_URL=http://172.29.203.1 npm run device-bridge
```

On macOS, both `npm run dev` and `npm run device-bridge` automatically insert a
loopback-only raw TCP forwarder when targeting the default USB-NCM address.
This avoids repeated Vite `502 Bad Gateway` responses when macOS permits native
tools such as `curl` to use the interface but rejects direct Node.js sockets.
The forwarder carries REST, OTA, and WebSocket traffic and exits with its parent
process; no manual system proxy is required.

## Related

- [OTA firmware update](ota.md)
- [OpenOCD / JTAG](openocd.md)
