# Flashing

[中文](flashing.zh-CN.md)

Firmware can be updated in two ways depending on the current state of the board.

## ROM BOOTSEL flashing (initial install or recovery)

If the board is already running this firmware, ask it to enter BOOTSEL and then
load the new combined RP2350 UF2:

```sh
radxa-linkr-debuggerctl bootloader
picotool load -v -x radxa-linkr-debugger-rp2350.uf2
```

After firmware changes, treat this BOOTSEL flow and the CDC ACM shell
fallback below as required validation paths; do not conclude the change until
you have verified the serial fallback path still works.

If HTTP/WS control is unavailable but the MCU CDC ACM shell is still up, you
can enter the same BOOTSEL path from the local Zephyr shell:

```text
linkr-debugger:~$ bootloader
```

If the board is already mounted as `RPI-RP2`, only run:

```sh
picotool load -v -x radxa-linkr-debugger-rp2350.uf2
```

On Linux you can also flash without root by using `udisksctl` to mount the
`RPI-RP2` volume and then copying the canonical UF2:

```sh
RPI_RP2=$(udisksctl mount -b /dev/sdX1 | awk -F" at " '{print $2}' | tr -d '[:space:]')
cp radxa-linkr-debugger-rp2350.uf2 "$RPI_RP2/"
```

Replace `/dev/sdX1` with the actual BOOTSEL block device path on your
system (use `lsblk -o NAME,SIZE,VENDOR,MOUNTPOINT` and look for the `RPI` vendor
entry). If you use drag-and-drop flashing through the `RPI-RP2` volume instead of
`picotool`, copy this same initial-install/recovery artifact:

```text
radxa-linkr-debugger-rp2350.uf2
```

## OTA flashing (after initial MCUboot install)

After the MCUboot-capable firmware is installed on RP2350, subsequent firmware
updates can be delivered via OTA using a MCUboot-format application binary.
Upload the firmware binary, trigger a test boot, and confirm:

```sh
radxa-linkr-debuggerctl ota upload /path/to/firmware.bin
radxa-linkr-debuggerctl ota test
# After verifying the test boot succeeded:
radxa-linkr-debuggerctl ota confirm
```

Or, after a successful test boot, wait for the 16-second watchdog health gate
to auto-confirm the image. If the test image is not confirmed and the watchdog
resets, the retained marker drives MCUboot rollback rather than ROM BOOTSEL.

Do not upload a `.uf2` or `.elf` file via OTA. OTA expects a MCUboot-format
application binary. Use the release asset `radxa-linkr-debugger-rp2350-ota.bin`,
which is copied from the sysbuild application output
`build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin`.
Despite the `signed.bin` build filename, this project config uses unsigned
MCUboot format.
