# Radxa Linkr Host Tools

`linkr-tray` is the persistent desktop daemon for Radxa Linkr Debugger. Its
single Rust process owns the tray and Host resources and exposes them
consistently to the Web UI and MCP clients:

```text
linkr-tray
    +-> desktop status icon when a graphical session is available
    +-> Web UI + board HTTP/WS gateway
    +-> shared CH347F Serial Broker
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

Open <http://127.0.0.1:8787/>. The Rust process talks directly to the debugger
at `http://172.29.203.1`; the Node/Ruby USB-NCM forwarding workaround is
not used.

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

The Web Serial path remains available for a browser-owned UART. Bridge mode,
MCP and other Agents share `linkr-serial-broker.v1`, so they can receive the
same UART stream without opening competing OS handles. Write claims are short
and operation-scoped.

The former Node gateway and MCP adapter have been removed. New integrations
should invoke `linkr-host`.
