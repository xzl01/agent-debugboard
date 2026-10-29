# J4 MCU Serial Transport Contract

This file retains its historical name so existing links do not break. J4 is
owned by the MCU and exposed directly over firmware transports, not through a
desktop serial broker. The separately retained legacy CH347 Host endpoint is
documented below; it is not the J4 product transport.

## Product identity

There is exactly one logical target serial port: `j4`.

- Physical UART: RP2350 UART1, TX GP10 and RX GP11.
- Default line coding: 115200, 8 data bits, no parity, 1 stop bit, no flow
  control.
- Baud accepts every integer from 4800 through 115200. The five recommended
  presets are 9600, 19200, 38400, 57600, and 115200; presets are UI
  convenience, not a whitelist. Requests above 115200 are rejected. Earlier
  230400/921600/1500000 samples were diagnostics, not product support.
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

Host write deadlines are derived from the active line coding and the bounded
eight-slot TX queue, plus a fixed 2-second scheduling margin. This avoids a
fixed timeout that would falsely fail legal low-baud writes while remaining
bounded.

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

## Automatic Terminal Replies

Explicit UART input remains multiwriter. Every complete WebSocket binary message
and every raw-TCP receive is admitted as one atomic TX request into the same
bounded queue; there is no per-client TX-owner acquisition, and a client that is
not selected for automatic replies is not read-only. Its keyboard, IME
composition, paste, Ctrl-C, and automation input still go to J4. ANSI and other
escape sequences from the target are never filtered.

Only automatic terminal replies have a single firmware-selected responder:

- CDC is the responder whenever its DTR line is asserted.
- Otherwise exactly one attached WebSocket slot is selected deterministically:
  the lowest-numbered attached slot.
- If neither is available, there is no automatic responder.
- Raw TCP is never selected for automatic replies.

`GET /api/v1/serial/j4` reports the global responder as
`terminal_responder`, an object such as
`{"transport":"cdc","slot":0}` or `{"transport":"websocket","slot":2}`, or
`null` when none is selected. A WebSocket `serial_state` frame instead reports
`terminal_responder` as a boolean for that slot and carries the selected
descriptor in `responder`; only an explicit `true` authorizes that page to
answer terminal queries. A descriptor never identifies the receiving browser by
itself, and DTR forces these Web automatic replies off without disabling manual
input.

Non-selected Web clients suppress xterm's internal DSR/DA/CPR and window-size
report generation. Their
explicit keyboard input, IME composition, and paste still send exactly once
through the manual path. The terminal UI therefore labels them **Auto-reply
observer** and **Manual input allowed**, not read-only.

State and data epoch boundaries prevent stale terminal state across responder
and line-coding changes. Generation values are monotonic within a connection,
and older connection/render epochs are discarded rather than re-applied
(ABA-safe). A generation change invalidates pending TX and rebuilds the xterm
renderer before subsequent RX. Responder promotion preserves the same xterm
modes: pre-promotion output is drained under automatic suppression, new RX stays
queued, and a zero-byte parser fence completes only after the old writes;
automatic replies are enabled only after that fence. Demotion merges any
unfinished promotion queue back into passive RX. A passive terminal's partial or
queued query is never replayed into an automatic reply, and rejected writes are
never replayed.

The finite 2026-10-21 board matrix used a controlled target tty:

| Attached clients | Automatic responder | Observed automatic replies |
| --- | --- | --- |
| one Web, no CDC DTR | that Web slot | exactly one `ESC[2;1R` |
| two Web, no CDC DTR | lowest Web slot | exactly one `ESC[2;1R` |
| Web + CDC DTR | CDC | only CDC `ESC[24;80R`; no Web reply |
| passive Web manual `x` | other client or none | target received `0x78` |

These rows are bounded functional evidence for terminal query handling, not a
claim that every possible terminal escape sequence or multi-client race was
exhaustively tested.

## Local terminal size

The Web UI fits its local xterm after layout/font readiness and on container,
window, fullscreen, font-size, or tab-visibility changes. Fit requests are
coalesced into an animation frame and canceled when the renderer is disposed.
Visual padding is on an outer frame; xterm mounts in an unpadded inner content
area so addon-fit 0.11.0 does not count the frame's padding as extra cells.
Fit also requires a nonzero layout box. When an App workspace hides an ancestor
while the browser document remains visible, no fit runs and the previous rows,
columns and alternate-screen contents are retained. ResizeObserver or a visible
document event schedules a new fit after the layout returns.

When the FontFaceSet API is available, the terminal requests its selected font
and waits for initial loading to settle **before creating/opening xterm**, so
the first glyph measurement does not cache a temporary fallback width. Missing
fonts fall back to the configured font stack; without that API rendering starts
normally. Font-family and font-size requests share the fit scheduler: hidden
layouts only retain the latest request. Once that font is ready and a nonzero
layout exists, the scheduler applies both public options before fitting, without
recreating the parser or clearing its buffers. Older font promises cannot replace
a newer request. An
initially hidden DOM renderer's invalid zero-size glyph cache is remeasured via
public same-size `Terminal.resize` before fit. Disposed/superseded renderer epochs
cannot be created by late font completions. RX queue limits and responder
promotion fences are unchanged.
This changes only the local terminal's rows and columns. Like picocom/raw UART,
the transport does not automatically push browser dimensions to the target tty
through `TIOCSWINSZ`. Raw UART does support **in-band terminal size queries**;
no PTY resize message is needed for these probes:

- Standard xterm/BusyBox `resize` saves the cursor (`ESC 7`), resets the scroll
  region (`CSI r`), moves beyond the bottom-right corner (`CSI 999;999 H` or
  `CSI 9999;9999 H`), requests cursor position (`CSI 6 n`), then restores the
  cursor (`ESC 8`). After a successful local fit to 132 columns × 43 rows, the
  selected Web responder returns CPR `ESC[43;132R`.
  A DA query is `CSI c` (`ESC[c`), not `ESC c` (RIS, a terminal reset).
- The character-size query used by `resize -s`, `CSI 18 t`, returns
  `ESC[8;43;132t` at the same size. The pinned xterm 6.0.0 defaults to
  `windowOptions: {}`, which suppresses this query. The Web terminal enables
  **only** `windowOptions: { getWinSizeChars: true }`; `setWinSizeChars` and all
  other window operations stay disabled.

These replies report the xterm's current fitted dimensions, including after a
later fit or local terminal resize. A fit alone sends no UART bytes. A target
utility can obtain the size through the queries and apply it to its own tty;
local fitting is not itself an update to the target's `stty size`.

The Web slot must be selected for automatic replies, DTR must be false, and
normal input admission must be available. Observer slots send neither CPR nor
the `CSI 18 t` reply; with CDC DTR asserted, CDC owns automatic responses and
this browser's dimensions are not authoritative. Automation, backpressure,
non-ready/read-only/configuring states and the responder-promotion fence retain
their existing suppression rules. This change does not bypass those gates or
retry an inhibited query automatically.

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

The GET current tuple and generation are authoritative for both HTTP and CDC.
USB SET_LINE_CODING completes at the USB control-transfer layer and cannot
report a later application rejection; clients must read GET status and its
configuration counters. Web exposes a custom numeric baud input in addition to
the five recommended presets.

### CDC line-coding staging and promotion

Firmware internally tracks four CDC line-control states: `DOWN` (physical DTR
low, published gate closed), `PROMOTING` (physical DTR high, staged work
pending), `UP` (DTR and ingress published), and `BLOCKED` (published gate closed
until a recovery boundary). These are internal implementation semantics. The
external `/api/v1/serial/j4` response does not expose a phase or block-reason
field; `dtr` is the published CDC/gate state, not the raw physical sample.

- While DTR is false, a USB default `SET_LINE_CODING` notification only stages a
  candidate tuple. It does not change active line coding or the generation.
- An HTTP/WS `PUT /api/v1/serial/j4/config` in that low state applies
  immediately through the normal worker path and advances the generation. It is
  not held behind a future DTR assertion.
- LOW→HIGH enters `PROMOTING`. Firmware first applies any staged USB tuple and
  observes completion or rollback; only then does it publish DTR and open CDC
  ingress. A failed apply, attach, or detach enters `BLOCKED` and keeps
  DTR/ingress closed until a successful LOW/reset boundary. The specific
  unknown-LOW and detach-failure reasons have safe same-LOW recovery.
- A request while the CDC session owns the port (non-`DOWN`) is rejected as
  busy/conflict; it is not staged into the USB path.
- Every USB notification increments a sequence before the HAL read and remains
  inflight until its complete state commit. Promotion requires an unchanged
  sequence and zero inflight notifications, so a HAL read window cannot promote
  a stale DTR sample.
- Staged/mailbox work carries a session epoch, and the worker validates that
  identity before applying. A stale attempt cannot cross a LOW/reset or a new
  session.
- Deferred USB notification handling is a production prerequisite: the
  notification callbacks only stage/commit state and wake the control worker.
  They never perform physical attach, promotion, or HAL configuration in
  notification context.

## PAUSED, capture ownership, and recovery

GP10/GP11 are shared with PIO2 logic-analyzer modes. Before capture takes an
owned pin, firmware freezes new serial admission and waits for an active UART TX
to reach physical wire-complete. The wait is bounded; queued source messages are
canceled rather than falsely completed. It then advances sink epochs and pauses
J4 before changing the pin mux.

The current FAST8 physical span is GP10–GP17, not only the selected logical
channels. Selecting just GP12–GP16 in the temporarily narrowed Web picker still
takes GP10/GP11 and pauses J4; the UI restriction is not UART isolation.

While PAUSED, raw TCP stops destructive socket reads and applies TCP
backpressure. WS payloads receive an explicit `serial_paused` error; the WS and
TCP connections remain open. `sink_read == -EAGAIN` is normal no-data while
paused, not a reason to unregister the client. FAULTED and RECONFIG states also
reject new WS payloads explicitly.

STOP or terminal capture cleanup returns GP10/GP11 to UART1, reattaches fresh
sink epochs, and resumes queued admission. Verify J4 before and after every
capture that owns either pin. The maximum pause wait is 1250 ms, covering one
480-byte 8E2 frame at the minimum 4800 baud (about 1.2 seconds) plus margin.
Current board evidence completed that low-baud event at 1.213 seconds before a
GP10 SINGLE capture and then restored 115200 baud.

## Retained legacy Host broker (not J4)

The optional Rust Host still exposes `/serial` on its loopback listener
(default `127.0.0.1:8787`) using `linkr-serial-broker.v1`. Its `uart0`/`uart1`
channels are legacy CH347 USB CDC functions, **not** aliases for MCU `j4`.
The Host's MCP serial consumer still uses this endpoint. This retention does
not create an old-firmware compatibility promise. Port 5173 is only a Vite
preview; neither endpoint is a J4 broker.

### Device admission

- Discovery requires USB VID/PID `1a86:55de`, independently verified USB device
  identity and serial, and exactly one complete pair of CDC communication
  interfaces 0 and 2 on that same physical device. `uart0` selects interface 0;
  `uart1` selects interface 2. This is USB function identity, not board pin or
  connector mapping.
- Port suffixes, lexicographic order, array indices, generic WCH VID matches,
  and tty names are never selection evidence. Missing interfaces, unknown
  identity, duplicated interfaces/serials, or multiple eligible devices fail
  closed before opening any OS port.
- An `open` request may include `device_serial` to select one unique USB device:

  ```json
  {"protocol":"linkr-serial-broker.v1","type":"open","channel":"uart0","baud":115200,"device_serial":"<USB-serial>"}
  ```

  Omitting the selector requires a unique device. A selector cannot bypass
  VID/PID/interface validation. Selecting another device on an already open
  channel returns `serial_device_conflict`; a baud mismatch returns
  `baud_conflict`. `opened` and `status` include `device_serial`.
  MCP `linkr_serial_connect` accepts the same optional `device_serial` and
  forwards it unchanged to the Broker open frame. Omission sends no selector.
- Native identity providers use Linux sysfs ancestry, Windows SetupAPI COM
  metadata plus Configuration Manager USB interface/physical-parent devnodes,
  and macOS IOKit serial-service/interface/physical-device ancestry. Physical
  grouping uses the actual parent node, never just matching serial strings.
  `serialport` interface metadata is enabled and cross-checked when present.
- On macOS the native CDC interface class distinguishes communication interfaces
  0/2 from data interfaces 1/3. Data interfaces normalize to their communication
  function. IOKit-advertised dialin/callout aliases count as one serial service;
  only its actual `IOCalloutDevice` path is admitted. No filename suffix or
  array index participates in either decision. Missing or contradictory native
  identity still returns `serial_open_failed` on any platform.

### Flow control and lifecycle

- Each peer has at most 64 queued outgoing messages (plus one in-flight send)
  and a 256 KiB payload-byte budget including the in-flight message. Enqueue is
  nonblocking. Exhausting either budget cancels only that peer and releases its
  subscriptions/write claim; other peers continue. Incoming Broker messages
  and individual frames are limited to 64 KiB. A separate, continuously polled
  socket reader feeds a sequential command executor through a nonblocking queue
  of 16 commands plus one executing command, with a 256 KiB raw-message budget
  including the executing command. Input overflow cancels only that peer.
  EOF/Close cancels execution and drops queued commands even while serial output
  is backpressured, then releases subscriptions and the write claim.
- Serial writes remain ordered. Write plus driver-queue drain has a 5-second
  deadline and is interruptible by shutdown/disconnect. The repo-local serial
  adapter never invokes the underlying synchronous `tcdrain`/blocking flush or
  hides it in an abortable-looking blocking task. It polls `bytes_to_write`
  (`TIOCOUTQ` on POSIX, `ClearCommError`/`COMSTAT.cbOutQue` on Windows), yielding
  between nonempty reports. `write_ack.completion` is `driver_queue_empty`:
  the async writes completed and the driver reported no queued output. This is
  **not** a guarantee that the USB-device FIFO or final UART stop bit reached the
  target. Queue-query errors fail the write rather than silently acknowledging.
  A failed/canceled write can have transmitted a prefix and is not safe to
  replay (`serial_write_failed.retryable=false`).
- Channel subscriptions are authoritative; there is no independently committed
  reverse index. A successful open commits its subscription and cancels idle
  reclamation in one critical section. Failed opens preserve the existing idle
  timer. Unexpected disconnect of the last subscriber starts the configured
  idle timer; explicit last-subscriber close releases the port immediately.
- Host shutdown first closes admission and persistently cancels the instance.
  Both Broker and board-proxy WebSockets, including late upgrade callbacks,
  belong to that lifecycle. Cancellation encloses upstream connect/send/close,
  not just reads. Old sockets cannot open ports after an in-process restart;
  clients must reconnect to the new instance. Transport termination need not
  complete a WebSocket close handshake and does not undo already sent UART bytes.
- Async tasks have a 1-second cooperative drain before abort-and-join. Only
  after task reclamation are ports and logs closed. HTTP drain is limited to
  1 second concurrently; each of the two log-session finishes and final log
  shutdown has a 1-second wait budget (up to 4 seconds total cooperative Host
  shutdown). OS-blocked filesystem/enumeration worker threads cannot be forcibly
  killed by Tokio; these deadlines bound async waits, not kernel scheduling.

The regression evidence for this Host path is offline: loopback mock sockets,
synthetic USB inventories, and Unix virtual serial ports. The J4 HIL evidence
below does not validate this legacy Host endpoint.

## Verification evidence

The current x1.2 board evidence is recorded in
[2026-10-21 x1.2 ADC/pinout/J4 HIL](../testing/results/2026-10-21-x1.2-adc-pinout-j4-hil.md).
It covers the embedded xterm.js terminal, the finite responder matrix above,
CDC + WS + two TCP sinks including a slow sink, accepted/complete ownership, raw
TCP 5555, and J4 recovery after a GP10 logic-analyzer capture. Its supported-rate
BER covers only 4800/5200/115200; rates above 115200 are rejected and the
high-rate samples are diagnostic, not product PASS rows. Read the report
limitations before treating it as electrical or worst-case timing
certification.
