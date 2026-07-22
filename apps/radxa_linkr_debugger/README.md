# radxa-linkr-debugger Zephyr App

This directory contains the Zephyr application for `radxa-linkr-debugger`. The root
[README.md](../../README.md) contains workspace setup, flashing, and usage
instructions.

Unless noted otherwise, run the commands below from the repository root.

Agent/AI operators should read the repository skill first:
[skills/radxa-linkr-debugger/SKILL.md](../../skills/radxa-linkr-debugger/SKILL.md).

Build (RP2350):

```sh
pip install -r bootloader/mcuboot/scripts/requirements.txt
west build -p always -b rpi_pico2/rp2350a/m33/mcuboot --sysbuild apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
```

Use `build/radxa_linkr_debugger/` as the only canonical build directory for this
app. RP2350 sysbuild places the MCUboot image at
`build/radxa_linkr_debugger/mcuboot/zephyr/zephyr.hex` and the application
artifacts under `build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/`,
including `zephyr.signed.hex` and `zephyr.signed.bin`. Under this project
configuration, `zephyr.signed.bin` is an unsigned MCUboot-format OTA payload
despite the filename.

Tests:

```sh
./apps/radxa_linkr_debugger/tests/run_unit_tests.sh
```

Schematics:

```text
doc/radxa-linkr-debugger-schematic-x1.1.pdf  (RP2350A)
doc/radxa-linkr-debugger-schematic.pdf       (RP2040 - archival only)
```

The USB interface enumerates as a composite USB device. The board exposes its
HTTP control API on the USB NCM interface at `http://172.29.203.1` by
default. Normal host-side use should prefer the released
`radxa-linkr-debuggerctl` CLI; direct `curl` is mainly for raw HTTP/API checks.
A USB CDC ACM serial port is also kept available for Zephyr cmdline access and
BOOTSEL fallback.

The same HTTP service exposes the embedded production Web UI at
`http://172.29.203.1/`. A clean firmware build requires Node.js 22, npm,
the Rust toolchain, the `wasm32-unknown-unknown` target, and `wasm-bindgen-cli
0.2.121`; CMake builds the Vite application, verifies its fixed asset set, and
stores the gzip-compressed resources in flash. The embedded logic decoder is served at the
stable browser URLs `/assets/decoder/logic-decoder.js` and
`/assets/decoder/logic-decoder_bg.wasm`, with `application/wasm` MIME for the
WASM binary. The project-owned Rust/WASM decoder supports UART, I2C, and SPI
protocols only; it is not a libsigrokdecode Python plugin compatibility layer.

The board also runs a DHCPv4 server on the NCM link so the host can acquire a
compatible address automatically.

For long-lived telemetry and bidirectional control, create a live session over
HTTP first and connect to the returned dedicated WebSocket URL under
`/api/v1/ws/<slot>`. The firmware supports up to four concurrent WebSocket
clients; each live session gets a dedicated slot URL. Subscriptions may request
`batch_size` from 1 through 20, with independent per-client sequence cursors so
one slow subscriber cannot consume another subscriber's samples.
Single-sample ADC telemetry keeps `sequence` and `uptime_us` and also includes
`sample_sequence` plus device-side monotonic `device_t_mono_us`. Compact batch
samples carry `sequence` and `uptime_us`; host clients normalize those values to
the same timing aliases. Single-sample telemetry
includes `dropped_samples` only when the per-client ring skipped one or more
samples. The same session can arm a triggered power capture with a
firmware ring buffer: 2048 samples on RP2350. Manual, current
threshold, allowlisted GPIO edge, and power-output off-to-on triggers are
supported. Triggered capture uses one global hardware buffer and therefore has
only one capture owner at a time. See
[the power analyzer protocol](../../doc/power-analyzer.md).
When HTTP/WS is unavailable but the CDC ACM shell still works, the local shell
command below enters the current MCU's ROM BOOTSEL path used by the HTTP API:

```text
linkr-debugger:~$ bootloader
```

Normal host operations should use the released `radxa-linkr-debuggerctl` CLI.
If you are developing `cmd-ng` itself, use `cargo run --manifest-path
cmd-ng/Cargo.toml -- ...`. Raw HTTP examples below are only for firmware/API
debugging. Full CLI examples are in the root [README.md](../../README.md), and
the [skill](../../skills/radxa-linkr-debugger/SKILL.md) remains the curl-first
Agent workflow.

Raw HTTP checks for firmware/API debugging:

```sh
curl -fsS http://172.29.203.1/api/v1/status
curl -fsS http://172.29.203.1/api/v1/power
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"on"}' \
  http://172.29.203.1/api/v1/power/12v_out
curl -fsS http://172.29.203.1/api/v1/adc/read?channel=5v_out
curl -fsS http://172.29.203.1/api/v1/watchdog
curl -fsS http://172.29.203.1/api/v1/switch
curl -fsS http://172.29.203.1/api/v1/switch/vin   # returns 1.8v or 3.3v
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"3.3v"}' \
  http://172.29.203.1/api/v1/switch/vin   # safe default
```

Raw MCUboot OTA API (RP2350 only; SHA256 only, no signature/authentication/secure boot/anti-rollback):

```sh
# Check OTA state
curl -fsS http://172.29.203.1/api/v1/ota

# Upload MCUboot-format application binary
curl -fsS -X POST \
  -H 'Content-Type: application/octet-stream' \
  -H 'X-Linkr-Ota-Size: <byte_size>' \
  -H 'X-Linkr-Ota-Sha256: <hex_sha256>' \
  --data-binary @/path/to/firmware.bin \
  http://172.29.203.1/api/v1/ota/upload

# Request test boot of verified image
curl -fsS -X POST http://172.29.203.1/api/v1/ota/test

# Manually confirm the running image
curl -fsS -X POST http://172.29.203.1/api/v1/ota/confirm
```

`GET /api/v1/ota` returns the OTA state machine state, expected/written/max byte
sizes, the MCUboot upload area ID, swap type, and whether the current image is
confirmed. OTA upload requires `X-Linkr-Ota-Size` and `X-Linkr-Ota-Sha256` headers.
The test image auto-confirms after a 16-second watchdog health gate; if unconfirmed
and a watchdog reset occurs, the retained marker allows MCUboot rollback instead
of forcing ROM BOOTSEL. Do not use OTA to upload `.uf2` or `.elf` files; use a
MCUboot-format application binary such as the release asset
`radxa-linkr-debugger-rp2350-ota.bin`.

Captive portal discovery: the firmware runs a DHCPv4 server that advertises
router and DNS as `172.29.203.1` and sends DHCP option 114 (Captive Portal URI)
with value `http://172.29.203.1/captive-portal/api`. A DNS responder bound to the
NCM interface on UDP 53 returns wildcard A records pointing to `172.29.203.1`
and NOERROR/NODATA for AAAA queries, with no forwarding or caching. A single HTTP
service bound to the NCM-local `172.29.203.1` address on port 80 routes by URL
path (`/`, `/assets/*`, `/api/v1/*`,
`/api/v1/ws/*`, `/captive-portal/api`, and legacy detection probes). It answers
GET `/captive-portal/api` with `application/captive+json` and a JSON body
carrying the portal URL; all other GET paths not matching the routed paths
return HTTP 302 redirecting to `http://172.29.203.1/`. OS auto-open is best-effort;
results vary by OS version and network configuration. Users can always open
`http://172.29.203.1/` directly or use curl/CLI.

The pinned Zephyr HTTP/1 server short-circuits dynamic-resource `HEAD` requests
before the application callback and returns a default headers-only HTTP 200.
Use `GET` when validating captive portal behavior.

VIN defaults to 3.3V at boot. G3-only VIN 1.8V switching is documented in the
expert gated section below; it requires target voltage compatibility confirmation
and physical measurement setup before use.

`GET /api/v1/status` includes `board_monitoring`, and WebSocket
`snapshot/status` messages include the same object. The categories are
`temperature`, `heap`, `memory`, `runtime`, and `cpu`; each one reports `available`
and a machine-readable `reason`. Values are emitted only when Zephyr exposes a real
device or runtime-stat API. With the default RP2350 configuration,
the board reports internal MCU die temperature, system heap runtime statistics,
real board uptime (`uptime_ms` / `uptime_seconds`), CPU utilization deltas, and
the Phase 2 additive memory pressure objects when those readings are available. The CPU
utilization field can still report `insufficient_runtime_window` until the
firmware has accumulated enough runtime delta to derive a percentage.

The `memory` category carries three pressure-reporting fields:

- `pressure_pct_x100` (legacy root, Phase 1 semantics) is `max(current system heap %, highest thread stack high-water %)` and remains for backward compatibility.
- `current_pressure` is an additive object: `max(current heap %, RX packet slab %, TX packet slab %, RX data buffer pool %, TX data buffer pool %)`. It can rise and fall dynamically and is not total or free RAM.
- `peak_pressure` is a boot-lifetime additive object with the same coverage as `current_pressure` plus thread stack high-water, plus a `since: "boot"` field.

Both `current_pressure` and `peak_pressure` include:
- `available: bool` and `reason: string` (fallback when the data source is absent)
- `pressure_pct_x100: int` in the range 0..10000
- `limiting_component: string` naming the component driving the maximum: `system_heap`, `net_pkt_rx`, `net_pkt_tx`, `net_buf_rx_data`, `net_buf_tx_data`, or `thread_stack` (peak only)
- `limiting_name: string` describing the limiting instance (thread name or pool name)
- `tie_count: int` when multiple components share the maximum value

`physical` reports linker/Kconfig-reserved footprint (`total_bytes`, `image_reserved_bytes`, `reserved_pct_x100`) and is not live occupancy or free RAM. `stacks` reports aggregate high-water values with `thread_count`, `measured_count`, `error_count`, `total_bytes`, `used_high_water_bytes`, `max_pressure_pct_x100`, and `max_pressure_thread`. The root `memory.coverage` keeps the legacy heap/stack meaning; `current_pressure.coverage` and `peak_pressure.coverage` describe the Phase 2 sources instrumented by their respective objects.

Rust and Web clients prefer `current_pressure` when available, fall back to the legacy root `pressure_pct_x100` for Phase 1 compatibility, and fall back again to heap-only when `memory` is absent entirely. Old firmware without `memory` is handled by the Rust CLI falling back to heap-only display, and the Web UI falling back to heap free space. `memory` source is `zephyr` when emitted.

`GET /api/v1/watchdog` reports the autonomous firmware watchdog state. Firmware
itself owns watchdog arming and feeding. If core firmware, API service, or the
CDC ACM cmdline fallback stop reporting healthy liveness, firmware stops
feeding the hardware watchdog and the retained recovery marker drives the next
boot into ROM BOOTSEL. The CDC ACM `bootloader` shell command remains available
as an independent fallback path.

Watchdog rollback is BLOCKED: no safe fault-injection path exists for
validating daemon/service failure scenarios. The current firmware and HIL
confirmation validate WebSocket recovery, autonomous watchdog status reporting,
and CDC ACM `bootloader` fallback into ROM BOOTSEL, but they do not provide a
controlled way to intentionally wedge HTTP/WS/cmdline liveness and prove
automatic watchdog timeout into BOOTSEL without ad hoc destructive methods.
Do not claim rollback behavior was HIL-verified.

Safe GPIO names such as `GP13` are derived from the MCU pin number; the
firmware allowlist keeps the connector note so users can map commands back to
the exposed header position.

Develop the primary Rust host CLI from source:

```sh
cargo build --manifest-path cmd-ng/Cargo.toml
```

For normal use, prefer the released CLI:

```sh
radxa-linkr-debuggerctl status
radxa-linkr-debuggerctl doctor
radxa-linkr-debuggerctl --json status
radxa-linkr-debuggerctl --json adc read
radxa-linkr-debuggerctl adc read -v 5v_out
radxa-linkr-debuggerctl power set 12v_out on
radxa-linkr-debuggerctl power set 20v_out on
radxa-linkr-debuggerctl switch route sd usb-reader
radxa-linkr-debuggerctl switch get vin   # returns 1.8v/3.3v
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

The released Rust CLI and direct `curl` HTTP requests both use the same endpoint
`http://172.29.203.1`.

OpenOCD:

```sh
radxa-linkr-debuggerctl power set 5v_out on
openocd -f interface/<ch347-interface>.cfg -f target/<target>.cfg
```

JTAG/SWD goes through the onboard CH347F path, which is wired directly to the
target debug connector. The RP2350 firmware does not act as a debug probe.

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

G3 GPIO25 operates as a watchdog heartbeat LED, active-low, blinking at roughly
1 Hz. The cycle advances only after a successful hardware watchdog feed. Skipped
or failed feeds reset it to the inactive state while firmware owns the GPIO. This
behavior is driven through Device Tree chosen properties and the existing watchdog
supervisor, not through a Zephyr `CONFIG_LED` or built-in heartbeat driver.
no firmware heartbeat LED.

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
power lists, and controls. On RP2350 it remains always on;
original raw API behavior.

G3 ADC current monitor inputs use an INA139 with a 10 mOhm shunt and a
50 kOhm output load. The Rust CLI and `curl` both report the
firmware's raw current-monitor chain directly; no host-side ADC calibration
tables or zero-point correction are applied.

## Expert: G3 VIN 1.8V Switching

VIN 1.8V switching applies only to G3 (RP2350A) boards and is not available on
This operation is side-effectful and requires confirmed target
voltage compatibility and physical measurement setup before use.

Prerequisites before any 1.8V switch:

1. Confirm your target device's VIO supports 1.8V signaling.
2. Connect a voltmeter or oscilloscope to the target VIO pin.
3. Acknowledge that incorrect voltage will likely damage the target.

To inspect current VIN state:

```sh
curl -fsS http://172.29.203.1/api/v1/switch/vin
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

CDC ACM shell equivalent:

```text
linkr-debugger:~$ vin get
linkr-debugger:~$ vin set 1.8v
linkr-debugger:~$ vin set 3.3v
```
