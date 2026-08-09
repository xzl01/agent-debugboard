# Radxa Linkr Host Tools

`linkr-host` is the local desktop entry point for Radxa Linkr Debugger. One
Rust process owns the host-only resources and exposes them consistently to the
Web UI and MCP clients:

```text
Browser -> http://127.0.0.1:8787 -> Web UI + board HTTP/WS gateway
                                      |
                                      +-> shared CH347F Serial Broker
Codex/OpenCode -> stdio MCP -----------+
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
at `http://172.29.203.1:8080`; the Node/Ruby USB-NCM forwarding workaround is
not used.

Useful commands:

```sh
cargo run --manifest-path host-tools/Cargo.toml -- status
cargo run --manifest-path host-tools/Cargo.toml -- doctor
cargo run --manifest-path host-tools/Cargo.toml -- open
cargo run --manifest-path host-tools/Cargo.toml -- mcp
```

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

The former Node gateway and MCP adapter remain in `web/serial-bridge.mjs` and
`web/scripts/linkr-mcp-server.mjs` as migration references. New integrations
should invoke `linkr-host`.
