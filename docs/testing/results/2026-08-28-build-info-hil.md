# 2026-08-28 Firmware Build Identity HIL

## Result

**PASS for the exercised firmware build-identity surface.** The canonical
combined UF2 was flashed, normal HTTP startup recovered, the CDC ACM shell
reported the new `uname` fields and Zephyr application-version commands, and
the generated headers contained the expected release and git build identity.

## Scope

Validating the firmware `uname`-style build identity change:

- Zephyr application `VERSION` and generated `app_version.h`.
- CMake-provided `BUILD_VERSION` from `git describe --always --abbrev=12
  --dirty`.
- New `uname`, `app version`, `app version-extended`, and
  `app build-version` shell surfaces.
- Startup log line containing the firmware build id.

## Build Artifacts

- Combined UF2:
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`
  - SHA256 `89c7924b152c3c4f02b3656971c97ec1c82b0bf020b0e344912833aebe2f0148`
- OTA payload:
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350-ota.bin`
  - SHA256 `c4a85f7a484648f097abe07a93e0eb9c999f8800d2911a24842b8f760a69e9f4`
- Generated application header:
  `build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/include/generated/zephyr/app_version.h`
  - `APP_VERSION_STRING = "0.3.0"`
  - `APP_VERSION_EXTENDED_STRING = "0.3.0+0"`
  - `APP_BUILD_VERSION = v0.2.1-208-gc72104d89ca1`
- Generated kernel/build header:
  `build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/include/generated/zephyr/version.h`
  - `BUILD_VERSION = v0.2.1-208-gc72104d89ca1-dirty`

## HIL Results

1. Canonical combined-UF2 flash: PASS. `POST /api/v1/bootloader` entered ROM
   BOOTSEL, RPI RP2350 enumerated as `/dev/sdb1`, `udisksctl mount` exposed the
   target, and the combined UF2 was copied and unmounted.
2. Normal startup: PASS. HTTP recovered within one second; `GET
   /api/v1/status` returned `ok=true`, `mcu=rp2350`, and watchdog healthy.
3. CDC ACM shell: PASS. `/dev/ttyACM2` (Radxa Linkr Debugger,
   `2fe3:db01`) accepted commands and returned:

   ```text
   uname -a
   Zephyr linkr-debugger 0.3.0 v0.2.1-208-gc72104d89ca1-dirty (Jan  1 1980 00:00:00) arm rp2350a rpi_pico2/rp2350a/m33/mcuboot

   uname -v
   v0.2.1-208-gc72104d89ca1-dirty (Jan  1 1980 00:00:00)

   app version
   0.3.0

   app version-extended
   0.3.0+0

   app build-version
   v0.2.1-208-gc72104d89ca1
   ```

   The `--help` handling and combined short options such as `-smi` are
   registered in the same command; the `-dirty` build id is the intended
   unique identity for this worktree build.
4. Startup identity: PASS by artifact inspection. `zephyr.elf` contains the
   `firmware build id %s at %s` log template and the generated dirty build id.

## Cleanup

- Board is back on the normal firmware image: USB `2fe3:db01`, NCM `eth0`
  reachable at `172.29.203.1`.
- No configuration snapshot, task blob, power route, or target power state was
  changed by this run.
- No BOOTSEL device remains mounted or active.

## Evidence

- [Serial capture](./2026-08-28-build-info-hil.serial.log)
- [Raw capture](./2026-08-28-build-info-hil.raw.jsonl)
- [SHA256SUMS](./2026-08-28-build-info-hil.SHA256SUMS)
