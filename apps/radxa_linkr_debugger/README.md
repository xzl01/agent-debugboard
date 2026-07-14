# radxa-linkr-debugger Zephyr App

This directory contains the Zephyr application for `radxa-linkr-debugger`. The root
[README.md](../../README.md) contains workspace setup, flashing, and usage
instructions.

Unless noted otherwise, run the commands below from the repository root.

Agent/AI operators should read the repository skill first:
[skills/radxa-linkr-debugger/SKILL.md](../../skills/radxa-linkr-debugger/SKILL.md).

Build (G2 / RP2040):

```sh
west build -p always -b rpi_pico/rp2040 apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
```

Build (G3 / RP2350):

```sh
west build -p always -b rpi_pico2/rp2350a/m33 apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
```

Use `build/radxa_linkr_debugger/` as the only canonical build directory for this
app, and use `build/radxa_linkr_debugger/zephyr/zephyr.uf2` as the only canonical
flash artifact.

Tests:

```sh
./apps/radxa_linkr_debugger/tests/run_unit_tests.sh
```

Schematics:

```text
doc/radxa-linkr-debugger-schematic-x1.1.pdf  (G3 / RP2350A)
doc/radxa-linkr-debugger-schematic.pdf       (G2 / RP2040)
```

The USB interface enumerates as a composite USB device. The board exposes its
HTTP control API on the USB NCM interface at `http://172.29.203.1:8080` by
default. Ordinary users should prefer the Web UI. Advanced users, Agents,
automation, and HIL validation should use the released Rust
`radxa-linkr-debuggerctl`; direct `curl` remains the raw HTTP/API path.
A USB CDC ACM serial port is also kept available for Zephyr cmdline access and
BOOTSEL fallback.

The board also runs a DHCPv4 server on the NCM link so the host can acquire a
compatible address automatically.

For long-lived telemetry and bidirectional control, create a live session over
HTTP first and connect to the returned dedicated WebSocket URL under
`/api/v1/ws/<slot>`. The firmware supports up to four concurrent WebSocket
clients; each live session gets a dedicated slot URL. Subscriptions may request
`batch_size` from 1 through 20, with independent per-client sequence cursors so
one slow subscriber cannot consume another subscriber's samples.
ADC telemetry includes `sample_sequence` and device-side monotonic
`device_t_mono_us`. The same session can arm a triggered power capture with a
firmware ring buffer: 2048 samples on G3 and 512 on G2. Manual, current
threshold, allowlisted GPIO edge, and power-output off-to-on triggers are
supported. Triggered capture uses one global hardware buffer and therefore has
only one capture owner at a time. See
[the power analyzer protocol](../../doc/power-analyzer.md).
When HTTP/WS is unavailable but the CDC ACM shell still works, the local shell
command below enters the current MCU's ROM BOOTSEL path used by the HTTP API:

```text
linkr-debugger:~$ bootloader
```

Advanced users, Agents, automation, and HIL checks should use the released
Rust `radxa-linkr-debuggerctl` CLI.
If you are developing `cmd-ng` itself, use `cargo run --manifest-path
cmd-ng/Cargo.toml -- ...`. Raw HTTP examples below are only for firmware/API
debugging. Full CLI examples are in the root [README.md](../../README.md), and
the [skill](../../skills/radxa-linkr-debugger/SKILL.md) remains the curl-first
Agent workflow.

Raw HTTP checks for firmware/API debugging:

```sh
curl -fsS http://172.29.203.1:8080/api/v1/status
curl -fsS http://172.29.203.1:8080/api/v1/power
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"on"}' \
  http://172.29.203.1:8080/api/v1/power/12v_out
curl -fsS http://172.29.203.1:8080/api/v1/adc/read?channel=5v_out
curl -fsS http://172.29.203.1:8080/api/v1/watchdog
curl -fsS http://172.29.203.1:8080/api/v1/switch
curl -fsS http://172.29.203.1:8080/api/v1/switch/vin   # G3: returns 1.8v or 3.3v; G2: unavailable
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"3.3v"}' \
  http://172.29.203.1:8080/api/v1/switch/vin   # safe default; G3 only
```

VIN defaults to 3.3V at boot. G3-only VIN 1.8V switching is documented in the
expert gated section below; it requires target voltage compatibility confirmation
and physical measurement setup before use.

`GET /api/v1/status` includes `board_monitoring`, and WebSocket
`snapshot/status` messages include the same object. The categories are
`temperature`, `heap`, `runtime`, and `cpu`; each one reports `available` and a
machine-readable `reason`. Values are emitted only when Zephyr exposes a real
device or runtime-stat API. With the default RP2040 and RP2350 configurations,
the board reports internal MCU die temperature, system heap runtime statistics,
real board uptime (`uptime_ms` / `uptime_seconds`), and CPU utilization deltas
when those readings are available. The CPU utilization field can still report
`insufficient_runtime_window` until the firmware has accumulated enough runtime
delta to derive a percentage.

`GET /api/v1/watchdog` reports the autonomous firmware watchdog state. Firmware
itself owns watchdog arming and feeding. If core firmware, API service, or the
CDC ACM cmdline fallback stop reporting healthy liveness, firmware stops
feeding the hardware watchdog and the retained recovery marker drives the next
boot into ROM BOOTSEL. The CDC ACM `bootloader` shell command remains available
as an independent fallback path.

FIXME: add a safe fault-injection path for validation of daemon/service failure
scenarios. The current firmware and manual validation confirm websocket
recovery, autonomous watchdog status reporting, and CDC ACM `bootloader`
fallback into ROM BOOTSEL, but they do not yet provide a controlled way to
intentionally wedge HTTP/WS/cmdline liveness and prove automatic watchdog
timeout into BOOTSEL without using ad hoc destructive test methods.

Safe GPIO names such as `GP13` are derived from the MCU pin number; the
firmware allowlist keeps the connector note so users can map commands back to
the exposed header position.

Develop the Rust host CLI from source:

```sh
cargo build --manifest-path cmd-ng/Cargo.toml
```

For advanced, Agent, automation, or HIL use, prefer the released CLI:

```sh
radxa-linkr-debuggerctl status
radxa-linkr-debuggerctl doctor
radxa-linkr-debuggerctl --json status
radxa-linkr-debuggerctl --json adc read
radxa-linkr-debuggerctl adc read -v 5v_out
radxa-linkr-debuggerctl power set 12v_out on
radxa-linkr-debuggerctl power set 20v_out on
radxa-linkr-debuggerctl switch route sd usb-reader
radxa-linkr-debuggerctl switch get vin   # G3: returns 1.8v/3.3v; G2: unavailable
radxa-linkr-debuggerctl watchdog status
```

If you are iterating on unreleased `cmd-ng` changes, the equivalent source-run
workflow is:

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- status
cargo run --manifest-path cmd-ng/Cargo.toml -- doctor
cargo run --manifest-path cmd-ng/Cargo.toml -- --json status
cargo run --manifest-path cmd-ng/Cargo.toml -- --json adc read
cargo run --manifest-path cmd-ng/Cargo.toml -- adc read -v 5v_out
cargo run --manifest-path cmd-ng/Cargo.toml -- power set 12v_out on
cargo run --manifest-path cmd-ng/Cargo.toml -- power set 20v_out on
cargo run --manifest-path cmd-ng/Cargo.toml -- switch route sd usb-reader
cargo run --manifest-path cmd-ng/Cargo.toml -- switch get vin
cargo run --manifest-path cmd-ng/Cargo.toml -- watchdog status
```

The released CLI and direct `curl` HTTP requests both use the same endpoint
`http://172.29.203.1:8080`.

OpenOCD:

```sh
radxa-linkr-debuggerctl power set 5v_out on
openocd -f interface/<ch347-interface>.cfg -f target/<target>.cfg
```

JTAG/SWD goes through the onboard CH347F path, which is wired directly to the
target debug connector. The RP2040/RP2350 firmware does not act as a debug probe.

Power-output naming intentionally distinguishes controllable 5V outputs from
`5V_FIN`. The firmware does not control `5V_FIN`.

Current schematic mapping (G3 / RP2350A):

- `12v_out`: `GP02_12V_EN` (GPIO 2)
- `5v_out`: `GP05_5V_EN` (GPIO 0)
- `5v_ws`: `GP09_5V_WS_EN` (GPIO 1)
- `20v_out`: `GP10_20V_EN` (GPIO 3)
- TF/SD route switch: `GP06_TF_SW` (GPIO 4)
- USB hub mux switch: `GP03_USB3_HUB` (GPIO 5)
- CH347 1.8 V VIN supply enable: `1V8_EN` (GPIO 6, internal to `switch vin`)
- TF write-protect: `TF_WP` (GPIO 22)
- CH347 VIO voltage select: `VIO_SEL` (GPIO 23, `switch vin`)
- Test point: `TP15` (GPIO 24)
- Status LED: `LED_BLUE` (GPIO 25)
- GPIO aliases: `CON_MAS` (GP7), `CON_REST` (GP8), `CON_USER` (GP9)
- J16 GPIO: `GP10`-`GP20`
- J16 ADC3/GPIO: `GP29` (ADC3)
- ADC current monitor inputs: `S_C_5V` (ADC0), `S_C_12V` (ADC1), `S_C_20V` (ADC2)

VIN defaults to 3.3V at boot. GPIO1 VDD_5V and its GPIO6 VDD_1V8 child rail are
always on in the G3 Device Tree model. The selectable CH347 VIO level is modeled
as a standard `regulator-gpio` regulator with exact 1.8V and 3.3V states, and
firmware selects it through the Zephyr regulator API.

The raw firmware API retains `5v_ws` as a compatibility name for GPIO1 VDD_5V.
The host CLI and TUI intentionally filter this board-internal rail from status,
power lists, and controls. On G3 it remains always on; G2 firmware keeps its
original raw API behavior.

G3 ADC current monitor inputs use an INA139 with a 10 mOhm shunt and a
50 kOhm output load. (G2 uses 51 kOhm.) The Rust CLI and `curl` both report the
firmware's raw current-monitor chain directly; no host-side ADC calibration
tables or zero-point correction are applied.

## Expert: G3 VIN 1.8V Switching

VIN 1.8V switching applies only to G3 (RP2350A) boards and is not available on
G2 (RP2040). This operation is side-effectful and requires confirmed target
voltage compatibility and physical measurement setup before use.

Prerequisites before any 1.8V switch:

1. Confirm your target device's VIO supports 1.8V signaling.
2. Connect a voltmeter or oscilloscope to the target VIO pin.
3. Acknowledge that incorrect voltage will likely damage the target.

To inspect current VIN state:

```sh
curl -fsS http://172.29.203.1:8080/api/v1/switch/vin
radxa-linkr-debuggerctl switch get vin
```

To switch to 1.8V (G3 only), confirm target compatibility first, then:

```sh
radxa-linkr-debuggerctl switch route vin 1.8v --confirm
# Then immediately measure target VIO pin — expect ~1.8V
```

To restore safe 3.3V:

```sh
radxa-linkr-debuggerctl switch route vin 3.3v --confirm
# Then immediately measure target VIO pin — expect ~3.3V
```

CDC ACM shell equivalent (G3 only; returns unavailable on G2):

```text
linkr-debugger:~$ vin get
linkr-debugger:~$ vin set 1.8v
linkr-debugger:~$ vin set 3.3v
```
