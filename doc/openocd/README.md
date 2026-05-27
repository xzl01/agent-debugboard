# OpenOCD Support

Radxa Linkr Debugger works with OpenOCD through the onboard CH347F path:

```text
PC OpenOCD -> CH347F -> target JTAG/SWD
PC radxa-linkr-debuggerctl -> RP2040 -> target power / boot mode / ADC / SD / GPIO
```

For normal use, prefer the released `radxa-linkr-debuggerctl` CLI. Use direct
`curl` only when debugging the raw HTTP API or when following the separate,
curl-first Agent skill.

CH347F is wired directly to the target debug connector. The RP2040 does not sit
in the JTAG/SWD path, does not mux those lines, and should not be treated as a
CMSIS-DAP/Picoprobe adapter.

## Install OpenOCD

macOS:

```sh
brew install open-ocd
```

Ubuntu/Debian:

```sh
sudo apt-get install openocd
```

Windows users can install OpenOCD through MSYS2 or a vendor-provided package.

Verify:

```sh
openocd --version
```

CH347F support depends on the OpenOCD build and adapter driver scripts available
on the host. If your system OpenOCD does not include CH347F support, use the
WCH/vendor OpenOCD build or add the matching interface script before running
the target flow.

## Target Flow

Power the target first:

```sh
radxa-linkr-debuggerctl power set 5v_out on
radxa-linkr-debuggerctl power list
```

If you need to inspect the raw HTTP API instead of the released CLI, the
equivalent fallback is:

```sh
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"on"}' \
  http://172.29.203.1:8080/api/v1/power/5v_out
curl -fsS http://172.29.203.1:8080/api/v1/power
```

Start OpenOCD with the CH347F interface from your OpenOCD installation and the
target config for the board under test:

```sh
openocd -f interface/<ch347-interface>.cfg -f target/<target>.cfg
```

The repository includes [ch347-example.cfg](ch347-example.cfg) as a template for
adapter speed and transport selection, but the concrete CH347F interface line is
host-build dependent.

OpenOCD normally exposes its default GDB server on TCP `3333` and telnet command
server on TCP `4444`:

```sh
gdb-multiarch build/firmware.elf -ex "target extended-remote localhost:3333"
telnet localhost 4444
```

## Reset Guidance

Prefer a target software reboot or OpenOCD reset command first:

```text
reset halt
reset run
```

Only if the target is unresponsive, has no reset line, or soft reset fails,
hard-restart the target by power-cycling the power output that actually powers it:

```sh
radxa-linkr-debuggerctl power set 5v_out off
sleep 2
radxa-linkr-debuggerctl power set 5v_out on
```

Do not power-cycle unrelated power outputs.
