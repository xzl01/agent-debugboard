[中文](webui.zh-CN.md)

# Web UI

The board serves a React/Vite dashboard over USB NCM at the board `.1` address of the host NCM `/30` (for example `http://10.20.30.1/`). No proxy or extra software needed — the page talks directly to the board's `/api/v1` endpoints.

## Access

Plug in the USB cable, read the board URL, and open it:

```sh
ip -brief addr show
# host NCM is .2 of the board /30; the board is .1
# or, on the board CDC shell:
ip
```

On most operating systems, the board's captive-portal detection will prompt the
browser to open this page automatically. If it doesn't, navigate to the printed
URL manually.

## How auto-open works

The board uses DHCP option 114 and HTTP detection endpoints to trigger the OS
captive portal prompt without becoming the host's Internet gateway:

- **DHCP**: the DHCPv4 server assigns `network + 2` from the board's persisted
  private `/30`, but deliberately does not advertise a default router or DNS
  server. It sends DHCP option 114 (Captive Portal URI) built at runtime from
  the current server address with the `/generate_204` path. This prevents the
  debugger from taking priority over Ethernet, Wi-Fi, or VPN Internet access.
- **HTTP port 80**: a single Zephyr HTTP service listens on port 80; the NCM host reaches it at the board `.1` address. When a
  GET request arrives at `/captive-portal/api` or `/generate_204`, it responds
  with HTTP 200 and the capport JSON body using the current board URL. Other
  unmatched GET paths return HTTP 302 with `Location` set to the board URL.
- **Auto-open is best-effort, not guaranteed.** Results vary by operating system
  version, network configuration, and whether the host has other active network
  interfaces. Users who need the Web UI can always open
  the printed URL directly in a browser, use `curl`, or rely on the CLI/TUI.

## Dashboard Overview

The main dashboard provides cards for:

- **Power controls** — turn `12v_out`, `5v_out`, `20v_out`, `vdd_5v`,
  `5v_usb_hub`, and `5v_otg` on/off. The hub and OTG port are children of
  `vdd_5v`: enabling a child also enables the parent; enabling the parent alone
  does not enable the children.
- **ADC monitoring** — live current readings for the four INA226 rails; the hub
  and OTG branches have no separate measurement. ADC0 is shown as the PD input's measured
  DC20V source voltage, while ADC1-3 are native pad-voltage cards.
- **Switch routes** — TF/SD routing between `target` and `usb-reader`, plus
  `tf_wp` write protection. x1.2 has no USB route control.
- **GPIO** — safe controls `GP5`-`GP9` and `GP12`-`GP16`, plus input-only
  `GP29`/ADC3. GP5/GP6 are ordinary J8 GPIOs, not switches.

The Power & Measurements card merges ADC0 into the PD settings panel:
the displayed scalar is the firmware's Device-Tree-reconstructed DC20V source
value, with `raw`/`mv` available as pad diagnostics. ADC1, ADC2, and ADC3 render
as separate native-voltage cards above the switch routes, using a 0..3.3 V
scale. The Web UI never applies `115/15` or a host-side calibration table.
ADC0 is uncalibrated to approximately 2%; the UI does not claim higher
precision.

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
- **J4 serial terminal** — one logical target console rendered by xterm.js. The
  page creates a firmware live session and subscribes to `serial.j4`; no browser
  device chooser, desktop serial broker, or local tty access is involved. The
  terminal auto-connects on entry. A manual disconnect is sticky for that page
  session and suppresses automatic retries, avoiding reconnect storms. Reconnect
  explicitly when desired.

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

## Workflow Library, Tasks, And Import/Export

### Workflow library

The **Automation** workspace keeps multiple workflows in the browser's
IndexedDB library. One workflow may deploy to several boards; deployments are
keyed by board store UUID and task ID. **Save to library** updates the active
workflow, **Save as new workflow** creates an independent entry, **Open** loads
a saved workflow, and the delete action removes only that library entry. The
unsaved editor draft is separate from the library and survives a reload through
local draft storage; a changed draft prompts before opening another workflow.

The library is browser-local. Export a workflow to keep a portable copy.
Workflow export/import uses canonical `linkr-test.v1` source and preserves
loops, Units, and assertions. Workflow import previews and validates the file
or pasted source before replacing the editor draft; it never runs the board.
Task IR import/export is a separate action under the **Task** card. Task IR
contains deployment requests, not an editable workflow.

### Built-in and stored tasks

MASKROM and EDL are six immutable firmware catalog entries at
`GET /api/v1/tasks/catalog`, one per mode and `5v_out`/`12v_out`/`20v_out`.
They are not stored in flash and do not consume the four task-v2 slots. The
**Task** card lists them together with task-v2 metadata from
`GET /api/v1/tasks`. Catalog and metadata fetches are independent: a catalog
failure hides built-ins behind an error while stored tasks remain usable.

The task-v2 store has four slots, separate UUID/revision, and exact
`linkr-test.v1` source plus `linkr-task.v1` IR (each up to 4000 bytes on the
board). The card lets you:

- save the current workflow as a board task; the board receives exact source
  only when it is at most 4000 bytes;
- open a stored task's workflow source. A board-backed source (`present`) can
  be reopened from a fresh browser profile after reboot. A source over 4000
  bytes stays in the local workflow library only (`local-only`), is clearly
  marked, and deploys only IR; a fresh browser cannot reopen it unless it was
  exported/imported. An IR-only task or built-in has no lossless source and
  shows that limitation instead of a false Open action.
- run a stored task from the exact `/api/v1/tasks/ir` bytes and dispatch each
  request through the ordinary power, GPIO, and switch endpoints. `wait_ms`
  is client-side; the first failure stops the run.
- import one task IR without changing siblings, or explicitly replace all
  tasks with full-set IR. Import never runs hardware. Importing full IR keeps
  a source only for an unchanged task-IR digest.
- export one task IR or the full IR set. Exporting a built-in copies only its
  main requests; it is not a lossless copy of catalog cleanup.
- delete one stored task or clear all stored tasks atomically. Clear keeps the
  store UUID and never changes live hardware.

If a stored task uses a reserved built-in ID, the built-in row marks the stored
entry as shadowed and running that ID selects the built-in. The old
Task writes use only the JSON/base64 mutation endpoint; there is no blob upload
or per-ID write/delete route.

The firmware does not auto-run, replay, or seed tasks at boot. A stored task
remains inert until explicitly run. The old `linkr/task/tasks` blob is not
migrated or read.

## Firmware Tools And Power Analysis

- **Firmware Tools** in the dashboard sidebar contains the OTA, normal
  restart, and ROM BOOTSEL controls. See [ota.md](ota.md) for the full OTA
  workflow.
- **Power Analysis** is a workspace tab with ordinary captures and startup
  analysis.
- **Automation** is a workspace tab with the workflow library, test editor,
  and the task-v2 Task card that lists, imports, exports, runs, deletes, and
  clears board tasks.

### Reboot And BOOTSEL

**Restart** (normal reboot) requests a normal cold restart through
`POST /api/v1/reboot`. The board restarts 250 ms after the response and the
page reconnects; this action never enters ROM BOOTSEL and never produces an
`RPI-RP2` device. Holding the physical BOOTSEL button across that reset is a
separate ROM behavior. Any rail not restored by a saved v1 configuration
snapshot returns to its inactive boot default; `vdd_5v` is the parent power item
and `5v_usb_hub`/`5v_otg` are its children and also boot off. Re-apply rail and PD setup
after the board returns.

**Enter BOOTSEL** is a separate action through `POST /api/v1/bootloader`; it is
not a restart alias. It is the only Web UI action that should make the board
enumerate as `RPI-RP2`. Flash only the combined
`radxa-linkr-debugger-rp2350.uf2` (MCUboot plus application); the
application-only `zephyr.uf2` does not include MCUboot and must never be used
for ROM BOOTSEL.

The physical BOOTSEL button is not a third Web UI action. While firmware is
running, hold it for 3 seconds and release to request a normal restart; a short
press does nothing, and holding it without releasing does not reset. Holding it
during power-on or reset is sampled by the ROM, which enters ROM BOOTSEL. The
button is serviced by a firmware thread and cannot recover a completely wedged
core; the hardware watchdog remains the fallback for that failure mode.

## Connecting Target Serial

Open the board-hosted page and select **Web Terminal**. The xterm.js J4 terminal
uses the same USB-NCM address as the rest of the dashboard:

1. The page creates one `/api/v1/live-sessions` slot.
2. It connects to the returned `/api/v1/ws/<slot>` URL.
3. It subscribes to `topic: "serial.j4"` and displays raw binary downlink.

Each browser binary frame is one atomic J4 write and is limited to 480 bytes.
The page distinguishes `serial_write_accepted` from the later
`serial_write_complete`; an accepted write is not yet proof of UART wire
completion. `serial_state` reports line coding, generation, DTR/RTS, USB state,
queue state, pause/fault transitions, and the automatic terminal responder.
Explicit input remains multiwriter. Only one client answers automatic terminal
queries: CDC while DTR is asserted, otherwise the lowest-numbered attached
WebSocket. Other pages show **Auto-reply observer** and **Manual input allowed**
and still send explicit keyboard, IME, paste, and Ctrl-C input; they are not
read-only, and incoming escape sequences are never filtered. Overrun messages
carry both the new drop delta and the cumulative total for this browser sink.
A responder change preserves the same xterm modes: pre-promotion output is
drained behind a parser fence before automatic replies are enabled, and a
generation change rebuilds the renderer before subsequent bytes. A passive
page's stale partial query is not answered later.

UART settings accept a custom numeric baud from 4800 through 115200 and also
offer five recommended presets. Rates above 115200 are rejected; earlier
high-rate samples are diagnostic only. After Apply, the page treats
`GET /api/v1/serial/j4` current settings and generation as authority; this is
also required after CDC SET_LINE_CODING because USB cannot return a later
application-level rejection. Low-baud write deadlines are derived from the
bounded eight-slot queue plus a 2-second margin.

The `dtr` shown by the page is the published CDC/gate state, not an internal
promotion phase. While DTR is false, a USB default line coding is staged only
and does not overwrite the active tuple or generation; an HTTP/WS Apply in that
low state takes effect immediately. When the terminal asserts DTR, the board
first applies any staged USB settings and only then publishes `dtr=true` and
opens ingress. A failed apply/attach/detach leaves the CDC path blocked until a
successful LOW/reset. The API does not expose a phase or block-reason field.

The terminal remains connected while J4 is temporarily paused for a logic
analyzer capture on GP10/GP11. Input is rejected explicitly during the pause;
after capture cleanup restores UART1 and resumes the runtime, the same terminal
continues without a page reload. Manual disconnect remains sticky and does not
start a retry loop.

For non-browser access, connect to raw TCP `0.0.0.0:5555` using the board's
current NCM address, or use the board's J4 CDC ACM interface. Raw TCP is binary,
has at most two clients, and is not Telnet, SSH, or noVNC. USB tty numbering is
host-dependent; identify the J4 interface instead of assuming a device number.

## Startup Power Analysis

The startup workflow lives in the **Power Analysis** workspace tab. It requires
the J4 terminal connection and an idle power-capture session. After one
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

## Development previews

The supported hardware UI is the production bundle embedded in firmware and
served at the board's port 80. The Vite server on port 5173 is a development
preview, not a device transport or serial broker. GitHub Pages may be used to
review static presentation, but current J4 product validation is performed
against the embedded page and its direct firmware HTTP/WebSocket paths.

## Related

- [OTA firmware update](ota.md)
- [OpenOCD / JTAG](openocd.md)
