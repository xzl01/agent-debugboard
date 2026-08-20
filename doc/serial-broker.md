# Host Serial Broker protocol

`linkr-host serve` exposes a loopback-only WebSocket endpoint at
`ws://127.0.0.1:8787/serial`. The endpoint speaks exactly
`linkr-serial-broker.v1`; unversioned JSON and raw text frames are rejected.

From a source checkout, build the Web UI and start it with
`cargo run --manifest-path host-tools/Cargo.toml -- serve`. The older
`npm --prefix web run device-bridge` implementation remains available during
the migration, but new Web and Agent integrations should use the Rust Host.

The Broker, rather than an individual browser tab, owns the CH347F serial
ports. UART0 maps to the `D1` device and UART1 maps to `D3`, with sorted CH347F
device order as a fallback. Each channel has:

- one lazy physical serial connection;
- any number of receive subscribers;
- one ordered transmit queue;
- an optional exclusive write owner;
- a short idle grace period after an unexpected client disconnect.

Web Serial remains a separate direct-browser mode. Web Serial and the Host
Broker cannot open the same operating-system serial device at the same time.

## Host raw RX archive

The desktop tray starts Host with `--serial-log-mode rx`. The Broker submits
each raw read to a bounded writer queue before UTF-8 decoding and broadcast, so
invalid UTF-8, NUL bytes, and original byte order are retained. Direct Web
Serial bypasses Host and is not included. Manual `linkr-host serve` defaults to
archive mode `off`; enable it explicitly when required.

The `linkr-serial-log.v1` layout is:

```text
serial-logs/YYYY-MM-DD/<session-uuid>-uart0/
  manifest.json
  rx-000001.bin
  rx-000001.ndjson
```

The binary file is canonical. NDJSON records contain segment, sequence,
offset, length, wall-clock time, and Host monotonic time. A manifest records
the serial path, baud, byte/record counts, segment count, end reason, dropped
bytes and one of `recording`, `complete`, `incomplete`, or `interrupted`.
Startup recovers leftover `recording` manifests as `interrupted`.

The default policy rotates at 64 MiB, retains up to 2 GiB of unpinned sessions,
and expires unpinned sessions after 30 days. If the bounded writer queue fills
or disk I/O fails, serial forwarding continues; Host status becomes degraded
and the affected manifest is never reported as complete. RX is enabled by
default through the tray. TX is intentionally excluded because it can contain
passwords and other secrets.

Local-only management endpoints are under `/host/api/v1`: status at
`/serial-logging/status`, list/detail/download under `/serial-logs`, pin via
`PUT /serial-logs/{id}/pin`, and confirmed delete via
`DELETE /serial-logs/{id}?confirm=true`. Downloads support `raw`, lossy `text`,
and `ndjson`. Browser requests from non-loopback origins are denied even when
that origin is trusted for hardware gateway access.

## Envelope

Every client and server frame is a JSON object containing the protocol and
message type:

```json
{
  "protocol": "linkr-serial-broker.v1",
  "type": "status",
  "request_id": "web-123",
  "channel": "uart0"
}
```

Requests that cause a side effect should include a unique `request_id`.
Channels are `uart0` and `uart1`; baud rates are integers from 300 through
4,000,000.

## Session flow

The server first sends `hello` with the assigned `client_id` and capabilities.
The client then opens a subscription:

```json
{
  "protocol": "linkr-serial-broker.v1",
  "type": "open",
  "request_id": "open-1",
  "channel": "uart0",
  "baud": 115200
}
```

`opened` confirms the physical path and baud. If another subscriber already
uses that channel, the baud must match. Incoming serial bytes are decoded as a
stream and broadcast as `data` frames with `sequence`, `host_t_mono_us`,
`byte_count`, and `text` fields. To avoid turning USB packet boundaries into a
large number of tiny WebSocket messages, the Host combines reads for at most 2
milliseconds or 16 KiB before publishing a frame. A frame is serialized once
and reused for all subscribers; byte order and streaming UTF-8 boundaries are
preserved.

Writes use an explicit encoding and complete only after the host serial driver
has drained the data:

```json
{
  "protocol": "linkr-serial-broker.v1",
  "type": "write",
  "request_id": "write-1",
  "channel": "uart0",
  "data": { "encoding": "utf8", "value": "uname -a\r" }
}
```

The corresponding `write_ack` reports the transmitted byte count. `base64` is
also accepted for binary-safe writes.

An exclusive owner may attach an observer-redaction token to a UTF-8 write:

```json
{
  "protocol": "linkr-serial-broker.v1",
  "type": "write",
  "request_id": "write-private-1",
  "channel": "uart0",
  "data": { "encoding": "utf8", "value": "..." },
  "observer_redaction": { "line_token": "eca90be0b06e496c9a410d875af47976" }
}
```

This is reserved for short internal shell bookkeeping such as the unique exit
status probe used by MCP. The owner receives the raw receive stream so it can
parse the result. Other subscribers receive the same target command output but
not echoed helper lines or the matching status marker. The filter is
line-buffered, handles tokens split across serial reads, and flushes visible
partial output when the owner releases or disconnects. Redaction requires an
active exclusive claim and is rejected for binary writes; it is not a general
secret transport and must not be used to conceal user-requested target
commands.

An automation or MCP consumer can request exclusive write access while all
subscribers continue receiving data:

```json
{
  "protocol": "linkr-serial-broker.v1",
  "type": "claim",
  "request_id": "claim-1",
  "channel": "uart0",
  "owner": "startup-test"
}
```

The Broker replies with `claimed` and publishes a `status` frame. Other clients
receive `serial_busy` for writes until the owner sends `release` or disconnects.
The browser uses this state to disable manual terminal input without hiding the
shared receive stream.

`close` removes only that client's subscription. When it is the last explicit
subscriber, the physical port closes immediately. A network drop instead uses
the idle grace period (`LINKR_SERIAL_IDLE_MS`, 30 seconds by default), allowing
a refreshed browser or restarted agent to reconnect without cycling the port.

## Errors

Errors use a stable `code`, human-readable `message`, optional `request_id` and
`channel`, and a `retryable` flag. Current codes include:

- `unsupported_protocol`, `invalid_json`, `invalid_channel`, `invalid_request`;
- `serial_not_found`, `serial_open_failed`, `serial_not_open`;
- `baud_conflict`, `serial_busy`;
- `invalid_write`, `serial_claim_required`, `serial_redaction_active`,
  `serial_write_failed`, `serial_io_error`;
- `broker_internal_error`.

Clients must match errors by `request_id` and must not count a write as sent
until `write_ack` arrives.
