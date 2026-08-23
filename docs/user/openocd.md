[中文](openocd.zh-CN.md)

# OpenOCD / JTAG

Radxa Linkr Debugger can be used together with OpenOCD: the Linkr Debugger
handles target power and recovery control, while the onboard CH347F handles
target JTAG/SWD.

## Architecture

- **Linkr Debugger** — provides power control, boot-mode selection, and
  recovery for the target board.
- **CH347F** — wired directly to the target debug connector. RP2350 does not
  sit in the JTAG/SWD path and does not act as a CMSIS-DAP or JTAG probe.

## Install and Verify

The repository ships a pinned, CH347-enabled OpenOCD package as the
`openocd-latest` flake output. The package name is stable; the upstream
revision is pinned to the commit that included CH347 JTAG/SWD support at
update time (currently `da3920b0a52dc2d394afb222c688dac7e57acc1b`) and does
not float on every evaluation. Use this package whenever you need a
reproducible CH347-capable OpenOCD.

Run the pinned package directly from the repository:

```sh
nix shell .#openocd-latest -c openocd --version
nix shell .#openocd-latest -c openocd -c "adapter list" -c shutdown
```

The executable name is `openocd`, and `adapter list` must include `ch347`.
To consume the package from outside the repository, use the flake reference:

```sh
nix shell github:xzl01/agent-debugboard#openocd-latest -c openocd --version
```

The flake also exposes `agent-debugboard.packages.${system}.openocd-latest`,
and the repository overlay adds `pkgs.openocd-latest` to a Nixpkgs import.

If you cannot use the Nix package, install OpenOCD from your system package
manager and verify:

```sh
openocd --version
```

If your system OpenOCD does not advertise `ch347` in `adapter list`, prefer
the repository's `openocd-latest` package, fall back to the WCH/vendor OpenOCD
build, or add the matching interface script before running the target flow.

## Power the Target

Power the target board before starting OpenOCD:

```sh
radxa-linkr-debuggerctl power set 5v_out on
```

## Start OpenOCD

Start OpenOCD with the CH347F interface script available in your OpenOCD
installation and the target configuration for the board under test:

```sh
openocd -f interface/<ch347-interface>.cfg -f target/<target>.cfg
```

Replace `<ch347-interface>` with the CH347F interface script name (depends on
your OpenOCD build) and `<target>` with the target board configuration.

## CH347F Support

CH347F support depends on the OpenOCD build. If the system OpenOCD package does
not include a CH347F interface script, use the WCH/vendor OpenOCD build or add
the matching interface script.

## GDB and Telnet

OpenOCD normally exposes:

- **GDB server** on TCP port `3333`
- **Telnet control** on TCP port `4444`

Connect GDB with:

```sh
target extended-remote :3333
```

## Reset Strategies

Prefer OpenOCD reset commands or the target OS reboot path first:

- `reset halt` — reset the target and halt immediately
- `reset run` — reset the target and let it run

The current CH347 driver does not expose physical SRST in SWD mode. The
RP2040 target configuration uses `SYSRESETREQ` instead, so `reset halt` and
`reset run` act on the target core rather than a board-level reset line.
Do not rely on physical reset through the CH347F.

Use power-cycling (`radxa-linkr-debuggerctl power set 5v_out off` then `on`)
only as a hard-restart fallback when soft reset is not available.

## Full Workflow

See [docs/reference/openocd/README.md](../reference/openocd/README.md) for the full OpenOCD
workflow and configuration details.

## Related

- [Web UI](webui.md)
- [OTA firmware update](ota.md)
