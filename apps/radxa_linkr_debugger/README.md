# radxa-linkr-debugger Zephyr App

This directory contains the Zephyr application for `radxa-linkr-debugger`.
Workspace setup is documented in the
[developer getting-started guide](../../docs/developer/getting-started.md),
flashing in the [flashing guide](../../docs/developer/flashing.md), and normal
operation in the [user guide](../../docs/user/README.md).

Unless noted otherwise, run the commands below from the repository root.

Agent/AI operators should read the repository skill first:
[skills/radxa-linkr-debugger/SKILL.md](../../skills/radxa-linkr-debugger/SKILL.md).

Build (RP2350):

```sh
make workspace
pip install -r zephyr/scripts/requirements.txt
pip install -r bootloader/mcuboot/scripts/requirements.txt
make firmware
```

Use `build/radxa_linkr_debugger/` as the only canonical build directory for this
app. RP2350 sysbuild places the MCUboot image at
`build/radxa_linkr_debugger/mcuboot/zephyr/zephyr.hex` and the application
artifacts under `build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/`,
including `zephyr.signed.hex` and `zephyr.signed.bin`. Under this project
configuration, `zephyr.signed.bin` is an unsigned MCUboot-format OTA payload
despite the filename.

`sysbuild.cmake` automatically generates both canonical root-level release
artifacts at the end of every build:
`build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2` for combined
MCUboot plus application BOOTSEL flashing, and
`build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350-ota.bin` for OTA.
Flash the combined UF2 for initial install or recovery; it contains both the
bootloader and the application and supports `picotool load -x`.

Tests:

```sh
./apps/radxa_linkr_debugger/tests/run_unit_tests.sh
```

Schematics:

```text
docs/hardware/radxa-linkr-debugger-schematic-x1.1.pdf  (RP2350A)
```

The USB interface enumerates as a composite USB device. The board exposes its
HTTP control API on the USB NCM interface at `http://172.29.203.1` by
default. Normal host-side use should prefer the released
`radxa-linkr-debuggerctl` CLI; direct `curl` is mainly for raw HTTP/API checks.
A USB CDC ACM serial port is also kept available for Zephyr cmdline access and
BOOTSEL fallback.

The same HTTP service exposes the embedded production Web UI at
`http://172.29.203.1/`. A clean firmware build requires Node.js 22, npm,
the Rust toolchain, the `wasm32-unknown-unknown` target, and
`wasm-bindgen-cli 0.2.121`; CMake builds the Vite application, verifies its fixed asset set, and
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
trigger-only state machine. Manual, current threshold, allowlisted GPIO edge,
and power-output off-to-on triggers are supported. Firmware reports the trigger
device timestamp, sample sequence, and telemetry dropped counter; the host
persists the waveform from ADC telemetry. Only one trigger owner is allowed at
a time. See
[the power analyzer protocol](../../docs/reference/power-analyzer.md).

### ADC Telemetry

The firmware exposes four ADC descriptors in a fixed order on both
`GET /api/v1/adc/read` and the live WebSocket telemetry frames
(`topic="adc"` for both single and batch frames):

- `5v_out`, `12v_out`, `20v_out`: `kind="current"`, signed integer
  microamps. `power_enabled` carries the live state of the matching
  `power/<name>` output.
- `adc3`: `kind="voltage"`, signed integer microvolts on GP29
  (`ADC3`). Nominal range 0..3,300,000 µV (0..3.3 V) with no probe
  scaling or host-side calibration.

The HTTP read path stays rich and emits the diagnostic chain (`raw`,
`mv`, `current_ua`, `sensor_value`); for `adc3`, clients reconstruct
signed microvolts from `sensor_value`. HTTP does not emit `voltage_uv`
or `value`. The live WebSocket emits a compact shape: per reading only
`name`, `signal`, `kind`, `unit`, `value` (signed integer in `unit`),
and (current only) `power_enabled`.
There is no dual-emission shim on the WS path; clients that build on
the previous verbose WebSocket shape must update. Host clients must
not apply any ADC calibration table or zero-point correction on
either path.

The batch frame declares channels once (kind, unit per channel) and
then carries `values: i32[]` positionally aligned with channels, one
scalar per channel per sample. `power_enabled_mask` is an unsigned 8-bit
field whose bits map to current channels only (kind="current"); the
voltage channel does not carry a power state. See the authoritative
contract in [docs/reference/adc-telemetry.md](../../docs/reference/adc-telemetry.md).

`adc read adc3` is a thin convenience path used by the host CLI and
Web UI to surface the new voltage reading. The `GET /api/v1/adc/read`
HTTP path remains the rich read; the WebSocket path remains the
compact telemetry.

### GP29 ADC3 Ownership

GP29 (`ADC3`) stays in the persisted/safe catalog but is owned by the
`adc3` voltage monitor on this firmware, so it is **input-only**. Any
`gpio set GP29 ...` output request fails at the firmware layer, and
the host CLI / TUI / Web UI must surface that failure rather than
retrying.

Persistence behaviour for saved snapshots:

- A snapshot that records GP29 as `direction="input"` continues
  to decode and replay normally. The firmware treats an input
  snapshot as the safe default and replays it after firmware
  defaults on the next boot.
- A snapshot that records GP29 as `direction="output"` remains
  decodable so historical boards do not corrupt their snapshots, but
  the replay hits GP29 and stops. Earlier entries stay
  applied; GP29 and later entries stay pending. The host must report
  `apply_state="failed"` for GP29 with no hidden rollback.

Old wording that lists GP29 alongside `GP10`-`GP20` as a freely
drivable safe GPIO is replaced by this contract.
When HTTP/WS is unavailable but the CDC ACM shell still works, the local shell
command below enters the current MCU's ROM BOOTSEL path used by the HTTP API:

```text
linkr-debugger:~$ bootloader
```

Normal host operations should use the released `radxa-linkr-debuggerctl` CLI.
If you are developing `cmd-ng` itself, use `cargo run --manifest-path
cmd-ng/Cargo.toml -- ...`. Raw HTTP examples below are only for firmware/API
debugging. Full CLI examples are in the
[CLI reference](../../docs/user/cli.md), and the
[skill](../../skills/radxa-linkr-debugger/SKILL.md) remains the curl-first Agent
workflow.

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
curl -fsS http://172.29.203.1/api/v1/switch/tf_wp   # returns writable or protected
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"writable"}' \
  http://172.29.203.1/api/v1/switch/tf_wp   # safe default (switch off)
curl -fsS http://172.29.203.1/api/v1/switch/vin   # returns 1.8v or 3.3v
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"3.3v"}' \
  http://172.29.203.1/api/v1/switch/vin   # safe default
```

## Persistent Configuration

The firmware keeps exactly one explicit snapshot of selected control values
that survives reboot, MCUboot OTA, and combined-UF2 recovery. The canonical
design and full client surface are documented in
[docs/reference/persistent-configuration.md](../../docs/reference/persistent-configuration.md); this
section describes the firmware-side contract the clients depend on.

### Storage And Startup

The snapshot lives in the existing `storage_partition` and is stored through
Zephyr `Settings+NVS` at the key `linkr/config/snapshot`. The settings
subsystem initializes only the `linkr/config/snapshot` key, validates the
record through the bounded v1 codec, and reports its state through the
config service. The snapshot header is exactly 12 bytes; byte 4 version 1 is
the only accepted version and byte 7 zero is the only accepted restore
padding. The codec returns `unsupported_version` for any version byte other
than 1; a v1 header with a nonzero byte 7 is rejected as `invalid_snapshot`.
A version byte other than 1 is never replayed, migrated, or auto-cleared. The config service
runs after `linkr_debugger_control_init()` succeeds and before HTTP init,
so the HTTP/WS status snapshot can include the `config` summary at first
contact. Missing data means `absent`; mount, read, corrupt, or
unsupported-version errors leave firmware defaults in place and surface
through the `backend` and
`reason` fields plus the `apply_state` of each affected item. No write path
returns from `main`, delays watchdog startup, erases storage, or
auto-formats. Save replaces the single key; clear calls `settings_delete()`
and is idempotent.

### HTTP Contract

Three endpoints under `http://172.29.203.1` form the authoritative contract.
The full request and response shapes are defined by the firmware, and every
client enumerates item identities from the firmware rather than embedding
them locally.

| Verb | Path | Body | Purpose |
|------|------|------|---------|
| `GET` | `/api/v1/config` | (none) | Read current state, snapshot, pending count, and the firmware-enumerated item catalog |
| `PUT` | `/api/v1/config` | `{"items":["power/12v_out",...],"confirm":false}` | Capture the live values of the selected IDs, persist them as the v1 snapshot, and apply them |
| `DELETE` | `/api/v1/config` | (none) | Idempotent; clears the stored snapshot without altering live hardware |

The only common response fields are `schema`, `ok`, `command`, and `action`.
Successful `get` adds `backend`, `snapshot`, `pending`, and `items`. The
`get` snapshot object reports `version` `1` when a valid snapshot is present
and `null` when no snapshot is present. Each item reports `id`,
`kind` (`power|switch|gpio`), typed `current` (`{state}`, `{route}`, or
`{direction,value}`) or `null` when live state is unavailable, nullable
typed `saved`, `selected`, `requires_confirm` (boolean or `null` when no
saved or live value exists), and `apply_state`
(`not_saved|applied|pending|failed`). Successful `save` adds `saved_items`,
`confirmation_items`, `applied_items`, `snapshot`, and numeric `pending`.
The save `snapshot.version` is `1` for every successful new Save (safe or
confirmed-dangerous). Successful
`clear` adds `noop`, `snapshot`, and numeric `pending`.

Dangerous classification is firmware-owned and applies to `power=on`, every
USB route value, `VIN=1.8v`, and any GPIO entry with the output direction
bit set. A Save that includes any dangerous ID without `confirm:true`
is rejected with `confirmation_required` and lists the offending IDs in
`dangerous_items`. `confirmation_items` belongs to a successful save
response. A partial save stops at the first failed item and reports the
applied set and the still-pending set. Stable error codes: `invalid_json`,
`empty_selection`, `unknown_item`, `duplicate_item`, `confirmation_required`,
`item_unavailable`, `no_snapshot`, `busy`, `body_too_large`,
`backend_unavailable`, `invalid_snapshot`, `unsupported_version`,
`control_capture_failed`, `storage_error`, `storage_write_failed`,
`apply_failed`, `internal_error`.

A `confirmation_required` error lists `dangerous_items`; a `busy` error carries
`activity`; and an `apply_failed` error carries `applied_items`, `failed_item`,
and `pending_items`. The service acquires the
capture owner and then the flash owner once before persisting and replaying.

### Boot Restore And Replay Order

Boot starts from the Device Tree and firmware defaults. After the config
service initializes, every structurally valid v1 snapshot replays every
saved entry, including saved dangerous values, on every normal boot,
because the firmware confirmation that authorized the Save also authorized
future replay. Save persists and applies in the same shared order, so a
confirmed Save takes effect immediately and on every future boot until a
later Save or `config clear` replaces the snapshot. Ordinary control
setters are volatile; only an explicit `config save` writes the snapshot,
and there is no auto-persist path on top of normal control operations.

Save and boot restore share this exact replay order so coupled final state
is preserved:

1. saved GPIO inputs (direction `input`)
2. `switch sd`
3. `switch tf_wp`
4. `switch usb`
5. `power vdd_5v`
6. `power 12v_out`
7. `power 5v_out`
8. `power 20v_out`
9. `switch vin`
10. saved GPIO outputs (direction `output`)

Replay stops at the first hardware failure, leaves earlier effects reported
as applied, and keeps the failed plus remaining entries pending. There is
no rollback, no re-attempt, and no partial hardware write after the stop;
retrying means repeating the confirmed Save, which re-captures live values
and replays them through the same order. A failed Save still persists the
snapshot, so the next boot replays it again. A USB route change is applied
before any explicit `vdd_5v` override so the coupled rail matches the
route; omitting `vdd_5v` from the snapshot preserves the route-driven side
effect.

### CDC ACM Commands

The CDC ACM shell exposes the same operations on its own command surface
through the `config` subcommand. Shell mutations share the firmware-owned
confirmation and busy checks and cannot bypass them. The three verbs are
`config show`, `config save [--confirm] <firmware-item-id>...`, and
`config clear`. The shell uses the same
firmware-enumerated IDs that the HTTP layer exposes, with no host-side
catalog.

<!-- persistent-config-example: app-cdc-config-show -->
```console
linkr-debugger:~$ config show
config available=true reason=ready saved_count=0 pending_count=0
```

<!-- persistent-config-example: app-cdc-config-save-dangerous -->
```console
linkr-debugger:~$ config save --confirm power/12v_out
config save saved_count=1 pending_count=0
```

<!-- persistent-config-example: app-cdc-config-clear -->
```console
linkr-debugger:~$ config clear
config clear hardware_changed=false
```

Stable shell facts:

- `config show` prints one line with `available`, `reason`, `saved_count`,
  and `pending_count`.
- Save without `--confirm` for any dangerous ID prints
  `config save error=confirmation_required` and one
  `confirmation_id=<id>` line per dangerous ID, then exits nonzero.
- A partial save prints `config save error=apply_failed` with
  `failed_id=<id> failed_errno=<n>` for the first failure, then one
  `applied_id=<id>` line per applied ID and one
  `pending_id=<id>` line per still-pending ID.
- `config clear` always reports `hardware_changed=false`; live hardware
  state is unchanged.

The primary result or error line precedes `confirmation_id`, `applied_id`,
`failed_id`, and `pending_id` detail lines. The shell prompt or echo is not a
result, and `config show` proves only `available`, `reason`, `saved_count`, and
`pending_count`; HTTP GET proves item state.

Existing CDC `bootloader`, `vin`, and `tf_wp` commands remain unchanged.
The CDC ACM `bootloader` shell command is the independent fallback when
HTTP/WS is unavailable.

### Capture And OTA Exclusion

Service paths that reach owner arbitration exclude timing-critical logic/sigrok
capture and the MCUboot OTA path. GET never acquires owners. The capture arbiter owns one global
hardware buffer; the flash arbiter holds at most one owner between
`CONFIG` and `OTA`. Concurrent capture or OTA activity returns
`busy activity=capture` or `busy activity=ota` (HTTP 409 `busy`); the
snapshot is not written, no replay runs, and the owner stays held by the
in-flight operation. There is no automatic retry, no host-side queue,
and no silent partial save. The operator must wait for the in-flight
operation to finish and re-issue the request.

### Recovery Boundaries

The snapshot is firmware-owned and survives every recovery path the board
already supports:

- Boot back into defaults after `config clear`.
- Survive reboot, MCUboot OTA test/confirm, and combined-UF2 recovery
  using the canonical `radxa-linkr-debugger-rp2350.uf2` artifact.
- OTA payloads are the separate `radxa-linkr-debugger-rp2350-ota.bin`
  file; the app-only `zephyr.uf2` build output is not a valid ROM
  BOOTSEL image and flashing it through the ROM BOOTSEL path will brick
  the board.
- `DELETE /api/v1/config` and CDC `config clear` delete the stored
  snapshot but never change live hardware.

A stored v1 snapshot, including power-on, USB routes, `VIN=1.8v`, and GPIO
`output` entries, is replayed in full on every normal boot once it survives
the recovery path intact. A new Save after recovery writes a fresh v1
header; a stored blob whose version byte is not 1 is never replayed,
migrated, or auto-cleared.

### Local Versus HIL Validation

The plain-C codec and policy host tests, the firmware service and HTTP
host tests, the Rust client tests, and the Node documentation-contract
checker run on a developer workstation or in CI. They prove the contract,
the lock and busy behavior, the error codes, the replay order, and the
documentation grammar; they do not prove real-board persistence across
reboot, MCUboot OTA, or combined-UF2 recovery. Local validation is not
real-hardware HIL. The 2026-08-05 real-hardware HIL passed the v1
save-and-apply flow; see the
[dated v1-save HIL report](../../docs/testing/results/2026-08-05-persistent-config-v1-save-hil.md).
The historical 2026-07-30 real-hardware HIL passed all six runner flows;
see the [historical six-flow report](../../docs/testing/results/2026-07-30-persistent-config-hil.md).
The HIL procedure lives in `docs/testing/hil-functional-test-spec.md`. Future
local tests remain distinct from board HIL and do not replace a new board
run when hardware behavior changes.

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
The test image auto-confirms after a 16-second watchdog health gate. Firmware is
designed to use the retained marker to request MCUboot rollback after an
unconfirmed-image watchdog reset, but fault-injection HIL for this recovery path
is still blocked. Do not use OTA to upload `.uf2` or `.elf` files; use a
MCUboot-format application binary such as the release asset
`radxa-linkr-debugger-rp2350-ota.bin`.

Captive portal discovery: the firmware runs a DHCPv4 server that assigns the
host a local NCM address without advertising a default router or DNS server, so
connecting the debugger cannot take over the host's Internet route. It sends
DHCP option 114 (Captive Portal URI) with value
`http://172.29.203.1/captive-portal/api`. A single HTTP service bound to the
NCM-local `172.29.203.1` address on port 80 routes by URL
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

**Logic Analyzer Backend**: The sigrok binary protocol runs over WebSocket
(`/api/v1/live-sessions` → `/api/v1/ws/<slot>`) and raw-TCP port 5556; the two
transports are mutually exclusive. Bounded pre=0 and post=1..512 use exact finite
PIO+DMA: trigger NONE is ungated immediate, rising/falling are hardware IRQ-gated,
EITHER snapshots the current pin level in firmware then waits for the opposite edge
(arm-time race exists). Web bounded pre-trigger for rising, falling, and either
uses the prepared packed ring/sink lifecycle under the current per-mode limits.
Post>512 bounded and lower-rate post=0 use ring streaming.
After START_REQ the ordered state progression is: START_RESP with state 2 (ARMED)
or 3 (RUNNING for NONE), then EVENT armed (rising/falling/either only), then EVENT
triggered, then DATA, then EVENT stopped.

All modes use the common packed arena. WIDE11 is the current dual-lane
implementation: its 144184 B hardware burst slice overlays the 149048 B total
backing allocation, which is also shared with the 30720 B WebSocket telemetry
ring. The allocation is sized to `max(normal, burst)=149048 B`; WIDE11 does not
extend it, and WIDE12 is historical only. GP29 is excluded from WIDE11 because
it is owned by ADC3 and input-only; see [GP29 ADC3 ownership](#gp29-adc3-ownership).
See the
[authoritative logic-analyzer architecture and matrices](../../docs/reference/logic-analyzer.md)
and the [dated WIDE11 HIL evidence](../../docs/testing/results/2026-07-27-logic-analyzer-generic-packed-burst-hil.md).
The dated 2026-07-27 report is historical `pre=0` evidence and does not validate
the later pre-trigger implementation.
CDC ACM shell BOOTSEL and combined-UF2 HTTP BOOTSEL recovery were both
confirmed in final validation.

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
- `vdd_5v`: `GP09_5V_WS_EN` (GPIO 1, follows USB mux route)
- `20v_out`: `GP10_20V_EN` (GPIO 3)
- TF/SD route switch: `GP06_TF_SW` (GPIO 4)
- USB hub mux switch: `GP03_USB3_HUB` (GPIO 5)
- CH347 1.8 V VIN supply enable: `1V8_EN` (GPIO 6, internal to `switch vin`)
- TF write-protect: `TF_WP` (GPIO 22, `switch tf_wp`; defaults to `writable` at boot)
- CH347 VIO voltage select: `VIO_SEL` (GPIO 23, `switch vin`)
- Test point: `TP15` (GPIO 24)
- Status LED: `LED_BLUE` (GPIO 25)

G3 GPIO25 operates as a watchdog heartbeat LED, active-low, blinking at roughly
1 Hz. The cycle advances only after a successful hardware watchdog feed. Skipped
or failed feeds reset it to the inactive state while firmware owns the GPIO. This
behavior is driven through Device Tree chosen properties and the existing watchdog
supervisor, not through a Zephyr `CONFIG_LED` or built-in heartbeat driver.

- GPIO aliases: `CON_MAS` (GP7), `CON_REST` (GP8), `CON_USER` (GP9)
- J16 GPIO: `GP10`-`GP20`
- J16 ADC3 / GPIO: `GP29` (ADC3, input-only while ADC3-owned; see
  [GP29 ADC3 ownership](#gp29-adc3-ownership))
- ADC current monitor inputs: `S_C_5V` (ADC0), `S_C_12V` (ADC1), `S_C_20V` (ADC2)

VIN defaults to 3.3V at boot. GPIO1 VDD_5V is coupled to the USB mux route
(`pc` on, `target` off) and boots off under the default `target` route, while
its GPIO6 VDD_1V8 child rail stays always on in the G3 Device Tree model. The
selectable CH347 VIO level is modeled as a standard `regulator-gpio` regulator
with exact 1.8V and 3.3V states, and firmware selects it through the Zephyr
regulator API.

GPIO1 VDD_5V is exposed as the `vdd_5v` power output across the raw API, host
CLI/TUI, and Web UI. It powers the USB hub domain and follows the `switch usb`
route (`pc` on, `target` off); manual `power set` is honored between route
changes and re-imposed on the next route change. Turning it off also cuts the
GPIO6 VDD_1V8 child rail used for CH347 1.8 V VIN.

G3 ADC current monitor inputs use an INA139 with a 10 mOhm shunt and a
50 kOhm output load. The Rust CLI and `curl` both report the
firmware's raw current-monitor chain directly; no host-side ADC calibration
tables or zero-point correction are applied.

## Expert: G3 VIN 1.8V Switching

VIN 1.8V switching applies to the supported G3 (RP2350A) hardware. This
operation is side-effectful and requires confirmed target voltage compatibility
and physical measurement setup before use.

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
