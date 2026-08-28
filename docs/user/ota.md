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

The firmware is designed to use a retained marker so that an unconfirmed test
image reset can request MCUboot rollback rather than ROM BOOTSEL. The
repository HIL runner can exercise this safely through the test-only
`CONFIG_LINKR_DEBUGGER_FAULT_INJECTION` build; production firmware does not
include that hook. Explicit `bootloader` commands and ordinary non-OTA watchdog
resets still enter ROM BOOTSEL normally.

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

The Web UI provides an OTA card under **Firmware Tools** in the dashboard. It delivers
RP2350 firmware updates over the same USB NCM HTTP API, with no separate host
tooling required. When the UI is served from GitHub Pages over HTTPS, Linkr Host
(`npm run host`) is required. See
[webui.md](webui.md) for details.

## MCUboot Rollback and Recovery

The intended unconfirmed-image failure path uses the retained marker to request
MCUboot rollback after a watchdog reset. The fault-injection path is implemented
only in the HIL overlay; verify the running image before confirmation and keep
the physical ROM BOOTSEL recovery path available. Initial install and recovery
use the combined `radxa-linkr-debugger-rp2350.uf2` artifact described above.

## Related

- [Web UI](webui.md)
- [OpenOCD / JTAG](openocd.md)
