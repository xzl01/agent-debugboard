# Linux distribution packages

Each native recipe emits two packages:

- `radxa-linkr-debuggerctl`: standalone CLI/TUI, the `rdb` alias, the shared desktop launcher and icon, and the Linux WebUSB access rule
- `radxa-linkr-debugger-firmware`: combined ROM BOOTSEL UF2 and unsigned MCUboot-format OTA image

The launcher opens `radxa-linkr-debuggerctl` as a terminal TUI. Web, Host, tray,
and autostart files are intentionally not included, and the firmware package
never contains the launcher, icon, udev rules, or application-only `zephyr.uf2`.

The single rule source is `packaging/udev/70-linkr-webusb.rules`. Native CLI
packages install it as `/usr/lib/udev/rules.d/70-linkr-webusb.rules`; the Nix
desktop package carries it under `$out/lib/udev/rules.d/`. It matches Linkr's
`2fe3:db01` USB identity, not an individual serial number, and uses `uaccess`
to grant device read/write access to the active local desktop user's UID, not
to an individual browser or session. SSH-only/headless logins do not establish
the grant, but processes sharing an already-authorized UID can use it.

Reconnect the board with the browser running after enabling the rule. This
project adds no USB reset, forced re-enumeration, or browser restart hook. Rule
reload follows the distribution's existing policy: Arch's systemd package
manager hook may also replay system-wide udev `change` events. Those events are
not themselves USB bus resets, but existing local udev rules may run. NixOS requires
an explicit `services.udev.packages` declaration; user-level archive installers
remain unprivileged and do not install system rules. See
[Linux WebUSB permissions](../docs/user/install.md#linux-webusb-permissions).

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
