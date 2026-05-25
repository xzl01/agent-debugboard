# agent-debugboard Zephyr App

This directory contains the Zephyr application for `agent-debugboard`. The root
[README.md](../../README.md) contains workspace setup, flashing, and usage
instructions.

Agent/AI operators should read the repository skill first:
[skills/agent-debugboard/SKILL.md](../../skills/agent-debugboard/SKILL.md).

Build:

```sh
west build -p always -b rpi_pico/rp2040 apps/agent_debugboard -d build/agent_debugboard
```

Use `build/agent_debugboard/` as the only canonical build directory for this
app, and use `build/agent_debugboard/zephyr/zephyr.uf2` as the only canonical
flash artifact.

Tests:

```sh
./apps/agent_debugboard/tests/run_unit_tests.sh
```

Schematic:

```text
doc/agent-debugboard-schematic.pdf
```

The USB interface enumerates as a composite USB device. The board exposes its
HTTP control API on the USB NCM interface at `http://172.29.203.1:8080` by
default. The host CLI (`agent-debugboardctl`) and direct `curl` requests both
use this endpoint. A USB CDC ACM serial port is also kept available for Zephyr
cmdline access and BOOTSEL fallback.

The board also runs a DHCPv4 server on the NCM link so the host can acquire a
compatible address automatically.

For long-lived telemetry and bidirectional control, multi-client live use should
create a live session over HTTP first and connect to the returned dedicated
WebSocket URL under `/api/v1/ws/<slot>`; the firmware keeps the HTTP client
limit aligned with the four live WebSocket slots.
When HTTP/WS is unavailable but the CDC ACM shell still works, the local shell
command below enters the same RP2040 ROM BOOTSEL path used by the HTTP API:

```text
debugboard:~$ bootloader
```

Useful CLI commands:

```sh
agent-debugboardctl --json status
agent-debugboardctl --json power list
agent-debugboardctl --json power set 12v_out on
agent-debugboardctl --json power set 12v_out off
agent-debugboardctl --json power set 5v_out on
agent-debugboardctl --json power set 5v_out off
agent-debugboardctl --json power set 5v_ws on
agent-debugboardctl --json power set 5v_ws off
agent-debugboardctl --json power set 20v_out on
agent-debugboardctl --json power set 20v_out off
agent-debugboardctl --json adc read
agent-debugboardctl --json adc read 5v_out
agent-debugboardctl --json adc read -v 5v_out
agent-debugboardctl --json switch list
agent-debugboardctl --json switch get sd
agent-debugboardctl --json switch get usb
agent-debugboardctl --json switch route sd target
agent-debugboardctl --json switch route usb target
agent-debugboardctl --json gpio list
agent-debugboardctl --json gpio get GP13
agent-debugboardctl --json gpio set GP13 1
agent-debugboardctl --json gpio input GP13
agent-debugboardctl --json watchdog status
agent-debugboardctl --json bootloader
```

Equivalent direct HTTP checks with `curl`:

```sh
curl -fsS http://172.29.203.1:8080/api/v1/status
curl -fsS http://172.29.203.1:8080/api/v1/power
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"on"}' \
  http://172.29.203.1:8080/api/v1/power/12v_out
curl -fsS http://172.29.203.1:8080/api/v1/adc/read?channel=5v_out
curl -fsS http://172.29.203.1:8080/api/v1/watchdog
```

`GET /api/v1/status` includes `board_monitoring`, and WebSocket
`snapshot/status` messages include the same object. The categories are
`temperature`, `heap`, `runtime`, and `cpu`; each one reports `available` and a
machine-readable `reason`. Values are emitted only when Zephyr exposes a real
device or runtime-stat API. With the default RP2040 configuration, the board
reports internal RP2040 CPU die temperature, system heap runtime statistics,
real board uptime (`uptime_ms` / `uptime_seconds`), and CPU utilization deltas
when those readings are available. The CPU utilization field can still report
`insufficient_runtime_window` until the firmware has accumulated enough runtime
delta to derive a percentage.

`GET /api/v1/watchdog` reports the autonomous firmware watchdog state. Firmware
itself owns watchdog arming and feeding. If core firmware, API service, or the
CDC ACM cmdline fallback stop reporting healthy liveness, firmware stops
feeding the RP2040 watchdog and the retained recovery marker drives the next
boot into ROM BOOTSEL. The CDC ACM `bootloader` shell command remains available
as an independent fallback path.

FIXME: add a safe fault-injection path for validation of daemon/service failure
scenarios. The current firmware and manual validation confirm websocket
recovery, autonomous watchdog status reporting, and CDC ACM `bootloader`
fallback into RP2040 BOOTSEL, but they do not yet provide a controlled way to
intentionally wedge HTTP/WS/cmdline liveness and prove automatic watchdog
timeout into BOOTSEL without using ad hoc destructive test methods.

Safe GPIO names such as `GP13` are derived from the RP2040 pin number; the
firmware allowlist keeps the connector note so users can map commands back to
the exposed header position.

Build the host CLI:

```sh
go build -o agent-debugboardctl ./cmd/agent-debugboardctl
```

Host helper:

```sh
./agent-debugboardctl status
./agent-debugboardctl doctor
./agent-debugboardctl --json status
./agent-debugboardctl adc read
./agent-debugboardctl adc read -v 5v_out
./agent-debugboardctl power set 12v_out on
./agent-debugboardctl power set 20v_out on
./agent-debugboardctl switch route sd usb-reader
./agent-debugboardctl watchdog status
```

OpenOCD:

```sh
./agent-debugboardctl --json power set 5v_out on
openocd -f interface/<ch347-interface>.cfg -f target/<target>.cfg
```

JTAG/SWD goes through the onboard CH347F path, which is wired directly to the
target debug connector. The RP2040 firmware does not act as a debug probe.

Power-output naming intentionally distinguishes controllable 5V outputs from
`5V_FIN`. The firmware does not control `5V_FIN`.

Current schematic mapping:

- `12v_out`: `GP02_12V_EN`
- `5v_out`: `GP05_5V_EN`
- `5v_ws`: `GP09_5V_WS_EN`
- `20v_out`: `GP10_20V_EN`
- TF/SD route switch: `GP06_TF_SW`
- ADC current monitor inputs: `S_C_5V`, `S_C_12V`, `S_C_20V`

All ADC current monitor inputs use an INA139 with a 10 mOhm shunt and a
51 kOhm output load. `agent-debugboardctl` now reports the firmware's raw
current-monitor chain directly and no longer applies host-side ADC calibration
tables or zero-point correction.
