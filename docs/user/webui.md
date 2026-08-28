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

Within the **Configuration > Saved** tab, the **Saved configuration** card is
always visible. Its saved rows and actions appear below the header; power and
switch groups start open while the GPIO group starts closed. Group disclosure
only changes local presentation and never saves, clears, or refreshes anything.
Selection and in-progress feedback remain intact when a group is opened or closed.

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

## Workspace Tabs

The right workspace offers parallel tabs for **Web Terminal**, **Power
Analysis**, **Logic Analyzer**, **Automation**, and **Configuration**. GPIO
controls live in **Hardware controls → I/O**, which keeps one authoritative
hardware control surface. The pin disc color itself
reflects every latest live value: black is LOW and red is HIGH, including
while a pin is an input. A dashed outer ring marks an input and a solid ring
marks an output; the smaller level disc stays inside that direction boundary.
Pins are driven by direct gestures with no selection step: a short tap
requests output LOW once the 220 ms double-tap window expires, a second tap
inside that window requests input acquisition instead, and holding a pin for
600 ms requests output HIGH while a danger-colored progress sweep uses that same
dashed/solid direction ring. Moving more than 8 px, losing pointer capture, a `pointercancel`,
or pressing Escape cancels a gesture without writing anything. Keyboard
operation is immediate on a focused pin: Enter/Space/0 drive LOW, 1 drives
HIGH, and I requests input; key auto-repeat is ignored. The card allows one
in-flight request at a time and marks the pending pin with a dimmed level
disc plus a solid warn-colored busy ring. Mounting or refreshing
the pinout never writes GPIO state, the UI is non-optimistic, and the latest
firmware snapshot remains the displayed authority while a request is in
flight. The
MASKROM/EDL recovery line `CON_MAS` (`GP7`) is a regular allowlisted output, so
it can be driven through these gestures. `GP29` stays input-only while it is
owned by the `adc3` voltage monitor; unsupported output attempts fail at the
firmware layer and the card displays that error.

The Hardware controls drawer remembers its last **Power** or **I/O** section in
browser storage. Clicking the backdrop outside the drawer closes it; clicks
inside the drawer do not.

## Built-In And Saved Tasks

Inside the **Automation** workspace, MASKROM and EDL appear as six ordinary
firmware-owned built-in task rows, fetched from `GET /api/v1/tasks/catalog`
and covering every combination of mode and `5v_out`, `12v_out`, or `20v_out`.
The Web UI keeps no recovery recipes of its own: rails, GPIO levels, and wait
timings all come from the validated firmware catalog. Built-ins use the same
generic task runner as stored tasks and dispatch only ordinary power and GPIO
requests. They are not stored in firmware flash and do not consume any of the
four stored-task slots. The **Task** card lets you:

- List the catalog built-ins together with tasks returned by `GET /api/v1/tasks`.
  The two fetches are independent: if the catalog is unavailable or fails
  validation, built-in rows disappear behind a visible error while stored
  tasks stay usable.
- Run a built-in directly without reading or mutating `/api/v1/tasks`.
- Save the current editor workflow after it is expanded and verified to
  contain only firmware-executable primitive steps.
- Run a stored task: the page downloads the current `linkr-task.v1` blob,
  selects the task by id, and dispatches each record through the ordinary
  power, GPIO, and switch endpoints in order. `wait_ms` is applied
  client-side after every successful request; the first failure stops the
  run and surfaces the failed record. The wire `body` sent to the board
  never carries `wait_ms`.
- Clear all stored tasks. Live hardware is unchanged.

If a stored task uses a reserved built-in ID, the matching built-in row marks
the stored entry as shadowed and running that ID selects the built-in. Clearing
stored tasks removes the collision but leaves all catalog built-ins visible.

The firmware does not auto-run, replay, or pre-stage saved tasks at boot.
A stored task that still lives in flash after a reboot is inert until the
**Task card** runs it explicitly. Old development-stage task data is
intentionally invalidated by the new marker `# linkr-task.v1` and the new
key `linkr/task/tasks`; there is no migration or read alias for it.

## Firmware Tools And Power Analysis

- **Firmware Tools** in the dashboard sidebar contains the OTA and BOOTSEL
  controls. See [ota.md](ota.md) for the full OTA workflow.
- **Power Analysis** is a workspace tab with ordinary captures and startup
  analysis.
- **Automation** is a workspace tab with the test editor and the
  flash-backed Task card that lists, stores, runs, and clears saved
  request sequences.

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
 npm run host
```

Then use the **Bridge** button in either serial terminal. The bridge prefers the
CH347F `D1` device for UART0 and `D3` for UART1, with sorted device order as
the fallback when those suffixes are unavailable.

### Linux serial device permissions

Direct Web Serial and the Linkr Host bridge both open serial devices as the current
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
or `npm run host`, and try again. The new group membership does not
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

The startup workflow lives in the **Power Analysis** workspace tab. It requires the
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
Linkr Host before using hardware controls from the hosted page:

```sh
cd web
npm ci
npm run host
```

The Pages build connects to `http://127.0.0.1:8787/api/v1`. By default, the
gateway forwards HTTP and WebSocket traffic directly to the firmware service
at `http://172.29.203.1` and supplies the CORS and Private Network Access
headers needed by the browser. Override the upstream address when necessary:

```sh
LINKR_BOARD_URL=http://172.29.203.1 npm run host
```

On macOS, `npm run dev` automatically inserts a
loopback-only raw TCP forwarder when targeting the default USB-NCM address.
This avoids repeated Vite `502 Bad Gateway` responses when macOS permits native
tools such as `curl` to use the interface but rejects direct Node.js sockets.
Linkr Host talks directly to the USB-NCM address and does not need the legacy
Node/Ruby forwarder.

## Related

- [OTA firmware update](ota.md)
- [OpenOCD / JTAG](openocd.md)
