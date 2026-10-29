[中文](troubleshooting.zh-CN.md)

# Troubleshooting

Common problems and how to fix them.

## CLI can't connect to the board

**Symptom:** `doctor` fails or `status` returns a connection error.

1. Check the USB cable. Data cables only — charge-only cables won't work.
2. Verify the board shows up as a network interface:
   - Linux: `ip link show` — look for a new `enp*` or `usb*` interface
   - macOS: `ifconfig` — look for a new `en*` interface
   - Windows: check Network Connections for a new RNDIS/NCM adapter
3. Read the host NCM address with `ip -brief addr show`; the board is the same `/30` host address minus one
4. Ping the printed board IP. If ping works but the CLI fails, try specifying
   the URL explicitly:
   ```sh
   radxa-linkr-debuggerctl --url "http://<board-ip>" doctor
   ```
5. If nothing works, try a different USB port or cable.

## Web UI doesn't open

**Symptom:** Browser shows "can't reach this site" at the board `.1` address of the host NCM `/30`.

1. Verify CLI connectivity first: `radxa-linkr-debuggerctl doctor`
2. If doctor passes but the browser can't connect, check if another network
   interface has priority. The board uses a link-local-style address; some
   OS configurations prefer other routes.
3. Try opening in an incognito/private window — extensions can interfere.
4. On macOS, the captive portal prompt may not appear if you dismissed it
   before. Navigate to the URL manually.

## Local Vite repeatedly returns 502 on macOS

**Symptom:** `curl "http://<board-ip>/api/v1/status"` returns 200, but
`http://127.0.0.1:5173/api/v1/status` returns `502 Bad Gateway`; the Vite log
contains `EHOSTUNREACH` from a Node.js socket.

Use the repository launcher rather than invoking `vite` directly:

```sh
cd web
npm run dev
```

The launcher automatically starts a loopback-only native TCP forwarder for the
USB-NCM interface and uses it for HTTP and WebSocket proxying. If the problem
persists, confirm that `/usr/bin/ruby` exists and that the startup log includes
`USB-NCM loopback:`. `LINKR_NCM_FORWARDER=off` disables the workaround for
diagnostics; it should not be set on an affected macOS host.

The launcher supervises the forwarder and restarts it on the same loopback port
after an unexpected exit. If repeated restarts fail, Vite exits with an explicit
forwarder error instead of staying online and returning an endless series of
502 responses.

## macOS Gatekeeper blocks the CLI

**Symptom:** "Apple cannot verify this software" dialog.

```sh
xattr -dr com.apple.quarantine ./radxa-linkr-debuggerctl
```

The install script does this automatically. If you unpacked manually, run the
command above on the binary.

## TUI shows stale data

**Symptom:** Switch state or GPIO values don't update after toggling.

The TUI polls at 60 Hz, but switch routes have physical settling time. After
changing a route, wait 1–2 seconds before checking `actual` vs `desired`.
If `desired` and `actual` disagree, the hardware mux may not have switched —
try the route command again.

## Switch route doesn't take effect

**Symptom:** `switch get` still shows the old route after `switch route`.

1. Enumerate the firmware-advertised switch catalog:
   ```sh
   radxa-linkr-debuggerctl switch list
   ```
2. Read the selected switch before changing it:
   ```sh
   radxa-linkr-debuggerctl switch get sd
   ```
3. Route only an advertised switch and route value:
   ```sh
   radxa-linkr-debuggerctl switch route sd usb-reader
   radxa-linkr-debuggerctl switch route tf_wp protected
   ```
4. Wait a few seconds and check again. Hardware muxes need settling time.
5. Don't run opposing route commands in rapid succession on the same switch.

x1.2 has no `switch/usb` item. `GET /api/v1/switch/usb` returns HTTP 404;
do not retry that non-existent endpoint. Read `GET /api/v1/switch` or
`radxa-linkr-debuggerctl switch list` for the current catalog instead.

## Web vdd_5v control is read-only or rejects a confirmed change

The header and hardware drawer share the same vdd_5v action. Both require a
current power snapshot and one confirmation, and stay pending together until the
request finishes. If automatic refresh was paused for 30 seconds, refresh the
board status first. Finish any active hardware task before retrying.

If connectivity, the output state, or the task lock changes while confirmation
is open, the action is rejected without sending a power request. Refresh and
check the current state rather than repeatedly clicking the switch.

## ADC reads zero

**Symptom:** `adc read` shows 0.000000A on all channels.

The power output must be ON for current to flow. Check:
```sh
radxa-linkr-debuggerctl power list
```

If the rail is off, turn it on first:
```sh
radxa-linkr-debuggerctl power set 5v_out on
```

## OTA upload fails

**Symptom:** `ota upload` returns an error.

- `sha256_mismatch`: the file was corrupted during transfer. Re-download and try again.
- `size_mismatch`: the `X-Linkr-Ota-Size` header doesn't match the actual file size. Re-upload.
- `invalid_mcuboot_header`: you're uploading the wrong file format. Use `radxa-linkr-debugger-rp2350-ota.bin`, not `.uf2` or `.elf`.
- `image_too_large`: the firmware doesn't fit in the OTA partition.

## Watchdog keeps resetting the board

**Symptom:** Board reboots repeatedly, LED stops blinking.

The watchdog resets the board when core services stop reporting healthy. This
usually means the firmware has crashed. Connect via CDC ACM serial to see the
crash log:

```sh
# Linux / macOS
screen /dev/ttyACM0 115200
```

If the board is stuck in a boot loop, use the BOOTSEL recovery:
```sh
# From the Zephyr shell (if accessible)
bootloader

# Or hold BOOTSEL while powering on, then:
picotool load -v -x radxa-linkr-debugger-rp2350.uf2
```

## CDC ACM serial not showing up

**Symptom:** No `/dev/ttyACM0` (Linux) or COM port (Windows) after plugging in.

1. Check `dmesg` (Linux) or Device Manager (Windows) for USB enumeration errors.
2. Try a different USB port — some hubs don't pass CDC ACM properly.
3. On Linux, you may need to add your user to the `dialout` group:
   ```sh
   sudo usermod -aG dialout $USER
   ```
   Log out and back in for the group change to take effect.

## Serial terminal size differs from the target tty

The Web UI fits its **local xterm** after layout and fonts are ready, when the
window or terminal container changes size, and when a hidden browser tab becomes
visible again. Fullscreen and font-size changes also refit the local terminal.
Switching away from the Terminal workspace may hide an ancestor without changing
browser-tab visibility. In that case fit is skipped until the terminal has a
nonzero layout box again; hidden percentage sizes must not shrink the terminal
or discard its alternate screen. Initial webfonts are loaded before xterm's
first measurement, with fallback fonts used if loading fails.

Like picocom over raw UART, this does not automatically push browser dimensions
to the target tty through `TIOCSWINSZ`, but a remote `resize` does not require a
PTY message: standard xterm/BusyBox `resize` probes the bottom-right cursor
position over UART and reads CPR. After local fit succeeds at 132 columns ×
43 rows, the selected browser responder returns `ESC[43;132R`. The `CSI 18 t`
character-size query used by `resize -s` returns `ESC[8;43;132t`. Only that
read-only window-size query is enabled; window resizing and other window
operations are not.

Check the **Auto replies** indicator: an observer browser sends no automatic
size replies. With CDC DTR asserted, CDC owns responses, not this browser.
Automation, backpressure, a non-ready serial port or a pending responder
promotion can also suppress replies. After those conditions clear, rerun the
target's `resize` command. Local fit alone sends no UART bytes and does not
automatically update the target's `stty size`. See the
[serial transport contract](../reference/serial-broker.md#local-terminal-size).

## Logic analyzer capture fails

**Symptom:** `POST /api/v1/logic-analyzer` returns `invalid_config`.

- The Web picker is temporarily limited to GP12–GP16. ADC1–ADC3 capture support
  is not implemented; their contacts remain read-only pending the capture
  firmware/ownership/protocol review. The eight-channel target is digital
  GP12–GP16 plus GP27–GP29, not analog voltage sampling. GP7–GP9 recovery controls
  are disabled. The native sigrok
  protocol retains GP10–GP20; see [Capture GPIO Pins](logic-analyzer.md#capture-gpio-pins).
- FAST8 still takes GP10/GP11 and pauses J4, even when only GP12–GP16 are selected.
- `pre_samples > 0` only works with edge triggers at ≤25 MHz.
- Total samples (pre + post) can't exceed 512.
- Rate must be between 100,000 and 125,000,000 Hz.

## Multiple TUI instances interfere

**Symptom:** One TUI instance's changes don't appear in another.

This shouldn't happen — the TUI uses HTTP polling and multiple instances
should coexist. If you see issues, make sure each instance is using the same
board URL. Mixing `--url` arguments can cause confusion.

## Still stuck?

- Check the [CLI Reference](cli.md) for command details
- Check the [HTTP API Reference](api.md) for raw API debugging
- File an issue: [GitHub Issues](https://github.com/xzl01/agent-debugboard/issues)
