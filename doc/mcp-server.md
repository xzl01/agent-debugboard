# Local MCP server

The repository includes a generic local stdio MCP server for Codex, OpenCode
and other MCP clients. It is implemented by the Rust `linkr-host` process and
is a host adapter, not a second device protocol:

```text
Agent -- stdio MCP -- Linkr device gateway -- USB NCM HTTP -- RP2350
                         |
                         +-- linkr-serial-broker.v1 -- CH347F UART0/UART1
                                      |
                                      +-- Web and other Agent subscribers
```

The Web application continues to use the loopback HTTP/WebSocket gateway. MCP
uses the same gateway and Serial Broker, so it does not open a second CH347F
handle, disconnect Web readers, or invent a separate UART ownership model.

## Start and configure

Build the host binary and Web assets once:

```sh
npm --prefix web ci
npm --prefix web run build
cargo build --release --manifest-path host-tools/Cargo.toml
```

Run MCP directly with
`host-tools/target/release/linkr-host mcp`. The server writes MCP only on
stdout; diagnostics and automatically started Host logs go to stderr. MCP
completes its stdio handshake before checking Web assets, the Host port,
USB-NCM or the debugger, so a temporary local dependency failure does not make
the Agent disable the server for the rest of its session. Listing MCP tools
does not touch the board.

By default the MCP process supervises the Web UI, device gateway and Serial
Broker in the background. Failed startup and unexpected Host exit are retried
with exponential backoff from 250 ms up to 30 seconds. An already-running Host
is health-checked every two seconds and adopted without opening a competing
listener. Use `--no-autostart` when process lifetime is managed separately;
MCP still accepts requests immediately and starts working when the external
Host later appears.

Gateway readiness uses the host-only `GET /healthz` endpoint. It does not poll
firmware `/api/v1/status`, so one MCP operation produces only its intended
firmware request and does not consume a second slot from the device HTTP pool.

For Codex, add the following to `~/.codex/config.toml`, using the absolute path
to this checkout:

```toml
[mcp_servers.radxa_linkr_debugger]
type = "stdio"
command = "/Volumes/拓展盘/Dev/agent-debugboard/host-tools/target/release/linkr-host"
args = ["mcp"]
startup_timeout_sec = 20
```

For OpenCode v2, add a local stdio server to `opencode.json`:

```json
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "servers": {
      "radxa_linkr_debugger": {
        "type": "local",
        "command": [
          "/Volumes/拓展盘/Dev/agent-debugboard/host-tools/target/release/linkr-host",
          "mcp"
        ]
      }
    }
  }
}
```

Restart the MCP client after changing its configuration. Other MCP clients can
use the equivalent stdio command and argument list.

Environment overrides:

| Variable | Default | Meaning |
| --- | --- | --- |
| `LINKR_MCP_API_BASE` | `http://127.0.0.1:8787/api/v1` | Device-gateway REST base |
| `LINKR_MCP_SERIAL_URL` | `ws://127.0.0.1:8787/serial` | Serial Broker endpoint |
| `LINKR_WEB_ROOT` | auto-discovered `web/dist` | Built Web UI served by an auto-started Host |
| `LINKR_BOARD_URL` | `http://172.29.203.1:8080` | Native debugger USB-NCM control service |

The command-line `--no-autostart` switch disables only Host process management;
it does not make MCP initialization depend on Host readiness. Automatic startup
and the gateway are intentionally loopback-only. A custom API base must be
started and secured by its operator.

The former Node MCP adapter is retained temporarily as `npm --prefix web run
--silent mcp:node` for migration testing. It is not the primary installation
path.

## Tools

| Tool | Behavior |
| --- | --- |
| `linkr_board_status` | Read a compact firmware/power/route summary; pass `detail: "full"` for GPIO and complete diagnostics |
| `linkr_adc_read` | Read all current-monitor ADC channels |
| `linkr_power_set` | Set `5v_out`, `12v_out` or `20v_out`; requires `confirm: true` |
| `linkr_switch_route` | Set SD, USB or VIO routing; requires `confirm: true` |
| `linkr_serial_connect` | Subscribe to UART0/UART1 at a selected baud rate |
| `linkr_serial_status` | Read connection, baud, subscriber and write-owner state without opening the UART |
| `linkr_serial_read` | Return bounded text after a cursor |
| `linkr_serial_expect` | Wait for text or a regular expression after a cursor |
| `linkr_serial_write` | Perform one ordered write with an optional short claim |
| `linkr_serial_command` | Claim, write, wait for a prompt, then release |
| `linkr_serial_shell_command` | Run an authenticated POSIX shell command and return its exit code plus bounded output |
| `linkr_serial_login` | Detect an existing shell or complete username/password login under one short claim |
| `linkr_serial_disconnect` | Release only this MCP subscriber |

Every result uses the `radxa-linkr-debugger.mcp.v1` envelope. Successful board
results preserve the firmware `radxa-linkr-debugger.v1` payload inside
`result`. Tool failures return a stable `error.code` and `error.message`. When
the local Host cannot be reached, read-only calls return
`host_temporarily_unavailable` with `retryable: true`, `retry_after_ms` and a
request outcome in `error.details`. Hardware-changing calls are not marked
retryable because their outcome must never be guessed or replayed blindly.

Long-running `linkr_serial_expect`, `linkr_serial_login`,
`linkr_serial_command` and `linkr_serial_shell_command` calls emit an immediate
MCP progress event, one heartbeat per second, and a final completed/failed
event when the client supplies a progress token. This keeps supported Agent
clients visibly active without granting access to a separate log file. Progress
messages contain only the tool activity and elapsed time; passwords and full
commands remain excluded. Clients that do not render MCP progress retain the
same final structured result.

Serial cursors are local to one MCP process and count retained UTF-16 text
offsets. Reuse `next_cursor` for subsequent reads. A Broker reconnect inside the
same MCP process carries each channel's cursor checkpoint forward, so new data
never reuses an old offset. Each channel keeps at most 1 MiB of decoded text; an
expired cursor returns `serial_cursor_expired`, while a cursor ahead of the
current process returns `serial_cursor_ahead`. Both errors include the current
earliest and latest cursors. `linkr_serial_command` and exclusive
`linkr_serial_write` claim the Broker only around one operation, so they do not
hold UART ownership while an Agent is thinking.

`linkr_serial_login` accepts target-test credentials as tool arguments but does
not return the password, command echo, or raw login transcript. It recognizes
an already authenticated shell, supports passwordless login, and releases the
Broker claim on success or failure. Configure the prompt fields for targets
whose login or shell prompts differ from the defaults.

Use `linkr_serial_shell_command` after login when the target provides a POSIX
shell. It appends a unique exit-status marker, defaults to failing the tool when
the command returns non-zero, and returns structured `exit_code` and `output`.
The Serial Broker keeps the unique status probe raw only for the MCP owner;
shared Web terminals and their logs receive the real command output without
the internal variable assignment, `printf`, or `__LINKR_RC_*` marker. The
returned MCP output is sanitized the same way.
Keep `linkr_serial_command` for U-Boot, UEFI, bootloaders, or other consoles
that are not POSIX shells.

## Safety boundary

MCP v1 deliberately excludes arbitrary REST paths, GPIO mutation, OTA,
BOOTSEL, Qualcomm EDL and Rockchip MASKROM. Those actions require the explicit
CLI/curl/manual workflows documented by the project. Ordinary target UART
commands remain explicit Agent actions and can change the test target; callers
must still choose commands appropriate for the connected device.

The gateway and MCP server listen only through local stdio/loopback defaults.
There is no authentication or authorization layer, so do not expose port 8787
or the MCP stdio process to untrusted users.
