# WebUI Sigrok Bridge Architecture

## Overview

The Web UI communicates with the firmware logic analyzer using the sigrok binary
protocol over a WebSocket transport.

## Architecture

### Web UI Control Flow

```
Browser                          Firmware
   │                                │
   │──── POST /api/v1/live-sessions ──▶│
   │◀──── { ws_url: "/api/v1/ws/0" } ──│
   │                                │
   │──── WebSocket binary frames ──────▶│
   │◀──── WebSocket binary frames ───────│
```

1. Browser creates a live session via `/api/v1/live-sessions`
2. Browser connects to the returned `/api/v1/ws/<slot>` WebSocket URL
3. Browser and firmware exchange sigrok binary protocol frames over WebSocket

### Firmware WebSocket Binary Frame Routing

The firmware WebSocket endpoint (`/api/v1/ws/<slot>`) routes frames based on type:
- **TEXT frames**: JSON for power telemetry, status snapshots (existing path)
- **BINARY frames**: Sigrok protocol (new path)

Binary frames are routed directly to the sigrok protocol handler without conversion.

## Protocol

The Web UI uses the sigrok binary protocol as documented in `docs/reference/sigrok-linkr-v2.md`.

Key protocol parameters for Web UI:
- **CONFIG v1 pre/post**: uint16; bounded captures use 1..65535 and stream uses
  post=0
- **HELLO capability flags**: bit 0 (CONFIG_V2) advertises CONFIG_V2_REQ support;
  bit 1 (GENERIC_PACKED_BURST) advertises the unified generic packed-burst
  architecture with exact 100000-sample lossless capture at high rates
- **CONFIG_V2**: after HELLO advertises server_flags bit 0, frame0x0b
  carries u32LE pre/post and is required for bounded post > 65535. With bit 1,
  bounded pre=0 and post=65536..100000 uses the common packed pipeline at every
  otherwise supported rate and pin plan. WIDE11 at 125 MHz remains invalid.
- **pre_samples**: Always 0 (pre-trigger not exposed in Web UI)
- **post_samples**: 1..65535 for ordinary bounded captures, 0 for stream mode
  (at lower rates runs until user-stop; at supported high rates with GENERIC_PACKED_BURST
  captures exactly 100000 samples losslessly then auto-STOP/drain)
- **Other large configs**: configurations not in the supported matrix receive
  CONFIG_RESP, then START returns INVALID_CONFIG
- **Supported modes**: FAST8 (GP10-17), WIDE11 (GP10-GP20, 11 channels; GP29 excluded from LA)
- **GP7-GP9**: Not available in Sigrok modes

## Key Implementation Notes

### Concurrency Control

- Only one sigrok session at a time across both WebSocket and raw-TCP 5556 transports
- If TCP 5556 has an active session, WebSocket binary frames return BUSY error
- If WebSocket has an active session, raw-TCP 5556 connections return BUSY

### Session Lifecycle

- Live session created via `/api/v1/live-sessions` must be explicitly closed
- Session expiration is automatic in firmware for unused sessions
- The firmware supports up to four concurrent WebSocket clients

### Data Flow

- Firmware sends DATA frames with 8-byte metadata header
- Sample indices are modulo 24 bits
- On overrun, possible overrun, or bounded-capture completion, firmware sends
  terminal EVENT and stops
- Generic packed-burst acquisition is store-and-forward: with HELLO bit1,
  supported high-rate post=0 captures exactly 100000 samples locally and sends
  DATA frames after acquisition completes

## File Reference

### Firmware
- `linkr_debugger_ws.c` — WebSocket infrastructure with binary frame routing
- `linkr_debugger_sigrok_linkr.c` — Sigrok protocol handler (TCP 5556 and WebSocket)

### Web UI
- `web/src/components/LogicAnalyzerCard.tsx` — Logic analyzer UI component
- `web/src/lib/sigrokClient.ts` — Sigrok protocol client for WebSocket

## Transport Properties

The sigrok-over-WebSocket path provides:

- Standard sigrok binary protocol (shared with native PulseView/sigrok-cli)
- Better protocol efficiency (binary vs text)
- Consistent protocol across Web UI and native integrations
- Pre-trigger not exposed in Web UI (simpler contract)

## See Also

- `docs/reference/sigrok-linkr-v2.md` — Sigrok protocol specification
- `docs/reference/logic-analyzer.md` — Logic analyzer overview
- `docs/reference/capture-trigger-architecture.md` — Hardware capture architecture
