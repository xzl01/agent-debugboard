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

The Web UI uses the sigrok binary protocol as documented in `doc/sigrok-linkr-v1.md`.

Key protocol parameters for Web UI:
- **CONFIG pre/post**: uint16, bounded 1..65535
- **pre_samples**: Always 0 (pre-trigger not exposed in Web UI)
- **post_samples**: 1..65535 for bounded captures, 0 for stream mode
- **Supported modes**: FAST8 (GP10-17), WIDE12 (GP10-20+GP29)
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

- `doc/sigrok-linkr-v1.md` — Sigrok protocol specification
- `doc/logic-analyzer.md` — Logic analyzer overview
- `doc/capture-trigger-architecture.md` — Hardware capture architecture
