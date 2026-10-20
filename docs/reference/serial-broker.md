# J4 MCU Serial Transport Contract

This file retains its historical name so existing links do not break. The
current product has no desktop serial-broker protocol. J4 is owned by the MCU
and exposed directly over firmware transports.

## Product identity

There is exactly one logical target serial port: `j4`.

- Physical UART: RP2350 UART1, TX GP10 and RX GP11.
- Default line coding: 115200, 8 data bits, no parity, 1 stop bit, no flow
  control.
- Shared receive storage: one 4096-byte UART RX ring.
- Receive sinks: one USB CDC ACM4 sink, four fixed WebSocket sinks, and two
  fixed raw-TCP sinks.
- Transmit sources: CDC, one per WebSocket slot, and one per raw-TCP client.

Every sink has an independent cursor. A slow WebSocket, TCP client, or USB host
can lose its own oldest bytes without blocking or discarding another sink's
data. All storage, queues, client slots, stacks, and framing buffers are fixed;
the MCU serial path does not allocate unbounded buffers.

USB ACM numbering is host-dependent. `ACM4` describes the firmware interface
observed in the final x1.2 HIL topology, not a promise that Linux will always
create `/dev/ttyACM4`.

## WebSocket transport

The embedded Web UI creates a normal live session through
`POST /api/v1/live-sessions`, connects to its `/api/v1/ws/<slot>` URL, and sends:

```json
{"type":"subscribe","topic":"serial.j4","id":"terminal-j4"}
```

The four existing WS contexts are reused; no separate serial WebSocket pool
exists. Within one slot, `serial.j4`, live ADC, and sigrok modes are mutually
exclusive. The page opens a separate slot when it needs telemetry and terminal
traffic concurrently.

### Data framing

- MCU → browser: each binary WebSocket message contains raw J4 bytes.
- Browser → MCU: one complete final binary WebSocket message becomes one atomic
  UART TX request, maximum 480 bytes.
- The receiver assembles nonblocking short reads and RFC 6455 continuation
  frames. It does not admit a partial frame, does not reuse the preceding frame
  type, and drains an oversized message before accepting the next message.
- NUL, `0xff`, and arbitrary binary bytes are preserved. Text frames remain the
  JSON control plane.

On successful admission firmware sends `serial_write_accepted` with an event
ID. This means the bounded UART queue owns the payload; it does **not** mean the
last stop bit reached the wire. The shared completion dispatcher later sends
the matching `serial_write_complete` only to the source slot, with disposition
`completed` or `canceled`. A disconnected slot cannot receive an old session's
completion.

`serial_state` reports runtime state, line coding, generation, DTR/RTS, USB
configuration, queue state, and key cancellation/drop counters.
`serial_overrun.dropped_bytes` is the increment since the prior notice;
`total_dropped_bytes` is that sink's cumulative counter.

## Raw TCP transport

The listener binds all local IPv4 addresses:

```text
0.0.0.0:5555
```

It accepts at most two clients, one fixed sink/source pair per connection.
Every socket `recv` of up to 480 bytes becomes one atomic UART TX request. If
the UART queue cannot accept it immediately, the adapter retains that one fixed
message and retries through its bounded poll loop instead of reading more
socket data. Downlink supports partial `send`; only bytes accepted by the socket
are consumed from the serial sink.

This is raw binary TCP. It has no banner, echo, CR/LF conversion, Telnet/IAC
negotiation, SSH, or noVNC protocol. A simple client is:

```sh
nc <board-ncm-ip> 5555
```

The listener, serial-state polling, sink probes, and completion dispatch remain
bounded at 20 ms or less. It binds `0.0.0.0`, so a persisted dynamic NCM subnet
does not require a listener rebuild or fixed-address special case.

## USB CDC ACM transport

The J4 USB interface is another sink/source for the same logical port. DTR
controls CDC receive attachment; line-coding notifications use the same
configuration state machine as HTTP. The control shell is a different CDC ACM
interface and must not be mistaken for J4.

## Configuration API

`GET /api/v1/serial/j4` returns availability, runtime state, current line
coding, generation, DTR/RTS/USB state, sink counts, queue state, transport
counters, and drops.

`PUT /api/v1/serial/j4/config` accepts the complete tuple plus
`expected_generation`:

```json
{
  "baud": 115200,
  "data_bits": 8,
  "parity": "none",
  "stop_bits": 1,
  "flow_control": "none",
  "expected_generation": 1
}
```

HTTP 202 returns `accepted:true` and a `request_id`; application is
asynchronous and its result appears in subsequent GET status. A stale
generation or busy transition returns HTTP 409. The body accumulator remains
poisoned after an assembly failure until transaction COMPLETE/ABORT, preventing
a legal trailing fragment from becoming a second request.

## PAUSED, capture ownership, and recovery

GP10/GP11 are shared with PIO2 logic-analyzer modes. Before capture takes an
owned pin, firmware freezes new serial admission and waits for an active UART TX
to reach physical wire-complete. The wait is bounded; queued source messages are
canceled rather than falsely completed. It then advances sink epochs and pauses
J4 before changing the pin mux.

While PAUSED, raw TCP stops destructive socket reads and applies TCP
backpressure. WS payloads receive an explicit `serial_paused` error; the WS and
TCP connections remain open. `sink_read == -EAGAIN` is normal no-data while
paused, not a reason to unregister the client. FAULTED and RECONFIG states also
reject new WS payloads explicitly.

STOP or terminal capture cleanup returns GP10/GP11 to UART1, reattaches fresh
sink epochs, and resumes queued admission. Verify J4 before and after every
capture that owns either pin. The active worst case of one 480-byte frame at
9600 baud through the pause timeout is host-model tested; current board evidence
covers capture while J4 is idle or carrying normal traffic, not that worst-case
wire-duration timeout.

## Retired host path

Early development builds described a loopback host broker, two logical
`uart0`/`uart1` channels, direct browser Web Serial, and CH347 serial discovery.
Those contracts are retired and are not compatibility surfaces. The onboard
CH347 remains relevant to JTAG/SWD documentation and an external
`/dev/ttyACM1` may be used as a logic-analyzer stimulus fixture, but neither is
the J4 product transport. Port 5173 is a Vite preview only; there is no
`127.0.0.1:8787` J4 broker endpoint.

## Verification evidence

The current x1.2 board evidence is recorded in
[2026-10-21 x1.2 ADC/pinout/J4 HIL](../testing/results/2026-10-21-x1.2-adc-pinout-j4-hil.md).
It covers the embedded xterm.js terminal, CDC + WS + two TCP sinks including a
slow sink, accepted/complete ownership, raw TCP 5555, and J4 recovery after a
GP10 logic-analyzer capture. Read the report limitations before treating it as
electrical or worst-case timing certification.
