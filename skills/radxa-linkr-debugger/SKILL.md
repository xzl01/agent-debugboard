---
name: radxa-linkr-debugger
description: Use the local Radxa Linkr Debugger MCP server, curl, or the optional CLI to inspect and operate target power, ADC, routing and shared UART sessions, with explicit fallback workflows for watchdog recovery and RP2350 BOOTSEL over USB NCM/CDC.
---

# Radxa Linkr Debugger

The active hardware target is G3 with RP2350A. G2/RP2040 is retired and must
not be built, flashed, or treated as a supported fallback. RP2354 requires a
dedicated board definition and HIL validation before use with this skill.

Prefer the installed Host's resident Streamable HTTP MCP endpoint at
`http://127.0.0.1:8787/mcp` for Agent-side status, ADC, confirmed power/route
control, and target UART work when its tools are available. The unified
installer runs it under `linkr-tray`; `linkr-host mcp` remains the stdio
compatibility path. MCP shares the host Serial Broker with the Web UI and provides
cursor-based reads, bounded waits, and short exclusive writes. See
`./doc/mcp-server.md` from the repository root.

The tray enables Host-managed raw UART RX archiving. Bridge/MCP UART sessions
are stored as `linkr-serial-log.v1` under the user's application-data directory;
direct Web Serial is not included. The canonical artifact is `rx-*.bin`, while
`rx-*.ndjson` is only its timestamp/offset index. Treat any manifest with
`complete: false` or non-zero `dropped_bytes` as incomplete. Never infer that a
decoded terminal view is a byte-exact archive. TX is intentionally not stored
because login credentials or other secrets may be present.

On Unix, Host explicitly requests exclusive mode on the serial builder so the
TTY is reserved atomically during open. Do not call `set_exclusive(true)` again
after the open succeeds:
the macOS CH347 driver returns `EBUSY` for a repeated `TIOCEXCL` even when the
same process already owns the port.

Use direct HTTP requests with `curl` as the lowest-common-denominator fallback.
The board enumerates as a USB NCM network interface and exposes its control API
at the default device URL `http://172.29.203.1`.

Production firmware also serves its embedded Web control panel from the root of
that URL. The page talks to the same-origin `/api/v1` HTTP and WebSocket paths.
MCP is the preferred Agent adapter when available; curl remains deterministic
and machine-readable when the adapter is not installed. The HTTP listener is
bound to the NCM-local `172.29.203.1` address rather than every network
interface.

The board also runs a DHCPv4 server on the NCM link so the host can acquire a
compatible IPv4 address automatically. mDNS is not required for the normal
workflow.

`curl` remains the lowest-common-denominator path across macOS, Linux, and
Windows. The host CLI/TUI path in this repository is the Rust implementation
under `./cmd-ng/`. The RP2350 USB CDC ACM port is intentionally kept as a
secondary path for Zephyr cmdline access and BOOTSEL fallback.

For long-lived telemetry and bidirectional control, the firmware also exposes a
live-session workflow: create a live session over HTTP, then connect to the
returned dedicated websocket URL under `/api/v1/ws/<slot>`.
The interactive TUI is expected to close only its own websocket session
explicitly when it exits; unused sessions should expire automatically in
firmware. The firmware supports up to four concurrent websocket clients via
dedicated slot URLs under `/api/v1/ws/<slot>`. If you rebuild the CLI after
websocket lifecycle fixes, verify repeated open/close cycles and concurrent
subscriber behavior with the freshly built skill-local binary.

> **Agent automation rule**: use the `linkr_*` MCP tools first when they are
> installed. Reuse serial `next_cursor` values and prefer
> `linkr_serial_command` over repeated full-log reads. Use the default compact
> `linkr_board_status` result unless GPIO or complete monitoring diagnostics are
> needed, then pass `detail: "full"`. Fall back to `curl` when
> MCP is unavailable, and use the CLI for the interactive TUI, `doctor`, or
> advanced batch workflows. OTA, BOOTSEL, EDL/MASKROM and arbitrary GPIO
> mutation are intentionally outside MCP v1 and must use the explicit
> workflows below.

## MCP Fast Path

The primary installed MCP entry is the resident
`http://127.0.0.1:8787/mcp` endpoint supervised by `linkr-tray`. From a source
checkout, build it once with
`cargo build --release --manifest-path ./host-tools/Cargo.toml`; the command
`./host-tools/target/release/linkr-host serve` exposes Web, Broker, and MCP in
one loopback daemon. `./host-tools/target/release/linkr-host mcp` is retained
for stdio-only clients; it completes the MCP handshake immediately, then starts
and supervises the loopback Host in the background.
Temporary Host startup failures use bounded exponential retry and must not be
treated as a permanently disabled MCP server. Client configuration and the
complete tool contract are in `./doc/mcp-server.md`. The Node adapter is
migration-only and must not be preferred for new installations.

- Read-only: `linkr_board_status`, `linkr_adc_read`, `linkr_serial_status`,
  `linkr_serial_read`, `linkr_serial_expect`.
- Confirmed hardware state: `linkr_power_set`, `linkr_switch_route`; always pass
  `confirm: true` only after checking the requested rail/route and target.
- Shared UART: call `linkr_serial_connect`, then use cursors returned by
  `linkr_serial_read`/`linkr_serial_expect`. Use `linkr_serial_login` for target
  login without returning the password. Prefer `linkr_serial_shell_command`
  when a POSIX shell exit code is needed; keep `linkr_serial_command` for
  bootloaders and non-shell consoles. Commands and login claim the channel only
  for their operation and release it automatically. Shell-command exit probes
  are private Broker bookkeeping: the MCP owner receives them for parsing, but
  shared Web terminals and logs receive only the target command/output.
- Cleanup: call `linkr_serial_disconnect` when the Agent no longer needs the
  subscription. Other Web/Agent subscribers remain connected.
- Recovery: when a read-only tool returns `host_temporarily_unavailable`, honor
  `error.details.retry_after_ms` and retry only when
  `error.details.retryable` is true. Never automatically replay power, route or
  serial-write operations. A closed cached Serial Broker connection is replaced
  on the next tool call while preserving monotonic channel cursor checkpoints.
  Treat `serial_cursor_expired` and `serial_cursor_ahead` as reset boundaries;
  resume only from the returned earliest/latest cursor after checking context.
- Visibility: provide an MCP progress token for serial expect, login and command
  calls. The server emits start, one-second heartbeat and completion events
  without exposing passwords or full commands. If a client does not display MCP
  progress, use the final structured result and stderr diagnostics.

MCP results use `radxa-linkr-debugger.mcp.v1`; embedded board responses retain
`radxa-linkr-debugger.v1`. Treat `isError`, `error.code`, and invalid serial
cursors as structured control flow, not as text to scrape.

The examples below assume this skill is checked into the current repository at
`./skills/radxa-linkr-debugger` and commands are run from the repository root. If
the skill is installed elsewhere, for example under `.claude/skills`, replace
the `./skills/radxa-linkr-debugger` prefix with the actual skill directory. Do not
use `./skills/...` from another repository unless that repository contains this
skill at that path.

- Default device URL: `http://172.29.203.1`
- Optional CLI binary (macOS/Linux): `./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl`
- Optional CLI binary (Windows): `./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl.exe`
- Primary Host/MCP binary from a source checkout: `./host-tools/target/release/linkr-host`
- Unified source installer: `./scripts/install-host.sh` or `./scripts/install-host.ps1`
- Installed MCP endpoint: `http://127.0.0.1:8787/mcp`

## Repository Change Rules

When an agent changes files in this repository, follow `./AGENTS.md` and keep
this skill aligned with user-facing docs.

- Any code change must update the corresponding skill and documentation in the
  same change.
- If firmware behavior or host CLI logic changes, update the
  related skill/docs and run the relevant tests before finishing.
- For commit tasks changing `cmd-ng` Cargo dependency inputs (`Cargo.toml` or
  `Cargo.lock`) or Nix packaging, follow `./AGENTS.md`: refresh `cargoHash` in
  `nix/package.nix` from a native Nix build's reported hash when needed and run
  `nix flake check -L` before finishing.
- If firmware changes, verify and preserve the USB CDC ACM serial BOOTSEL
  fallback path before finishing.
- If this skill or another repo skill changes, run a subagent validation/test
  before finishing.
- When adding new functionality, add corresponding functional tests whenever
  practical.

## Canonical Build and Flash Paths

For this repository, the firmware build and flash locations are fixed:

- Canonical build directory: `./build/radxa_linkr_debugger/`
- Combined MCUboot+app UF2 (safe for BOOTSEL): `./build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`
- App-only UF2 (not for BOOTSEL flashing): `./build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.uf2`
- RP2350 OTA release asset: `radxa-linkr-debugger-rp2350-ota.bin`

The build system auto-generates both UF2 artifacts via a post-build step. For
ROM BOOTSEL flashing, always use the combined UF2 because it includes MCUboot
and the application. Do not flash the app-only `zephyr.uf2` through ROM BOOTSEL;
it does not install the bootloader and can brick the board.

When an agent builds firmware, always use that exact build directory. For RP2350
sysbuild, the MCUboot hex is under
`./build/radxa_linkr_debugger/mcuboot/zephyr/zephyr.hex`, and the application
artifacts are under `./build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/`.
The application `zephyr.signed.bin` is an unsigned MCUboot-format OTA payload
under this project config despite the filename. Do not switch to alternate build
directories and do not flash stale artifacts copied somewhere else, such as a
temporary mount point.

The canonical firmware build includes the Web UI and requires Node.js 22 plus
npm. CMake runs the locked frontend build and embeds gzip-compressed assets in
flash; do not bypass that step with stale files from `web/dist`. Before an
RP2350 sysbuild, install MCUboot's image-tool dependencies with
`scripts/setup-zephyr.sh`, then run
`pip install -r .zephyr-workspace/bootloader/mcuboot/scripts/requirements.txt`
in the active workspace Python environment. Use `scripts/build-firmware.sh` for
the canonical full build.

## First Checks

1. Confirm that curl is available.

   macOS/Linux:

   ```sh
   curl --version
   ```

   Windows PowerShell or CMD:

   ```powershell
   curl.exe --version
   ```

2. Confirm that the board answers on the default HTTP endpoint.

   macOS/Linux:

    ```sh
     curl -fsS http://172.29.203.1/api/v1/status
    ```

   Windows PowerShell:

   ```powershell
    curl.exe -fsS http://172.29.203.1/api/v1/status
   ```

   For interactive browser use, open `http://172.29.203.1/`. The embedded
   page controls the board without a gateway for normal HTTP/WS board features.
   Its target serial panel has two supported paths:

    - **Override path** (direct CH347 Web Serial): after manually adding
        `http://172.29.203.1` to
       `chrome://flags/#unsafely-treat-insecure-origin-as-secure`, relaunching
       the browser, and reopening the board page. Edge accepts this Chromium
       address. Ordinary web pages cannot navigate to browser-internal URLs; the
       address must be copied and pasted into the address bar. The board-hosted
       setup dialog presents a three-step tutorial: copy the flag URL, copy the
       exact origin, then enable the flag and relaunch. Both address surfaces
       are independent copy buttons with their own feedback. Copy success only
       confirms the text was placed in the clipboard; it does not confirm a
       working serial connection. The only Web Serial action is beside
       **Bridge** in each visible UART pane; the card does not render a separate
       serial guidance control. The chooser still appears when **Web Serial** is
       clicked and must be accepted. Because the board page is
       served over HTTP, the Clipboard API may not be available in all browser
       contexts; the copy controls must use an HTTP-compatible fallback when
       that API is unavailable. If copying still fails, the full address remains
       visible for manual selection. The modal dialog
       carries `role="dialog"` and `aria-modal="true"`, traps initial focus
       inside, contains Tab/Shift+Tab navigation within the dialog, closes on
       Escape with focus restored to the trigger element, and restores body
       scroll on close. This override is experimental and weakens origin
       security for that page; it does not remove the user gesture or chooser
       requirement.
   - **Bridge fallback**: when the override is not enabled or not available,
       keep the board page open, run `npm run build && npm run host` from
       `web/` in a separate terminal, and use the page's **Bridge** button.

   When testing the board-hosted UI with Playwright, distinguish between two
   failure modes. A `page.goto` failure or resource-load timeout points to a
   board, NCM, HTTP server, or browser setup problem. An assertion failure after
   the page loads points to a UI regression. The insecure-context test must
   confirm that the red button opens the setup dialog without requesting a
   serial port. The override-active test must confirm that the button uses the
   direct Web Serial path. Actual chooser display, manual CH347 selection, and
   serial I/O remain manual HIL because the chooser is a mandatory browser
   security mechanism and cannot be bypassed programmatically.

3. Treat these outcomes as follows:
   - Exit code `0` with valid JSON and `ok: true`: the board is ready.
   - Valid JSON with `ok: false`: read `error.code` and `error.message`; HTTP transport works, but the board rejected the operation.
   - Connection refused / timeout / no route: the board NCM link or address needs attention.
   - `curl` missing: install curl or use the optional repo-local CLI as a fallback.

## Optional: Install/Build Repo-Local CLI

This skill uses only repo-local scripts and binaries. Do not modify `PATH`,
shell profiles, or global install directories.

Install the CLI into the repo-local output directory when you want the TUI,
`doctor`, or the convenience wrapper around the HTTP API.

macOS/Linux:

```sh
./skills/radxa-linkr-debugger/scripts/install.sh
```

Install a specific release version. An explicit version always skips local
source builds and downloads the requested primary Rust CLI release artifact:

```sh
./skills/radxa-linkr-debugger/scripts/install.sh --version <tag>
```

Windows PowerShell:

```powershell
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\skills\radxa-linkr-debugger\scripts\install.ps1
```

Windows PowerShell specific release version:

```powershell
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\skills\radxa-linkr-debugger\scripts\install.ps1 -Version <tag>
```

After installation, run the matching binary. The release download path uses
`radxa-linkr-debuggerctl-rust_<os>_<arch>.*` archives.

macOS/Linux:

```sh
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --version
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --json doctor
```

Windows CMD:

```bat
.\skills\radxa-linkr-debugger\scripts\bin\radxa-linkr-debuggerctl.exe --version
.\skills\radxa-linkr-debugger\scripts\bin\radxa-linkr-debuggerctl.exe --json doctor
```

## Build Primary Rust cmd-ng

The repository's primary host CLI/TUI path is the Rust `cmd-ng`
implementation.

macOS/Linux/Windows with Rust installed:

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- --help
cargo run --manifest-path cmd-ng/Cargo.toml -- --json status
cargo run --manifest-path cmd-ng/Cargo.toml --
```

The Rust tool keeps the same default URL (`http://172.29.203.1`) and still expects the `radxa-linkr-debugger.v1` JSON envelope. Running it without a subcommand starts the primary TUI, which polls HTTP status/ADC endpoints and keeps power, SD route, and GPIO controls in one adaptive grid.

The board-internal VDD_5V rail is exposed as the `vdd_5v` power output in
CLI/TUI status, power lists, controls, and the Web UI. It powers the USB hub
domain and follows `switch usb`: routing to `pc` turns it on, routing to
`target` turns it off. Manual `power set vdd_5v` is allowed between route
changes and is re-imposed on the next route change. At boot the mux defaults to
`target` and `vdd_5v` is off. Turning it off also cuts the VDD_1V8 child rail
used for CH347 1.8 V VIN.

## JSON Contract

Agent automation should expect the top-level fields:

- `schema`: must be `radxa-linkr-debugger.v1`
- `ok`: boolean success flag
- `command`: command name
- `error`: present on failure, with `code` and `message`

If `ok` is `false`, do not infer success from partial fields. Handle
`error.code` first.

`GET /api/v1/status` and WebSocket `snapshot/status` messages include the same
`board_monitoring` object. Its `temperature`, `heap`, `memory`, `runtime`, and `cpu`
members each report `available` and a machine-readable `reason`. Treat
`available: false` as authoritative; the firmware does not invent sensor,
memory, runtime, or CPU values when Zephyr has no reliable source enabled. On
the default RP2350 configuration, the board should report internal CPU die
temperature, system heap runtime statistics, real board uptime (`uptime_ms` /
`uptime_seconds`), CPU utilization deltas, and the Phase 2 additive memory pressure objects.
The CPU percentage can still temporarily report `insufficient_runtime_window`
until enough runtime delta has been collected.

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

Rust and Web clients prefer `current_pressure` when available, fall back to the legacy root `pressure_pct_x100` for Phase 1 compatibility, and fall back again to heap-only when `memory` is absent entirely. Old firmware without `memory` is handled by the Rust CLI falling back to heap-only display and the Web UI falling back to heap free space. `memory` source is `zephyr` when emitted.

For short reset debugging, the watchdog supervisor also prints periodic memory
diagnostics in the firmware log. These lines prioritize heap allocated, free,
total, and peak bytes from the system heap plus real uptime. Treat them as
side-effect-free log output only: they do not feed the watchdog, do not advance
the CPU utilization sampling window, and do not change watchdog behavior.

The same 1 Hz diagnostics cadence also emits a watchdog trace line. Use it to
see whether the supervisor is still alive, whether hardware watchdog feeding was
`ok`, `failed`, or `skipped`, and which liveness source (`core`, `api`, or
`cmdline`) is currently blocking feeding. Treat this as log-only debug output;
it does not itself change watchdog behavior.

Firmware also logs the reset cause at boot and USB device lifecycle events
during runtime. When watchdog recovery enters ROM BOOTSEL, the boot message
distinguishes between an explicit bootloader command and an unhealthy liveness
stop.

## Persistent Configuration

Use the raw HTTP API first. The default remains the portless NCM URL
`BOARD_URL="${BOARD_URL:-http://172.29.203.1}"`. Persistent configuration stores
one explicit snapshot on a single v1 wire format. Ordinary control
setters remain volatile, so changing a power output, switch, or GPIO does
not save that change. Every successful Save writes the v1 snapshot and
applies it immediately; every structurally valid v1 snapshot replays every
saved entry, including dangerous values, on every future normal boot. A
stored blob whose version byte is not 1 is never replayed, migrated, or
auto-cleared. The full model and API contract are in
[Persistent Configuration](../../doc/persistent-configuration.md).

### Read Saved Configuration

Always read before saving or clearing. `GET /api/v1/config` is the
only source for item IDs and danger classification. Do not keep a board catalog
or infer risk from an ID, kind, current value, or prior firmware release. Only
firmware classifies danger through `requires_confirm`.

<!-- persistent-config-example: skill-curl-config-read -->
```sh
BOARD_URL="${BOARD_URL:-http://172.29.203.1}"
curl -fsS "$BOARD_URL/api/v1/config"
```

Require `schema: "radxa-linkr-debugger.v1"`, `command: "config"`,
`action: "get"`, and `ok: true` before using the result. Inspect `backend`,
`snapshot`, and `pending`, then review every `items` row. The
`snapshot.version` reports `1` when a valid snapshot is present and `null`
when no snapshot is present. Each row reports
`id`, `kind`, `current`, `saved`, `selected`, `requires_confirm`, and
`apply_state`. An unavailable current value or unknown danger
classification is not permission to save that item.

### Save Selected Current Values

Save accepts firmware item IDs, captures their current values into the v1
snapshot, and applies them immediately. A successful response reports
`saved_items`, `confirmation_items`, `applied_items`, `snapshot`, and
`pending`. For a safe item, first verify that the current GET
response contains the exact ID and `requires_confirm: false`, then send
`confirm: false`. The `switch/sd` ID below is an executable contract example,
not a catalog to reuse without that check.

<!-- persistent-config-example: skill-curl-config-save-safe -->
```sh
BOARD_URL="${BOARD_URL:-http://172.29.203.1}"
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"items":["switch/sd"],"confirm":false}' \
  "$BOARD_URL/api/v1/config"
```

A danger-classified current value may be saved only after a person reviews the
GET result and explicitly accepts that exact value. Check that the row has the
exact ID and `requires_confirm: true`, then send the confirmed request. Never
turn an earlier `confirmation_required` response into confirmation
automatically.

<!-- persistent-config-example: skill-curl-config-save-dangerous -->
```sh
BOARD_URL="${BOARD_URL:-http://172.29.203.1}"
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"items":["switch/usb"],"confirm":true}' \
  "$BOARD_URL/api/v1/config"
```

The confirmed Save does much more than change the live state: it writes
the v1 snapshot and applies the saved value immediately, and the stored
snapshot authorizes the firmware to replay it on every future
normal boot until a later Save or `config clear` replaces the snapshot.
Review the impact: a confirmed Save of `power/12v_out on`, a non-default
USB route, `VIN=1.8v`, or any GPIO `output` value takes effect immediately
and becomes the board's default power, route, voltage, or GPIO state on
every reboot until the user re-saves or clears. The Save-time firmware
confirmation is the only danger gate; boot replays the stored v1 snapshot
in full without asking again. A partially failed Save reports
`apply_failed` with `applied_items`, `failed_item`, and `pending_items`;
the snapshot is still stored, the next boot replays it again, and the
retry path is repeating the confirmed Save after inspecting live state.
There is no hidden config rollback.

### Clear Without Changing Hardware

Clear deletes the saved snapshot only. It does not restore defaults, reverse a
save or boot replay, or change any live output, route, or GPIO. The response summary contains
`noop`, `snapshot.present: false`, `snapshot.version: null`, and `pending: 0`.

<!-- persistent-config-example: skill-curl-config-clear -->
```sh
BOARD_URL="${BOARD_URL:-http://172.29.203.1}"
curl -fsS -X DELETE "$BOARD_URL/api/v1/config"
```

After clearing, read the config again and check the normal hardware status
separately. Status and WebSocket summaries expose `available`, `reason`,
`saved_count`, and `pending_count`; these are summaries, not proof that hardware
changed. The CLI clear summary says `current hardware unchanged`, and CDC
prints `config clear hardware_changed=false`.

### Handle Confirmation And Busy Errors

Treat `ok` as authoritative. If it is false, stop success handling and parse
`error.code` plus `error.message`, even when transport succeeded. Preserve the
JSON body for non-2xx responses rather than relying only on curl's exit code.

<!-- persistent-config-example: curl-config-save-dangerous-unconfirmed -->
```sh
set +e
response="$(curl --fail-with-body -sS -X PUT http://172.29.203.1/api/v1/config -H 'Content-Type: application/json' --data '{"items":["switch/usb"],"confirm":false}')"
curl_status=$?
set -e
[ "$curl_status" -eq 22 ]
printf '%s\n' "$response"
```

The expected HTTP 409 body remains in `response`; parse its `error.code`,
`error.message`, and `dangerous_items`, then compare those IDs with a fresh
HTTP GET. Do not auto-confirm or replay the request from this error path.

For `confirmation_required`, inspect `dangerous_items`. Do not add confirmation
and resubmit automatically. A person must compare those IDs with a fresh GET,
review the current values, and choose whether to issue a new confirmed request.
For `busy`, inspect `activity`, which is `capture` or `ota`. Do not retry on a
timer. Leave the config operation stopped; after an operator ends or completes
the named activity, begin again with GET.

Other errors such as `item_unavailable`, `no_snapshot`, `backend_unavailable`,
`invalid_snapshot`, `unsupported_version`, `storage_error`,
`storage_write_failed`, and `control_capture_failed` also stop the workflow.
For `apply_failed`, preserve and report `applied_items`, `failed_item`, and
`pending_items` because the result can be partial. Never infer full failure or
full success from the HTTP status alone.

### CLI And CDC Fallback

Use the Rust CLI only when curl is unavailable or CLI-specific output is needed.
Its grammar is `config show`, `config save [--confirm]
<firmware-item-id>...`, and `config clear`. Keep
`--json` enabled for automation and parse the same `ok` and `error.code`
contract.

<!-- persistent-config-example: skill-cli-config-show -->
```sh
radxa-linkr-debuggerctl --json config show
```

<!-- persistent-config-example: skill-cli-config-save-safe -->
```sh
radxa-linkr-debuggerctl --json config save switch/sd
```

<!-- persistent-config-example: skill-cli-config-save-dangerous -->
```sh
radxa-linkr-debuggerctl --json config save --confirm switch/usb
```

<!-- persistent-config-example: skill-cli-config-clear -->
```sh
radxa-linkr-debuggerctl --json config clear
```

The CLI and curl examples intentionally use unique marker IDs. First discover
the real IDs and firmware risk flags with `config show`; never copy the sample
IDs into an unrelated board workflow. The CLI does not auto-confirm a failed
request.

If NCM HTTP is unavailable but USB CDC ACM still responds, use the equivalent
Zephyr shell grammar. CDC reports compact summaries and machine-readable error
tokens, including confirmation IDs, busy activity, and partial save IDs.

<!-- persistent-config-example: skill-cdc-config-fallback -->
```console
linkr-debugger:~$ config show
linkr-debugger:~$ config save <firmware-item-id>...
linkr-debugger:~$ config save --confirm <firmware-item-id>...
linkr-debugger:~$ config clear
```

### Automatic Current Synchronization

The Web Saved Config panel keeps its `Current` column aligned with the
board without a manual reload. Current is firmware-authoritative data from
`/api/v1/config`; observed changes to power `state`, switch `route`, or
allowlisted GPIO `direction/value` trigger an automatic refresh of
Current. Names and identifiers are supplied by firmware, not a
host-side catalog.

Identical, reordered, or unrelated status or WebSocket frames do not cause
additional config GETs; one actual relevant value transition produces one
Current refresh, and high-rate status or WS polling does not flood the
firmware. The automatic refresh does not write flash, change the saved
snapshot, replay saved values, or auto-persist ordinary power, switch,
or GPIO setters; `config save` stays the only persistence path; ordinary
volatile setters stay volatile.

Local unsaved item-selection checkbox drafts on the Saved Config panel
survive ordinary Current synchronization, so the operator's selected set
is preserved when the upstream value changes.

`Refresh` is a manual recovery or retry action after a transient failed
request or suspected stale UI. It is not a required normal step; ordinary
live transitions already keep `Current` accurate through the automatic
refresh described above.

Local checker, mock, fixture, and Vitest results remain a focused proof of
this contract. Local validation is not real-hardware HIL. Todo 6 post-fix
real-board HIL remains required for this code change until executed under
the dated combined-UF2 build.

<!-- persistent-config-current-sync:
current-source:Current-from-/api/v1/config
current-trigger:live-power/switch/GPIO-transitions-auto-refresh
current-scope:power-state|switch-route|GPIO-direction-value
current-no-write:display-sync-no-auto-save-no-flash-no-apply
current-no-flood:one-transition-one-refresh;identical-frames-zero-GETs
current-draft-survives:local-checkbox-draft-survives-refresh
current-refresh-recovery:Refresh-manual-recovery-not-required
current-mutation-truthful:save-clear-pending-until-authority
current-hil-boundary:Todo-6-post-fix-HIL-still-required
-->

### Persistence Recovery Safety

The snapshot uses the existing `storage_partition` through Settings+NVS at
`linkr/config/snapshot`. The header is fixed at 12 bytes; byte 4 version 1
is the only accepted version and byte 7 zero is the only accepted restore
padding. Every successful Save writes the v1 header and applies the
snapshot immediately; every structurally valid v1 snapshot replays every
saved entry on every future normal boot until a later Save or
`config clear` replaces it. A stored blob whose version byte is not 1 is
never replayed, migrated, or auto-cleared.
Missing, corrupt, or unsupported storage falls back to safe firmware
defaults without formatting storage. This feature provides one explicit
snapshot, not named profiles, encrypted storage, authentication,
authorization, or automatic config rollback.

For ROM BOOTSEL installation or recovery, use only the combined MCUboot plus
application `radxa-linkr-debugger-rp2350.uf2`. The app-only `zephyr.uf2` is
invalid for ROM BOOTSEL and can brick the board. OTA accepts the MCUboot-format
`radxa-linkr-debugger-rp2350-ota.bin` only, not either UF2 file. These artifact
rules still apply when checking whether a saved snapshot survives recovery.

### Dry-Run And HIL Boundaries

Use `scripts/config-persistence-hil.sh` from this skill for an evidence-backed
plan. Dry-run is the default and does not invoke curl, serial I/O, sleeps,
BOOTSEL discovery, mounts, copies, flashing, or hardware operations. The runner
discovers IDs from GET in execute mode and never owns the hardware catalog. Its
current local contract sets and selects exact `switch/usb` `pc`, verifies capture and
OTA-active save/clear busy responses, paces OTA
upload with `--limit-rate 64K`, and enumerates controllable power outputs and
output GPIOs for safe cleanup plus readback validation. The
`dangerous-auto-restore` flow validates a confirmed dangerous Save plus
two consecutive reboots, asserting `snapshot.version: 1`, `pending: 0`,
and full replay of every saved entry on each boot. The 2026-08-05
real-hardware HIL report records this flow as PASS; see the
[dated v1-save HIL report](../../doc/testing/results/2026-08-05-persistent-config-v1-save-hil.md).

<!-- persistent-config-example: skill-config-hil-dry-run -->
```sh
sh skills/radxa-linkr-debugger/scripts/config-persistence-hil.sh --dry-run safe-reboot
```

Todo 14 checker, mock, and fixture results are local proof of the documented
contracts only. Local validation is not real-hardware HIL. The 2026-08-05
real-hardware HIL passed the v1 save-and-apply flow; see the
[dated v1-save HIL report](../../doc/testing/results/2026-08-05-persistent-config-v1-save-hil.md).
The historical 2026-07-30 real-hardware HIL passed all six runner flows
for persistence, confirmation, OTA retention, ROM BOOTSEL retention, and
CDC fallback; see the
[historical six-flow report](../../doc/testing/results/2026-07-30-persistent-config-hil.md).
Future local checks remain distinct from board HIL and cannot replace another
board run when hardware behavior changes.

## Common Commands

Set the board URL once per shell/session.

macOS/Linux:

```sh
BOARD_URL="http://172.29.203.1"
```

Windows PowerShell:

```powershell
$BoardUrl = 'http://172.29.203.1'
```

Read full board state.

macOS/Linux:

```sh
curl -fsS "$BOARD_URL/api/v1/status"
```

Windows PowerShell:

```powershell
curl.exe -fsS "$BoardUrl/api/v1/status"
```

List power outputs.

macOS/Linux:

```sh
curl -fsS "$BOARD_URL/api/v1/power"
```

Windows PowerShell:

```powershell
curl.exe -fsS "$BoardUrl/api/v1/power"
```

Control power outputs.

macOS/Linux:

```sh
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"on"}' \
  "$BOARD_URL/api/v1/power/12v_out"

curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"off"}' \
  "$BOARD_URL/api/v1/power/5v_out"
```

Windows PowerShell:

```powershell
curl.exe -fsS -X PUT -H "Content-Type: application/json" `
  --data '{"state":"on"}' `
  "$BoardUrl/api/v1/power/12v_out"

curl.exe -fsS -X PUT -H "Content-Type: application/json" `
  --data '{"state":"off"}' `
  "$BoardUrl/api/v1/power/5v_out"
```

Restart a target board with its normal software reboot or reset interface
first. Use power-cycling only as a hard-restart fallback when soft reboot/reset
is unavailable, the target is unresponsive, or no reset line is exposed.
Confirm the output name first, then turn it off, wait briefly for discharge,
and turn it back on:

```sh
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"off"}' \
  "$BOARD_URL/api/v1/power/5v_out"
sleep 2
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"on"}' \
  "$BOARD_URL/api/v1/power/5v_out"
curl -fsS "$BOARD_URL/api/v1/power"
```

Read ADC current monitors.

```sh
curl -fsS "$BOARD_URL/api/v1/adc/read"
curl -fsS "$BOARD_URL/api/v1/adc/read?channel=5v_out"
curl -fsS "$BOARD_URL/api/v1/adc/read?channel=12v_out"
curl -fsS "$BOARD_URL/api/v1/adc/read?channel=20v_out"
```

Use the firmware logic analyzer for RP2350 PIO2+DMA high-speed GPIO capture.
The sigrok binary protocol runs over two transports: Web UI uses WebSocket
(`/api/v1/live-sessions` → `/api/v1/ws/<slot>`); native sigrok/PulseView uses
raw-TCP port 5556. These transports are mutually exclusive; only one sigrok
session is allowed at a time across both paths.

On the WebSocket path, one binary WebSocket message may carry multiple complete
inner Sigrok request or response frames concatenated in FIFO order. Host response
parsers treat the payload as a byte stream, emit every complete inner Sigrok frame
in order, and may preserve an incomplete trailing inner response frame across receive
calls. Firmware request handling, however, rejects fragmented WebSocket messages and
truncated, oversized, or malformed inner request frames rather than preserving
them; it does not concatenate partial inner frames.

CONFIG v1 pre/post are uint16. Web bounded pre-trigger supports `rising`,
`falling`, and `either` only with `pre_samples >= 1`, `post_samples >= 1`, and
`pre_samples + post_samples <= 512`. Requested rates are 1-25 MHz, and the
selected physical plan must retain at least `2 * ceil(actual_rate / 1000)` samples.
SINGLE supports through 25 MHz, FAST8 through 10 MHz and rejects 25 MHz, and
WIDE11 through 5 MHz and rejects 10 MHz and 25 MHz. Before first connection,
the Web UI permits local editing when generic constraints pass; after connection
it uses real per-mode CAPS and rejects or disables old firmware or a mode without
CAPS mode flag bit 5 (`PRE_TRIGGER`, `1 << 5`). This is separate from HELLO
server flags bit 0 (`CONFIG_V2`) and bit 1 (`GENERIC_PACKED_BURST`). Stream,
trigger NONE, unsupported or high-rate generic packed burst, and ordinary deep
capture remain pre=0. Stream sends both pre and post as zero. Completion is
`pre_samples + post_samples`, and `triggerIndex` equals `pre_samples`.

Firmware reuses the prepared common packed ring/sink lifecycle. Packed samples
are the sole trigger authority after prefill, software scans the edge, and the
firmware freezes and drains the exact `[T-pre,T+post)` window. No invented IRQ
pairing or new buffer is used. Existing deep post behavior remains when pre=0.
The dated evidence is [2026-07-28 pre-trigger and UART HIL](../../doc/testing/results/2026-07-28-logic-analyzer-pre-trigger-uart-hil.md).

GP7-GP9 are not available in Sigrok modes. Web UI sigrok is limited to
GP10-GP20 (GP29 excluded from LA and input-only while used by ADC3). FAST8 mode
captures on GP10-GP17; WIDE11 mode captures on GP10-GP20 (11 channels).

**CONFIG_V2 deep burst**: HELLO server_flags bit 0 advertises CONFIG_V2 capability
and bit 1 advertises GENERIC_PACKED_BURST. The client must use frame0x0b
(CONFIG_V2_REQ, 16B) with u32LE pre/post fields only for bounded post > 65535.
The v1 frame0x05 (12B) remains for bounded captures with post <= 65535 and for
the post=0 stream sentinel; bit1 determines whether supported high-rate post=0
captures exactly 100000 samples and then auto-STOP/drains. With both flags,
bounded `pre=0`, `post=65536..100000` uses the common packed pipeline at every
otherwise supported rate and pin plan. WIDE11 at requested 125 MHz remains invalid.

**High-rate `post=0` capacity-burst matrix**:

| Mode | Rate | pre | post | Notes |
|------|------|-----|------|-------|
| SINGLE | 100 MHz or 125 MHz | 0 | 0 | Captures exactly 100000 samples losslessly then auto-STOP/drain |
| FAST8 | 100 MHz or 125 MHz | 0 | 0 | Captures exactly 100000 samples losslessly then auto-STOP/drain |
| WIDE11 | 100 MHz only | 0 | 0 | Captures exactly 100000 samples losslessly then auto-STOP/drain. 125 MHz rejected by START (INVALID_CONFIG) |

The 2026-07-27 WIDE11 HIL verified the then-current target at 100 MHz with
`pre=0`, `post=100000`, and high-rate `post=0`: each accepted deep-burst case
delivered exactly 100000 samples in 98 DATA frames with zero sample-index gaps.
The common packed arena applies to all modes: SINGLE (one 1-bit lane on FAST8 SM, autopush32, 32 samples/word,
12500 B at 100 MHz), FAST8 (one 8-bit lane, autopush32, 4 samples/word, 100000 B at 100 MHz),
and WIDE11 (SM-A GP10-GP17 8-bit autopush32 100000 B + SM-B GP18-GP20 3-bit autopush30
40000 B; two DMA channels; 144184 B shared burst slice (overlays the 149048 B total backing allocation)). SINGLE and FAST8 use one capture SM;
WIDE11 uses two. A triggered deep burst adds one trigger-only SM running the 3-instruction
trigger program. GP29 is excluded from WIDE11 LA and remains input-only while used by ADC3. Post-capture: up to 98 DATA
frames, max 1024 samples per frame, 140000 B total payload. Two-phase START prepares
ownership and quiesce before the response. NONE sends START_RESP in RUNNING state with no
ARMED event; triggered captures send START_RESP in ARMED state followed by the ARMED event.
GO then synchronously enables the sampler SM(s). The historical WIDE12 predecessor
evidence remains at `doc/testing/results/2026-07-26-logic-analyzer-wide12-100k-hil.md`;
it is not current WIDE11 evidence.

On RP2350 the capture backend uses a 32768-byte aligned hardware DMA write ring
(`uint32_t[8192]`, ring `size_bits=15`) with the official RP2350 DMA
`TRANS_COUNT` ENDLESS mode. If firmware sees definite overrun, possible overrun,
transport backpressure, explicit queue overrun, or bounded-capture completion,
it emits a terminal OVERRUN/ERROR/STOPPED event and stops. DATA and EVENT
sample indices wrap modulo 24 bits; wrap is not an error.

Terminal selection freezes the trigger and active sampler SMs and aborts ring
DMA A/B before any tail drain. DMA channel ownership is retained for the common
cleanup path; only samples already committed to ring memory are drained. In HIL
JSON, `capacity_stop_before_data=true` distinguishes a request that reached an
explicit capacity OVERRUN before its first DATA frame. Such a row passes the
lossless-or-stop contract, but is not evidence that the requested rate was
sustained.

START_RESP is the trigger-safe barrier. It is emitted only after the firmware has
acquired capture ownership and successfully prepared the PIO/DMA backend. Ordinary
finite/ring paths are already armed or running. For WIDE11 deep burst, GO enables
the sampler SM(s) after the START_RESP semantics described above. A host that receives START_RESP may immediately send UART trigger stimulus
or any other action that requires the capture engine to be ready; no false ARMED
or RUNNING EVENT will precede it. A failed start returns a synchronous FRAME_ERROR
and no ARMED or RUNNING EVENT. After START_RESP, the ordered state progression is:
START_RESP with state 2 (ARMED) or state 3 (RUNNING for NONE), then EVENT armed
(rising/falling/either only), then EVENT triggered, then DATA, then EVENT stopped.
NONE is ungated immediate and starts directly in RUNNING state without emitting
an ARMED EVENT; EITHER follows the normal ARMED→TRIGGERED path after first
snapshotting the level.

Measured continuous results on representative HIL setup: WebSocket
SINGLE 1MHz (10 consecutive 5-second runs, ~4.991M-4.997M samples each,
998.16-998.70 ksps effective, zero sample-index gaps, zero disconnects,
STOP response, immediate restart and HTTP health), FAST8 240 kHz,
WIDE11 149 kHz no-gap continuous ceilings; historical/representative raw-TCP
SINGLE 443 kHz, FAST8 241 kHz, WIDE11 147 kHz no-gap continuous ceilings (WIDE11
149 kHz WS ceiling is current target; 147 kHz TCP ceiling is historical WIDE12 reference).
Bounded captures at 100 kHz with post=65535 delivered exactly 65535 samples with
zero gaps for all modes and transports. Bounded pre=0 and post=1..512 HIL results:
WS SINGLE rising/falling post=512 at 5, 25, 50, 100 MHz all passed; WS SINGLE
EITHER post=512 at 5 and 100 MHz passed; TCP SINGLE rising post=512 at 100 MHz
passed; WS and TCP NONE post=1 at actual 125.081 MHz passed; WS continuous 1 MHz
5-second run produced 4,997,120 samples at 999,340.8 samples/s with zero gaps
or disconnects.

**Transport failure handling**: on the WebSocket transport, all Sigrok control,
response, and event sends use a 1000ms timeout. A send failure disconnects the
client, releases capture ownership, and resets its stream queue. This bounded-send
cleanup is state-independent and does not alter protocol request/response ordering.
Do not hide capture lifecycle failures by sleeping before a restart probe. A fresh
session must be able to START immediately after the prior STOP/close boundary;
Sigrok ERROR code 5 with detail 116 is an ADC sampler pause-ack timeout and fails
the restart check.
Final freeze-build regression confirmed: canonical sysbuild passed; app flash
695868/847832 B (82.08%), RAM 475832/532480 B (89.36%), heap 49156 B, and
combined UF2 1443840 B; only the combined UF2
(`build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`) was flashed.
Forced WS RST immediately after a 125MHz NONE/post=1 START: a fresh session 2s
later acquired ownership, received one sample at actual 125.081 MHz with 0 gaps, received
STOP_RESP, and reported HTTP health. Normal WS SINGLE rising pre=0/post=512 at requested
100 MHz passed with exactly 512 samples, trigger index 0, 0 gaps, no DATA/EVENT before
START_RESP, immediate restart, and HTTP health. HTTP BOOTSEL entry succeeded (picotool
lacked permissions; combined UF2 copied through udisksctl); CDC `/dev/ttyACM2` `bootloader`
command entered ROM BOOTSEL (serial read ended with expected EIO on USB disconnect);
same combined UF2 restored normal HTTP startup. Those 2026-07-27 freeze runs
passed all 54 authoritative TCP/WS cases and all 62 high-rate cases. The
authoritative matrix recorded 18 explicit capacity OVERRUN stops; four occurred
before first DATA and are diagnostic contract passes, not sustained-rate claims.

The reduced on-site WIDE11 mapping HIL used `/dev/ttyACM1` TX connected to GP10.
At 100 MHz/post=100000 it received 98 DATA frames and exactly 100000 samples with
zero gaps. GP10 bit 0 showed both levels and nine transitions in 8192 checked
samples, while the GP11-GP20 zero mask had zero violations. This reduced setup
does not verify independent high-state mapping for GP11-GP20 or lane-B alignment;
that requires the documented external generator. GP29 is excluded from WIDE11 LA. A concurrent
JSON WS telemetry client also remained connected during a raw-TCP deep burst: no old-epoch
sample was emitted after the bounded grace period, and telemetry resumed in a fresh
sequence epoch with advancing device time after arena release.

**Architecture**: the LA sink uses 8 DATA plus 1 terminal fixed slots; WS SINGLE
each DATA slot carries up to 2048 packed samples; qdepth=2 urgent wake; sink
consumer priority 7 and blocks naturally on full chunks; legacy callback, TCP, and
non-SINGLE paths use 1024-sample chunks at priority 8 with yield; sender applies
one-byte RLE with BIT_PACK fallback; 6144-byte coalescing buffer; explicit
terminal on buffer pressure; no silent drops.

The logic analyzer lives in the Terminal workspace in the Web UI. Captured
samples can be decoded in-browser using the project-owned Rust/WASM decoder
served at stable URLs:
- `/assets/decoder/logic-decoder.js` (served with JS MIME)
- `/assets/decoder/logic-decoder_bg.wasm` (served with `application/wasm` MIME, gzip-compressed)

The decoder supports UART, I2C, and SPI protocols only; it is not a
libsigrokdecode Python plugin compatibility layer. The UART decoder fixes the
sample-0 frame resume cursor: a frame accepted at sample 0 previously resumed
scanning at sample 1 and treated internal transitions as false start bits. The
cursor now mirrors ordinary-frame behavior at frame end minus one bit, retaining
back-to-back frames. The known-good `Press` waveform previously produced
`50 25 95 CD CD` and now produces exactly `50 72 65 73 73`; the full decoder
suite is 21/21. This was not a baud, inversion, or firmware corruption issue.

For native sigrok/PulseView, connect via raw-TCP port 5556:
```sh
sigrok-cli -d linkr-debugger:conn=tcp-raw/<board-ip>/5556 --scan
```

Full semantics and limits: `doc/logic-analyzer.md`.

High-rate recording is a separate websocket workflow. Use the Rust CLI when you
need to write NDJSON telemetry records to a file. It defaults to 1000Hz and
accepts `--rate-hz HZ` for a 1..1000Hz requested websocket subscription rate:

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- adc record /tmp/adc.ndjson 1000 --rate-hz 250
cargo run --manifest-path cmd-ng/Cargo.toml -- adc record /tmp/adc.csv 1000 --rate-hz 250
```

Each recorder row keeps the existing JSON schema, host receive timestamps, and
`metadata.requested_rate_hz`. Requests above 100Hz use batch JSON on the wire,
while the recorder still writes one row per device sample. Single-sample
firmware telemetry keeps `sequence` and `uptime_us` and also emits
`sample_sequence` plus `device_t_mono_us`. Compact batch samples carry
`sequence` and `uptime_us`; the recorder normalizes them to the same aliases and
also accepts explicit aliases from compatible firmware. It preserves device time
under `metadata.device_timing` and deletes its live session on both successful
completion and error paths. Prefer device time over host receive time when
analyzing cadence, and treat
`metadata.dropped_samples` as authoritative evidence that the per-client
sampling ring overran. A `.csv` output path writes device time and three current
channels directly, using `device_t_mono_us` first, then `uptime_us`, then `0`.

For triggered acquisition, arm the trigger detector over the same live
WebSocket. Firmware does not retain the waveform; subscribe to ADC telemetry
first and persist the stream on the host. Trigger names are `manual`, `current`,
`gpio`, and `power_on`:

```json
{"type":"command","command":"capture_arm","id":"capture-1","mode":"host-stream-v1","trigger":"current","output":"5v_out","threshold_ua":500000,"rate_hz":100}
```

For manual capture send `{"type":"command","command":"capture_trigger"}`.
Firmware emits `capture_triggered` when recording starts. Send `capture_stop`
after host recording ends, or `capture_cancel` to disarm it. The trigger event
contains `device_t_mono_us`, `sample_sequence`, and cumulative
`dropped_samples`; match the sequence against telemetry to align the waveform.
Only the owning WebSocket can trigger, stop, or cancel it.

WebSocket clients can subscribe to telemetry and send control commands on the
same connection. Example subscription payload:

```json
{"type":"subscribe","topic":"live","rate_hz":60}
```

Raw clients may add `"batch_size":20` for high-rate capture. Batch size is
limited to 20, and each client has an independent cursor into the shared sample
ring.

The host TUI does not need to redraw at the same rate as the live data stream.
The board controls WebSocket push cadence, and the same live push path carries
ADC telemetry plus status snapshots with `board_monitoring`, while the TUI can
render at a lower fixed frame rate.

Example control payload:

```json
{"type":"command","command":"power_set","output":"12v_out","state":"on"}
```

Autonomous watchdog recovery is firmware-owned. The host does not arm or feed
the watchdog. Firmware keeps the RP2350 hardware watchdog alive only while core
firmware, the HTTP/API service, and the CDC ACM cmdline fallback are still
reporting healthy liveness. WebSocket session silence, subscription timeout,
and session expiration are not watchdog failure conditions. If core firmware
wedges, the API service stops responding, or the CDC ACM cmdline fallback stops
reporting liveness, firmware stops feeding the watchdog, the MCU resets, and
the next boot enters ROM BOOTSEL using a retained marker. The direct
`bootloader` command and the CDC ACM shell fallback remain independent recovery
paths. Periodic memory diagnostics are log-only debug output and must not be
treated as an additional watchdog participant or a change to BOOTSEL
marker/reset semantics. The watchdog trace line is equally diagnostic-only.

On G3 (RP2350A) boards, GPIO25 (the blue status LED) functions as a watchdog
heartbeat. It blinks at approximately 1 Hz and advances only after a successful
hardware watchdog feed. Skipped or failed feeds reset it to the inactive state
while firmware owns the GPIO.

```sh
curl -fsS "$BOARD_URL/api/v1/watchdog"
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --json watchdog status
```

For raw ADC inspection or hardware debugging, use the optional CLI verbose
output to inspect the firmware-reported diagnostic fields:

```sh
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl adc read -v 5v_out
```

Switch mux routes.

```sh
curl -fsS "$BOARD_URL/api/v1/switch"
curl -fsS "$BOARD_URL/api/v1/switch/sd"
curl -fsS "$BOARD_URL/api/v1/switch/usb"
curl -fsS "$BOARD_URL/api/v1/switch/tf_wp"
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"usb-reader"}' \
  "$BOARD_URL/api/v1/switch/sd"
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"target"}' \
  "$BOARD_URL/api/v1/switch/usb"
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"protected"}' \
  "$BOARD_URL/api/v1/switch/tf_wp"
```

VIN control:

```sh
timeout 5s curl -fsS "$BOARD_URL/api/v1/switch/vin"   # returns 1.8v or 3.3v
timeout 5s curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"3.3v"}' \
  "$BOARD_URL/api/v1/switch/vin"   # safe default
```

VIN defaults to 3.3V at boot. Switching to 1.8V is side-effectful,
and requires confirmed target voltage compatibility and physical measurement
setup before use. The 1.8V procedure is documented in the Expert: VIN 1.8V
Switching section below.

When validating switch behavior, run the sequence strictly in order: send the
`switch route ...` request, wait briefly for settling, then issue the matching
`switch get ...`. Do not run conflicting route changes in parallel if your goal
is to verify stability on real hardware.

The unified `/api/v1/switch/*` family is the interface for mux-style controls
in this repository. `switch sd` controls the RS2099XTQC16 TF/SD route
between `target` and `usb-reader`, while `switch usb` controls the GP03 USB mux
between `pc` and `target`. `switch tf_wp` controls TF card write-protect on the
onboard GL3224 reader: `writable` (boot default, switch off, GPIO22 drives Q12
to pull SD_WP low, which the reader treats as writable) or `protected` (SD_WP
released, card read-only). GL3224 samples SD_WP at card insertion, so a WP
route change applies on the next card attach; re-route `switch sd` away and
back (or re-insert the card) to apply it immediately. The firmware is
the authority on the switch catalog: `GET /api/v1/switch` (and the status/WS
switches object) advertises each switch with its current `route`, the valid
`routes`, and a `requires_confirm` flag, so host clients enumerate switches
instead of hardcoding names or route vocabularies. On G3, GPIO1 VDD_5V and its GPIO6 VDD_1V8 child rail
are always on in Device Tree. The selectable CH347 VIO level is modeled as a
standard `regulator-gpio` regulator with exact 1.8V and 3.3V states; firmware
uses the Zephyr regulator API for `switch vin`. VIN defaults to 3.3V at boot.
Voltage switching is side-effectful; confirm your target supports the selected
level before applying it.

Use allowlisted GPIOs.

```sh
curl -fsS "$BOARD_URL/api/v1/gpio"
curl -fsS "$BOARD_URL/api/v1/gpio/GP13"
curl -fsS "$BOARD_URL/api/v1/gpio/CON_MAS"
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"direction":"output","value":1}' \
  "$BOARD_URL/api/v1/gpio/GP13"
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"direction":"output","value":1}' \
  "$BOARD_URL/api/v1/gpio/CON_MAS"
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"direction":"input"}' \
  "$BOARD_URL/api/v1/gpio/GP13"
```

GPIO list/status responses expose `name`, `pin`, and `note`, plus additive
firmware-owned physical layout metadata: `layoutGroup`, `layoutLabel`,
`layoutRow`, and `layoutColumn`. Output-capable safe allowlist:
`GP7` (`CON_MAS`), `GP8` (`CON_REST`), `GP9` (`CON_USER`), and
`GP10`-`GP20` (J16). `GP29` remains in the persisted/safe catalog but
is owned by the `adc3` voltage monitor on this firmware and is
**input-only**; output attempts fail at the firmware layer (see
[GP29 ownership in adc-telemetry.md](../../doc/adc-telemetry.md#gp29-ownership)).
Control targets may use canonical `GPxx`, raw numeric pins such as `4`, or
board-specific exact notes such as `CON_MAS` or `J16_PIN1`.

Enter BOOTSEL mode for flashing.

**⚠️ 重要：线刷必须使用合成完整 UF2 文件 `radxa-linkr-debugger-rp2350.uf2`，不能使用应用 UF2 文件 `zephyr.uf2`。使用应用 UF2 会导致板子变砖。**

```sh
timeout 5s curl -fsS -X POST "$BOARD_URL/api/v1/bootloader" || true
```

The USB connection can close while the MCU resets, so BOOTSEL enumeration is
the authoritative success check. Use a bounded retry loop with `timeout 5s
lsblk` to poll for a disk whose VENDOR column is exactly `RPI`:

```sh
RPI_DISK=
attempts=10
while [ "$attempts" -gt 0 ]; do
  RPI_DISK=$(timeout 5s lsblk -dpno NAME,VENDOR | awk '$2 == "RPI" { print $1; exit }')
  [ -n "$RPI_DISK" ] && break
  attempts=$((attempts - 1))
  sleep 1
done
[ -n "$RPI_DISK" ] || { echo "BOOTSEL device not found after 10s"; exit 1; }
```

Never assume a device letter such as `/dev/sdb`. The device name depends on
how many other USB storage devices are connected. The `lsblk` approach with the
exact `RPI` vendor match is the reliable discovery method.

Mount the discovered partition and copy the correct UF2:

```sh
RPI_PART=$(timeout 5s lsblk -lnpo NAME,TYPE "$RPI_DISK" | awk '$2 == "part" { print $1; exit }')
[ -n "$RPI_PART" ] || { echo "BOOTSEL partition not found"; exit 1; }
RPI_MOUNT=$(timeout 5s udisksctl mount -b "$RPI_PART" | awk -F" at " '{print $2}' | tr -d '[:space:]')
FLASH_UF2=build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2
cp "$FLASH_UF2" "$RPI_MOUNT/"
```

After copying, allow a settle period before declaring success. Use bounded
retries against the HTTP endpoint to confirm the board has re-enumerated and is
responding:

```sh
BOARD_READY=
attempts=15
while [ "$attempts" -gt 0 ]; do
  if timeout 5s curl -fsS "$BOARD_URL/api/v1/status" >/dev/null; then
    BOARD_READY=1
    break
  fi
  attempts=$((attempts - 1))
  sleep 2
done
[ "$BOARD_READY" = 1 ] || { echo "board HTTP did not recover"; exit 1; }
```

After firmware changes, treat this HTTP BOOTSEL flow and the CDC ACM shell
fallback below as required validation paths before you finish; verify that the
serial fallback path still reaches the standard ROM BOOTSEL workflow.

Latest measured canonical/HIL build (pre-token, i.e. measured and verified
by the 2026-07-31 ADC3 telemetry HIL before the Web-only
`text-[9px]` to `text-[11px]` token fix in
`web/src/components/PowerSparkline.tsx` was rebuilt or reflashed): RAM
494272/532480 bytes (92.82%), flash 734652/847832 bytes (86.65%),
combined UF2 1521152 bytes
(SHA256 `9d64df4bba89d6d8b78fa94d6bb0df134e9381b45df8e0ceee3b05cd4f9d8c48`),
OTA bin 734692 bytes
(SHA256 `fb68a90bce315e7ed2f3b236b128b1fc37df6afb010c50b9d3e95e4f0084284f`).
The dated
[ADC3 telemetry HIL report](../../doc/testing/results/2026-07-31-adc3-telemetry-hil.md)
records the exact build and recovery evidence for this baseline, including
the combined-UF2 dual BOOTSEL recovery. The earlier 701900/847832 flash
(82.79%) with 475896/532480 RAM (89.37%) and 1455616-byte combined UF2
baseline is historical; the dated
[pre-trigger and UART HIL report](../../doc/testing/results/2026-07-28-logic-analyzer-pre-trigger-uart-hil.md)
remains the authority for that build. Earlier freeze-build sizes are
historical and remain unchanged in their dated reports.

If the HTTP control plane is unavailable but the CDC ACM shell is still
reachable, use the local Zephyr shell command instead:

```text
linkr-debugger:~$ bootloader
```

On firmware, the CDC ACM shell also exposes VIN control:

```text
linkr-debugger:~$ vin get   # returns 1.8v/3.3v
linkr-debugger:~$ vin set 3.3v   # safe default
```

This shell command still uses the standard ROM USB BOOTSEL path, so the
device should reappear as the usual `RP2 Boot` / `RPI-RP2` target for UF2 or
`picotool` workflows.

If you want the TUI or convenience wrapper instead of raw HTTP, the CLI still
works:

```sh
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --json doctor
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --json status
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl
```

## MCUboot OTA Firmware Update

The firmware supports unsigned MCUboot OTA firmware update.

**Security facts**: No signature verification, no authentication, no secure boot,
and no anti-rollback protection. Any host with USB NCM access can submit a
firmware image. SHA256 is used only to verify the integrity of the uploaded
payload, not to authenticate the sender.

**Initial installation** requires ROM BOOTSEL flashing with the combined bootable
`radxa-linkr-debugger-rp2350.uf2` artifact. After MCUboot is installed,
subsequent updates can be delivered via OTA using MCUboot-format application
binaries such as `radxa-linkr-debugger-rp2350-ota.bin`.

**Auto-confirm behavior**: After `ota test` reboots into the new image, a
16-second watchdog health gate runs before the image is auto-confirmed. The
browser never calls confirm automatically; firmware owns the entire gate. If the
watchdog resets before auto-confirm completes, the dedicated retained marker
allows MCUboot to roll back to the previous confirmed image instead of forcing
ROM BOOTSEL. Explicit `bootloader` commands and ordinary non-OTA watchdog resets
still enter ROM BOOTSEL.

**Web dashboard OTA**: the embedded Web UI exposes the same OTA workflow under
**Advanced & recovery**. It accepts only MCUboot-format `.bin` files, computes
SHA-256 locally in the browser (Web Crypto API with pure-JS fallback), uploads
via the same `/api/v1/ota/*` endpoints, and shows raw firmware OTA state through
polling. The UI never auto-confirms; the firmware ~16-second watchdog gate is
the only auto-confirm path. When running the UI from GitHub Pages, start the
Rust Host gateway first (`npm run build && npm run host`) so the browser can reach
the board OTA endpoints over the HTTPS-to-HTTP bridge. The gateway permits the
OTA-specific headers (`X-Linkr-Ota-Size`, `X-Linkr-Ota-Sha256`) in CORS
responses.

CLI commands:

```sh
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --json ota status
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --json ota upload /path/to/firmware.bin
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --json ota test
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --json ota confirm
```

Or with `cargo run`:

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- --json ota status
cargo run --manifest-path cmd-ng/Cargo.toml -- --json ota upload /path/to/firmware.bin
cargo run --manifest-path cmd-ng/Cargo.toml -- --json ota test
cargo run --manifest-path cmd-ng/Cargo.toml -- --json ota confirm
```

Raw HTTP API:

```sh
# Check OTA state
curl -fsS "$BOARD_URL/api/v1/ota"

# Upload MCUboot-format binary
curl -fsS -X POST \
  -H 'Content-Type: application/octet-stream' \
  -H 'X-Linkr-Ota-Size: <byte_size>' \
  -H 'X-Linkr-Ota-Sha256: <hex_sha256>' \
  --data-binary @/path/to/firmware.bin \
  "$BOARD_URL/api/v1/ota/upload"

# Request test boot
curl -fsS -X POST "$BOARD_URL/api/v1/ota/test"

# Manually confirm
curl -fsS -X POST "$BOARD_URL/api/v1/ota/confirm"
```

`GET /api/v1/ota` returns `state` (`idle`/`uploading`/`verified`/`pending_test`/
`rebooting`/`failed`), expected/written/max byte sizes, the MCUboot upload area
ID, swap type, and `current_image_confirmed`. OTA upload requires both the
`X-Linkr-Ota-Size` and `X-Linkr-Ota-Sha256` headers. Do not upload `.uf2` or
`.elf` files via OTA; use a MCUboot-format application binary. The release OTA
payload `radxa-linkr-debugger-rp2350-ota.bin` is copied from sysbuild
`zephyr.signed.bin`; with this project config, that filename still represents
unsigned MCUboot format.

**HIL validation**: Changes affecting Web/host OTA control behavior, including
any modification to the OTA upload endpoint, the auto-confirm watchdog gate,
the rollback retained marker, the Web dashboard OTA UI, or the CLI `ota`
command logic, require board-level HIL functional validation before final
production acceptance. The HIL must exercise the full OTA sequence:
upload the MCUboot-format payload, trigger `ota test`, observe the test boot
reboot, confirm the watchdog health gate auto-confirms or manually confirm,
and verify rollback behavior when the watchdog resets before confirm. See
`doc/testing/hil-functional-test-spec.md` for the full checklist. HIL is
required before final acceptance; if any step is deferred or blocked, record it
explicitly and do not claim that the corresponding validation has passed.

## Web OTA HIL Automation

Two automated runners exercise the Web OTA path end-to-end without manual
browser interaction. Both default to dry-run mode and require `--execute` to
perform side-effectful operations.

**API runner** (`scripts/web-ota-hil.sh`): Issues raw HTTP requests against the
board OTA endpoints. Headless and fast. Exercises the OTA state machine, error
codes, and gate logic.

After test reboot, USB NCM may re-enumerate after the firmware's watchdog gate.
The API runner uses `test_marker_present` to distinguish a missed auto-confirm
from rollback: `idle` + confirmed + marker cleared is success, while the same
state with the marker still present is rollback. Missing marker evidence is
inconclusive. The manual flow posts `/confirm` only after observing
`pending_test`, and then requires confirmed idle with the marker cleared.

**Browser runner** (`web/scripts/ota-hil.mjs`): Drives a real Chromium/Chromium
instance via Playwright against the board-hosted Web UI at
`http://172.29.203.1/`. Exercises the full OTA card including local SHA-256
computation, upload, confirmation dialogs, and state polling. Two flows are
available: `auto` (waits for firmware watchdog auto-confirm) and `manual`
(clicks Confirm image after pending_test).

Both runners require explicit gates for side-effectful operations:

| Gate flag | Enables |
|---|---|
| `--allow-upload-test-reboot` | OTA upload, test boot, confirm flows |
| `--allow-bootsel` | HTTP or CDC ACM BOOTSEL entry |
| `--allow-flash` | UF2 copy to RPI-RP2 mount point |

`--flow all` is dry-run-only and cannot be combined with `--execute`.

The browser runner accepts `--playwright-module` and `--chromium-executable` to
control Playwright loading and the browser binary. It does not require global
Playwright installation; playwright-core is loaded dynamically through Node's
module resolution. For Nix users, a temporary nix environment or explicit Nix
store paths can supply the Chromium dependency without claiming an exact
unverified package attribute.

**API runner examples (dry-run)**:

```sh
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh --flow preflight
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh --flow status
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh --flow negative-upload
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh --flow all
```

**API runner examples (executable)**:

```sh
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh \
  --flow api-auto-confirm \
  --image build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin \
  --execute --allow-upload-test-reboot

./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh \
  --flow negative-upload \
  --execute --allow-upload-test-reboot

./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh \
  --flow bootsel-http \
  --execute --allow-bootsel

./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh \
  --flow flash-uf2 \
  --uf2 build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2 \
  --execute --allow-flash
```

Watchdog rollback is BLOCKED in both runners because no safe fault-injection
path exists. The API runner reports this explicitly when `--flow watchdog-rollback`
is selected.

The post-ring HIL above does not validate watchdog fault-injection or automatic
watchdog recovery. It only confirms that the measured Sigrok failure cases did
not wedge NCM/watchdog and that explicit HTTP/CDC ACM BOOTSEL entry paths worked.

**Browser runner examples (dry-run)**:

```sh
cd web
node scripts/ota-hil.mjs --dry-run
```

**Browser runner examples (executable)**:

```sh
cd web
node scripts/ota-hil.mjs --execute --flow both
node scripts/ota-hil.mjs --execute --flow auto
node scripts/ota-hil.mjs --execute --flow manual \
  --chromium-executable /nix/store/...-chromium-.../bin/chromium
```

Both runners use port 80 at `http://172.29.203.1`. The browser runner connects
to the board-hosted Web UI on the same NCM-assigned address. The shell runner
also uses port 80 and the same default URL.

## Rolling Nightly Pre-release

The repository publishes a separate rolling nightly channel through
`.github/workflows/nightly.yml`. It runs only on every push to the
`dev` branch, and it publishes (and overwrites) a mutable `nightly`
Git tag together with a prerelease that is explicitly marked **not**
the latest release. The channel uploads a fixed nine-asset subset (combined UF2, OTA
bin, ELF, map, three Rust CLI archives, the skill bundle, and
`SHA256SUMS.txt`) and prunes any other assets from previous runs.
Unlike the formal `Release` workflow, which ships 13 assets, the
nightly channel intentionally omits the Linux ARM64 Rust CLI archive and all
three unified desktop archives.

The nightly channel is rolling and testing-only. Formal `v*` releases
keep their own `Release` workflow, semantic tags, signing policy, and
release assets; the nightly channel does not affect them. Production
agents and end users must continue to download the formal `Release`
artifacts. Treat the nightly tag exactly like the other automated
canaries: do not promote it into production, do not sign or pin it,
and do not use it to claim board-level HIL has passed without
following the existing HIL procedure.

## OpenOCD / JTAG Workflow

Use OpenOCD through the onboard CH347F path when the target board exposes
JTAG/SWD through the debug fixture. CH347F is wired directly to the target
debug connector. The firmware controls target power and recovery lines;
it does not sit in the JTAG/SWD path and does not act as a CMSIS-DAP,
Picoprobe, or JTAG probe.

First check OpenOCD availability:

```sh
openocd --version
```

Power the target first, then start OpenOCD with the CH347F interface script
available in the host OpenOCD installation and the target config for the board
under test:

```sh
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"on"}' \
  "$BOARD_URL/api/v1/power/5v_out"
curl -fsS "$BOARD_URL/api/v1/power"
openocd -f interface/<ch347-interface>.cfg -f target/<target>.cfg
```

CH347F support depends on the OpenOCD build. If the system OpenOCD package
does not include a CH347F interface script, use the WCH/vendor OpenOCD build
or add the matching interface script.

Use the output that actually powers the target. If the target uses `12v_out`
or `20v_out`, replace `5v_out` accordingly.

When a reset is needed, prefer a target software reboot or OpenOCD reset
command first:

```text
reset halt
reset run
```

Only use power-cycling as a hard-restart fallback when soft reset is not
available or the target is unresponsive.

## Captive Portal Discovery Diagnostics

The firmware exposes a multi-path captive portal detection helper on the NCM
interface. The single HTTP service is bound to `172.29.203.1` on port 80 and
routes by URL path: `/`,
`/assets/*`, `/api/v1/*`, `/api/v1/ws/*`, `/captive-portal/api`, and legacy
detection probes.

DHCP assigns a local NCM address without advertising a default router or DNS
server, so the debugger does not replace the host's Internet path. It sends
DHCP option 114 (Captive Portal URI) set to
`http://172.29.203.1/captive-portal/api`. HTTP on port 80 answers
`/captive-portal/api` with `application/captive+json`; all other GET paths
not matching the routed paths redirect (HTTP 302) to `http://172.29.203.1/`.
OS auto-open is best-effort, not guaranteed; results vary by OS and multi-homed routing.

The pinned Zephyr HTTP/1 server handles dynamic-resource `HEAD` requests before
the application callback and returns a default headers-only HTTP 200. Use `GET`
for the following captive portal checks.

Check the captive portal HTTP endpoint:

```sh
timeout 5s curl -fsS -D - -o /dev/null http://172.29.203.1:80/captive-portal/api
```

Expect HTTP 200 with `Content-Type: application/captive+json`.

The root path serves the embedded Web UI. Check redirect behavior for an
unregistered legacy detection path:

```sh
timeout 5s curl -fsS -D - -o /dev/null http://172.29.203.1:80/generate_204
```

Expect HTTP 302 with `Location: http://172.29.203.1/`.

Read-only API endpoints reject non-GET methods. `POST` requests to
`/api/v1/status` and `/api/v1/adc/read` must return HTTP 405, as must `POST` to
`/captive-portal/api`.

DNS A record check (requires a DNS query tool such as `dig` or `nslookup` if
`curl` alone is insufficient):

```sh
timeout 5s dig +short @172.29.203.1 example.com A
```

Expect `172.29.203.1`. DNS AAAA check:

```sh
timeout 5s dig +short @172.29.203.1 example.com AAAA
```

Expect empty output (NOERROR/NODATA).

## Safety Rules

- Prefer machine-readable JSON responses for all non-interactive use.
- Treat power-output changes, GPIO changes, SD routing, VIN switching, and
  `bootloader` as side-effectful operations. Confirm the target and desired
  state before running them.
- Prefer soft reboot/reset for target-board restarts. Treat power-cycling as a
  hard-restart fallback that is destructive to target runtime state. Confirm
  the exact output and only cycle the output powering the target.
- `5V_FIN` is an input/source power input. Do not present it as a controllable output.
- VIN switching is side-effectful. Confirm your target supports the selected
  voltage (1.8V or 3.3V) before applying it. The TUI requires confirmation
  before changing VIN; the CLI requires `--confirm` flag.
- Only use allowlisted GPIOs reported by `GET /api/v1/gpio` or the equivalent
  CLI command.
- Do not expose board-internal schematic codenames in user-facing output.

## ADC Notes

The four ADC descriptors in this firmware are `5v_out`, `12v_out`,
`20v_out` (current, µA, with `power_enabled`) and `adc3` (voltage on
GP29, µV). The HTTP rich read and the compact WebSocket single-sample
and batch frames live in
[doc/adc-telemetry.md](../../doc/adc-telemetry.md); that file is the
authoritative contract. Highlights:

- `GET /api/v1/adc/read` exposes both raw ADC diagnostics
  (`readings[].raw`, `readings[].mv`) and the current-sense-amplifier
  result (`readings[].current_ua`, `readings[].sensor_value`). For
  `adc3` clients reconstruct signed integer µV from `sensor_value`;
  HTTP does not emit `voltage_uv` or `value` on that channel, and
  there is no `current_ua` or `power_enabled` on the voltage channel.
- The live WebSocket path uses a compact shape on purpose. Each
  reading carries only `name`, `signal`, `kind`, `unit`, `value`
  (signed integer in `unit`), and (current only) `power_enabled`.
  Wire shape changes are atomic: there is no dual-emission shim and
  no `raw`/`mv`/`current_ua`/`sensor_value` ever on the WS line.
  External clients that build on the previous verbose WebSocket shape
  must update, or switch to `GET /api/v1/adc/read` for diagnostics.
- The batch frame declares `channels[]` (`name`, `signal`, `kind`,
  `unit`) once and then carries `values: i32[]` aligned with channels;
  one scalar per channel per sample. `power_enabled_mask` is an
  unsigned 8-bit field whose bits map to current channels only
  (kind="current"); the voltage channel has no power state. `kind`/
  `unit` must agree (`current` ↔ `uA`, `voltage` ↔ `uV`); mismatches
  are rejected.
- The host CLI / Web UI no longer apply host-side ADC calibration
  tables or zero-point correction. Treat the reported values as the
  firmware's direct readings. ADC3 has no probe scaling and reads
  nominal 0..3,300,000 µV (0..3.3 V) on GP29.
- Current-monitor hardware uses INA139 with a 10 mOhm shunt and a
  50 kOhm output load; the ADC3 voltage monitor uses the GP29 ADC3
  input directly, not INA139.
- `GP29` stays in the persisted/safe catalog but is owned by `adc3`,
  so it is **input-only**. `gpio set GP29 ...` (output) requests fail
  at the firmware layer; the host must surface that failure rather
  than retrying. Old v1 input snapshots remain applicable as
  firmware-defaults auto-restore. Old v1 output snapshots remain
  decodable so historical boards do not corrupt, but the apply step
  hits GP29 first and stops. Earlier entries stay applied; GP29 and
  later entries stay pending.

Quick reference:

```sh
curl -fsS "$BOARD_URL/api/v1/adc/read"
curl -fsS "$BOARD_URL/api/v1/adc/read?channel=5v_out"
curl -fsS "$BOARD_URL/api/v1/adc/read?channel=adc3"
```

`adc read adc3` (CLI) and the ADC3 row in the Web Power card expose
the same channel.

## Expert: VIN 1.8V Switching

VIN 1.8V switching applies only to RP2350A boards. This operation is
side-effectful and requires confirmed target voltage compatibility and physical
measurement setup before use.

Prerequisites before any 1.8V switch:

1. Confirm your target device's VIO supports 1.8V signaling.
2. Connect a voltmeter or oscilloscope to the target VIO pin.
3. Acknowledge that incorrect voltage will likely damage the target.

To switch to 1.8V (G3 only):

```sh
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"1.8v"}' \
  "$BOARD_URL/api/v1/switch/vin"
# Then immediately measure target VIO pin — expect ~1.8V
```

To restore safe 3.3V:

```sh
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"route":"3.3v"}' \
  "$BOARD_URL/api/v1/switch/vin"
# Then immediately measure target VIO pin — expect ~3.3V
```

CDC ACM shell equivalent (G3 only):

```text
linkr-debugger:~$ vin set 1.8v
linkr-debugger:~$ vin set 3.3v
```
