[中文](troubleshooting.zh-CN.md)

# Troubleshooting

Common problems and how to fix them.

## CLI can't connect to the board

**Symptom:** `doctor` fails or `status` returns a connection error.

1. Check the USB cable. Data cables only — charge-only cables won't work.
2. Verify the board shows up as a network interface:
   - Linux: `ip link show` — look for a new `enp*` or `usb*` interface
   - macOS: `ifconfig` — look for `en*` with `172.29.203.x`
   - Windows: check Network Connections for a new RNDIS/NCM adapter
3. Ping the board: `ping 172.29.203.1`
4. If ping works but the CLI fails, try specifying the URL explicitly:
   ```sh
   radxa-linkr-debuggerctl --url http://172.29.203.1 doctor
   ```
5. If nothing works, try a different USB port or cable.

## Web UI doesn't open

**Symptom:** Browser shows "can't reach this site" at `http://172.29.203.1/`.

1. Verify CLI connectivity first: `radxa-linkr-debuggerctl doctor`
2. If doctor passes but the browser can't connect, check if another network
   interface has priority. The board uses a link-local-style address; some
   OS configurations prefer other routes.
3. Try opening in an incognito/private window — extensions can interfere.
4. On macOS, the captive portal prompt may not appear if you dismissed it
   before. Navigate to the URL manually.

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

1. Check the `desired` vs `actual` fields in the status output.
2. For USB and VIN routes, make sure you passed `--confirm`:
   ```sh
   radxa-linkr-debuggerctl switch route usb target --confirm
   ```
3. Wait a few seconds and check again. Hardware muxes need settling time.
4. Don't run opposing route commands in rapid succession on the same switch.

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

## Logic analyzer capture fails

**Symptom:** `POST /api/v1/logic-analyzer` returns `invalid_config`.

- Check that your pins are in the safe allowlist: GP7–GP9, GP10–GP20, GP29.
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
