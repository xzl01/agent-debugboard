# Radxa Linkr Host Tools

`linkr-tray` is the persistent desktop daemon for Radxa Linkr Debugger. Its
single Rust process owns the tray and Host resources and exposes them
consistently to the Web UI and MCP clients:

```text
linkr-tray
    +-> desktop status icon when a graphical session is available
    +-> Web UI + board HTTP/WS gateway
    +-> retained legacy CH347F Serial Broker (not MCU J4)
    +-> bounded raw UART RX archive on host disk
    +-> resident Streamable HTTP MCP at /mcp

Legacy MCP clients -> linkr-host mcp (stdio compatibility adapter)
```

It binds to loopback only. The hardware gateway has no authentication and
refuses a non-loopback `--host` value.

## Build and run

Build the Web UI once, then start the combined host:

```sh
npm --prefix web ci
npm --prefix web run build
cargo run --manifest-path host-tools/Cargo.toml -- serve
```

Open <http://127.0.0.1:8787/>. The Rust process talks directly to the debugger;
pass `--board-url http://<board-ip>` when using the derived network identity
(the board is `.1` of the host NCM `/30`). The Node/Ruby USB-NCM forwarding
workaround is not used.

Useful commands:

```sh
cargo run --manifest-path host-tools/Cargo.toml -- status
cargo run --manifest-path host-tools/Cargo.toml -- doctor
cargo run --manifest-path host-tools/Cargo.toml -- open
cargo run --manifest-path host-tools/Cargo.toml -- mcp
cargo run --manifest-path host-tools/Cargo.toml --bin linkr-tray -- --help
```

The preferred desktop path is `linkr-tray`. It runs the combined Host in the
same process and retries failed startup with bounded backoff. `--headless`
runs that same daemon without constructing GTK/AppIndicator UI, but the Linux
`linkr-tray` executable remains GTK-linked; a minimal system without GTK uses
standalone `linkr-host serve` instead. A per-user
lock makes graphical, headless, Nix, and non-Nix launches converge on one
daemon. If a graphical session becomes available later, the next CLI or `-d`
launch asks the headless daemon to release its Host and lock, then replaces it
with one graphical Tray daemon. If an explicit standalone Host already owns the
port, the graphical Tray adopts it without starting a competing Host and starts
an in-process replacement only if that endpoint later disappears. Its icon
follows the Web UI animation contract:
Starting chases the
four leaves over 1.2 seconds, Offline flashes all four over 1.2 seconds, and
Ready keeps the leaves stable except for the lower-right Host Bridge UART
heartbeat. In Ready, the upper-left, upper-right, and lower-left X leaves show
the firmware-reported 5 V, 12 V, and 20 V rails; only the center diamond runs
the 0.8-second board heartbeat. A binary Sigrok WebSocket session temporarily
rotates the complete X at one revolution per 480 ms. The Host publishes both
the live session count and a bounded
one-second activity latch in `/host/api/v1/status` under
`activity.logic_analyzer_sessions` and `activity.logic_analyzer_active`, so a
short capture still reaches the tray's 100 ms Host-activity probe without
adding a firmware protocol field. Rail and board-heartbeat status are read
directly from the configured firmware URL every 100 ms instead of passing
through the Host gateway. A small internal wake pulse keeps these animations
advancing when the Linux desktop is otherwise idle without adding status
requests. Its menu opens the Web console and JSON status, restarts its
in-process Host task, and shuts down the daemon on Quit. Desktop and Host
output is retained at
`$XDG_DATA_HOME/radxa-linkr-debugger/host.log` (defaulting to
`~/.local/share/radxa-linkr-debugger/host.log` on Linux). The
tray also enables raw UART RX archiving and opens the archive panel from its
menu. Source/manual `linkr-host serve` keeps archiving off unless
`--serial-log-mode rx` is supplied.

Raw archives use `linkr-serial-log.v1` under
`$XDG_DATA_HOME/radxa-linkr-debugger/serial-logs`. Each UART open creates a
session containing `manifest.json`,
rotated `rx-*.bin` canonical bytes, and `rx-*.ndjson` timestamp/offset indexes.
Defaults are 64 MiB segments, a 2 GiB unpinned quota, and 30-day retention.
On Unix, the archive root and session directories are forced to mode `0700`
and every raw, index, manifest, and pin file to `0600`, independent of umask.
Pre-existing symbolic links at managed archive paths are rejected or skipped,
including during startup hardening and artifact downloads.
Disk errors or writer-queue overflow never block UART forwarding; the Host
reports degradation and marks affected sessions incomplete. TX is not archived
because it can contain passwords and other secrets.

The Host always exposes the resident MCP endpoint at
`http://127.0.0.1:8787/mcp`. `linkr-host mcp` remains available for clients
that support only stdio.

`linkr-host mcp` completes its stdio handshake without waiting for the Web
gateway, Web assets, USB-NCM or the debugger. It supervises the loopback
Web/gateway/Broker process in the background and retries startup with bounded
exponential backoff, so an Agent session is not permanently disabled by a
temporary startup failure. Diagnostics go to stderr so stdout remains a clean
MCP transport. Pass `--no-autostart` when another process manages Host; MCP
still starts immediately and tools recover when that external Host appears.

Long-running UART expect, login and command tools emit MCP progress heartbeats
when the client supplies a progress token. Dependency failures are returned as
structured tool errors. Only errors explicitly marked `retryable: true` are
safe for automatic replay; hardware-changing calls are never marked retryable.

## Compatibility boundary

The embedded product terminal uses MCU `j4`; the optional Host's retained
`/serial` endpoint and MCP serial tools instead use the legacy
`linkr-serial-broker.v1` CH347 `uart0`/`uart1` channels. They are not aliases for
J4. Write claims are short and operation-scoped. This change does not remove
the Broker or introduce a legacy firmware compatibility promise.

CH347 admission requires VID/PID `1a86:55de`, verified physical USB identity,
and a complete, unique pair of CDC communication interfaces 0/2. Port-name
suffixes and sorted indices are never used. Multiple devices require the
optional `open.device_serial` selector; missing or ambiguous identity is an
error, not a fallback. Native verification uses Linux sysfs, Windows
SetupAPI/Configuration Manager physical-parent devnodes, and macOS IOKit
physical-device ancestry. macOS CDC data interfaces and native dialin/callout
aliases are normalized using OS metadata, not filenames. MCP
`linkr_serial_connect` exposes the optional `device_serial` selector and forwards
it to the Broker. It never bypasses identity checks.

Broker output is bounded per peer: 64 queued messages plus one in-flight send,
with a 256 KiB payload budget including that send. Nonblocking overflow cancels
only the slow peer. Input messages/frames are limited to 64 KiB; the independent
socket reader feeds a sequential executor with 16 queued commands plus one
executing command, within a 256 KiB raw-message budget. It continues detecting
disconnects during serial backpressure; EOF/Close cancels execution and releases
the peer's subscriptions and claim.

Serial write/drain has a cancellable 5-second deadline. A local adapter queries
the driver's output queue without calling the serial library's synchronous
`tcdrain`/blocking flush. `write_ack.completion=driver_queue_empty` means async
writes completed and the driver queue was empty, **not** physical UART
wire-complete. Failed writes may have sent a prefix and must not be automatically
replayed. Failed opens leave idle reclamation intact.

Managed shutdown closes admission before canceling Broker/proxy sockets and
serial tasks, including late upgrades and blocked upstream I/O. It drains async
tasks for 1 second, then aborts and joins them before closing ports/logs. HTTP
drain is bounded concurrently; log waits are bounded separately (up to 4 seconds
total cooperative shutdown). Old sockets cannot reopen channels after restart.
See [the serial transport contract](../docs/reference/serial-broker.md#retained-legacy-host-broker-not-j4)
for selectors, failure semantics, and OS-worker limitations. Host regressions
use mock USB inventories, loopback sockets, and Unix virtual serial ports;
they do not establish hardware compatibility or HIL acceptance.

The former Node gateway and MCP adapter have been removed. New integrations
should invoke `linkr-host`.
