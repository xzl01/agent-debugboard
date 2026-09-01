# Linux distribution packages

Each native recipe emits two packages:

- `radxa-linkr-debuggerctl`: standalone CLI/TUI, the `rdb` alias, and the shared desktop launcher and icon
- `radxa-linkr-debugger-firmware`: combined ROM BOOTSEL UF2 and unsigned MCUboot-format OTA image

The launcher opens `radxa-linkr-debuggerctl` as a terminal TUI. Web, Host, tray,
and autostart files are intentionally not included, and the firmware package
never contains the launcher, icon, or application-only `zephyr.uf2`.

Tagged releases and rolling nightly builds both call the shared
`.github/workflows/native-packages.yml` workflow. It stages the Linux CLI
archive and validated firmware images, then invokes each distribution's native
tool: `dpkg-buildpackage`, `rpmbuild`, and `makepkg`. The minimum compatibility
baselines are Debian 12 (bookworm) and RHEL 9; CI exercises the RPM recipe on
AlmaLinux 9.

For a local Debian build, place the extracted CLI and both firmware files in
`dist/native-package-input/` before running
`dpkg-buildpackage --build=binary --no-sign`. RPM and Arch builds consume the
same files through their native source mechanisms. The Arch recipe requires
all four `AGENT_DEBUGBOARD_*_SHA256` values; the shared workflow calculates
them from the exact staged inputs, and the published `SHA256SUMS.txt` records
the same source archive, CLI archive, and firmware assets.
