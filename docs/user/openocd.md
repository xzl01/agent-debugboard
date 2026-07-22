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

Install OpenOCD, then verify:

```sh
openocd --version
```

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

Use power-cycling (`radxa-linkr-debuggerctl power set 5v_out off` then `on`)
only as a hard-restart fallback when soft reset is not available.

## Full Workflow

See [doc/openocd/README.md](../../doc/openocd/README.md) for the full OpenOCD
workflow and configuration details.

## Related

- [Web UI](webui.md)
- [OTA firmware update](ota.md)
