[中文](ota.zh-CN.md)

# MCUboot OTA Firmware Update

The firmware supports unsigned MCUboot over-the-air firmware update for RP2350.

## Security Facts

- **No signature verification.** The OTA path does not verify cryptographic
  signatures.
- **No authentication.** Any host with USB NCM access to the board can submit a
  firmware image.
- **No secure boot.** The bootloader does not enforce a chain of trust.
- **No anti-rollback.** There is no version monotonicity check to prevent
  downgrading.
- **SHA256 for integrity only.** The CLI computes SHA256 of the uploaded payload
  and sends it as a header. The firmware recalculates the hash on the device and
  rejects mismatches. SHA256 verifies integrity, not authenticity or signing.

## Initial Installation

Initial installation of the MCUboot-capable firmware requires ROM BOOTSEL
flashing with a combined bootable artifact (`radxa-linkr-debugger-rp2350.uf2`).
After the first MCUboot install, subsequent updates can be delivered via OTA
using MCUboot-format application binaries.

## Accepted Artifact

- **Use**: MCUboot-format application `.bin` — specifically
  `radxa-linkr-debugger-rp2350-ota.bin`, copied from the sysbuild application
  output `zephyr.signed.bin`.
- **Do NOT upload** `.uf2` or `.elf` via OTA.
- Despite the `signed.bin` build filename, this project config uses unsigned
  MCUboot format.

## OTA Workflow

The workflow has three steps: **upload** → **test** → **confirm**.

1. **Upload** the MCUboot-format application binary to the board.
2. **Test** — request a test boot of the newly uploaded image. The board reboots
   after a short delay.
3. **Confirm** — after verifying the test boot succeeded, manually confirm the
   image. Alternatively, wait for the ~16-second watchdog health gate to
   auto-confirm.

If the test image is not confirmed and a watchdog reset occurs during the
unconfirmed window, the dedicated retained marker allows MCUboot to perform a
rollback to the previous confirmed image instead of forcing entry into ROM
BOOTSEL. Explicit `bootloader` commands and ordinary non-OTA watchdog resets
still enter ROM BOOTSEL normally.

## CLI Commands

```sh
radxa-linkr-debuggerctl ota status
radxa-linkr-debuggerctl ota upload /path/to/firmware.bin
radxa-linkr-debuggerctl ota test
radxa-linkr-debuggerctl ota confirm
```

### `ota status`

Reports the current OTA state, flash sizes, and MCUboot swap type.

States: `idle`, `uploading`, `verified`, `pending_test`, `rebooting`, `failed`.

### `ota upload PATH`

Sends a MCUboot-format `.bin` file. The CLI computes SHA256 and sends it along
with the byte size as headers.

### `ota test`

Requests a test boot of the verified image. The board reboots after a short
delay. If the watchdog reports healthy after the test boot, the image
auto-confirms after a ~16-second gate.

### `ota confirm`

Manually confirms the running image immediately, clearing the auto-confirm
timer.

## JSON Output for Automation

Agent or automation code should prefer JSON output:

```sh
radxa-linkr-debuggerctl --json ota status
radxa-linkr-debuggerctl --json ota upload /path/to/firmware.bin
radxa-linkr-debuggerctl --json ota test
radxa-linkr-debuggerctl --json ota confirm
```

## Web OTA Card

The Web UI also provides an OTA card under **Advanced & recovery**. It delivers
RP2350 firmware updates over the same USB NCM HTTP API, with no separate host
tooling required. When the UI is served from GitHub Pages over HTTPS, the
device-bridge gateway (`npm run device-bridge`) is required. See
[webui.md](webui.md) for details.

## MCUboot Rollback

If the test image is not confirmed and the watchdog resets, the retained marker
drives MCUboot rollback instead of ROM BOOTSEL. This means a bad image does not
leave the board unrecoverable — the previous confirmed image is restored
automatically.

## Related

- [Web UI](webui.md)
- [OpenOCD / JTAG](openocd.md)
