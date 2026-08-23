# OpenOCD Support

Radxa Linkr Debugger works with OpenOCD through the onboard CH347F path:

```text
PC OpenOCD -> CH347F -> target JTAG/SWD
PC radxa-linkr-debuggerctl -> RP2350 -> target power / boot mode / ADC / SD / GPIO
```

For this advanced workflow, use the released Rust `radxa-linkr-debuggerctl`
CLI. Use direct `curl` only when debugging the raw HTTP API or when following
the separate, curl-first Agent skill.

CH347F is wired directly to the target debug connector. The MCU firmware does
not sit in the JTAG/SWD path, does not mux those lines, and should not be treated as a
CMSIS-DAP/Picoprobe adapter.

## Install OpenOCD

### Nix package (recommended, pinned CH347)

The repository ships a pinned CH347-enabled OpenOCD package as the
`openocd-latest` flake output. The package name is stable; the upstream
revision is pinned to a specific commit that includes CH347 JTAG/SWD support
(currently `da3920b0a52dc2d394afb222c688dac7e57acc1b`), and does not float on
every evaluation. Use this whenever you need reproducible CH347 support.

Run it directly from the repository:

```sh
nix shell .#openocd-latest -c openocd --version
nix shell .#openocd-latest -c openocd -c "adapter list" -c shutdown
```

The package exposes `openocd` as its executable. `adapter list` must include
`ch347`.

Use the package from outside the repository via the flake reference:

```sh
nix shell github:xzl01/agent-debugboard#openocd-latest -c openocd --version
```

The flake also exposes `agent-debugboard.packages.${system}.openocd-latest`,
and the repository overlay adds `pkgs.openocd-latest` to a Nixpkgs import.
See `docs/developer/build.md` for the package/overlay/flake boundary.

### OS packages (fallback)

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

CH347F support depends on the system OpenOCD build and adapter driver
scripts available on the host. If `adapter list` does not include `ch347`,
prefer the repository's `openocd-latest` package above, fall back to the
WCH/vendor OpenOCD build, or add the matching interface script before
running the target flow.

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
  http://172.29.203.1/api/v1/power/5v_out
curl -fsS http://172.29.203.1/api/v1/power
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

The current CH347 driver does not provide physical SRST in SWD mode. The
RP2040 target configuration uses `SYSRESETREQ` instead, so `reset halt` and
`reset run` act on the target core, not a board-level reset line. Do not
rely on physical reset through the CH347F.

Only if the target is unresponsive, has no reset line, or soft reset fails,
hard-restart the target by power-cycling the power output that actually powers it:

```sh
radxa-linkr-debuggerctl power set 5v_out off
sleep 2
radxa-linkr-debuggerctl power set 5v_out on
```

Do not power-cycle unrelated power outputs.
