# Radxa Linkr Host Tools

`linkr-host` is the local desktop entry point for Radxa Linkr Debugger. One
Rust process owns the host-only resources and exposes them consistently to the
Web UI and MCP clients:

```text
linkr-tray -> supervises one loopback Host and reports health
                    |
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

The preferred desktop path is `linkr-tray`. It keeps the combined Host alive,
adopts an already healthy Host, retries failed startup with bounded backoff,
and shows green, amber, or red status for Web/Broker/MCP. Its menu opens the
Web console and JSON status, restarts only the Host child it owns, and shuts
down that managed child on Quit. A per-user lock prevents duplicate tray
icons. Host output is retained in the user's application-data directory. The
tray also enables raw UART RX archiving and opens the archive panel from its
menu. Source/manual `linkr-host serve` keeps archiving off unless
`--serial-log-mode rx` is supplied.

Raw archives use `linkr-serial-log.v1` under the per-user application-data
directory. Each UART open creates a session containing `manifest.json`,
rotated `rx-*.bin` canonical bytes, and `rx-*.ndjson` timestamp/offset indexes.
Defaults are 64 MiB segments, a 2 GiB unpinned quota, and 30-day retention.
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
