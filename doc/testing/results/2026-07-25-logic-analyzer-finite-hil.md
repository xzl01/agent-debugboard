# Logic Analyzer Finite HIL Evidence — 2026-07-25

## Build and Flash

Canonical build command:

```sh
west build -p always -b rpi_pico2/rp2350a/m33/mcuboot --sysbuild apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
```

Safe combined UF2 path for ROM BOOTSEL flashing:

```
build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2
```

## Board Endpoints and Setup

- Default device URL: `http://172.29.203.1`
- Sigrok WebSocket transport: `/api/v1/live-sessions` → `/api/v1/ws/<slot>` (binary Sigrok protocol)
- Sigrok raw-TCP transport: port `5556`
- HIL stimulus: GP10 UART at 115200 baud

## Test Artifact SHA-256 Hashes

JSON originals were generated under `/tmp`. The hashes below identify the reviewed output files; the JSON files themselves are not checked in.

| Artifact filename | SHA-256 |
|---|---|
| `linkr-finite-ws-rising-falling-5-100mhz-final.json` | `7201dbcca16d5406f643e22077ed80a47063552dd71ad0afe8b5fb4de8ab8d5e` |
| `linkr-finite-ws-either-5-100mhz-final.json` | `fe20b413bbfecd2dc747da19ff466b0069a7d99a3f0f17ad4fb3be0be753b138` |
| `linkr-finite-tcp-rising-100mhz-final2.json` | `243d242b0fd910db0520f125ffaf030f6465184f0fe44e79cd8f14b2380fb93e` |
| `linkr-finite-ws-none-125mhz.json` | `a88cc1f4c6a2b695f4b5234860daf2b9e9b9d284a3e4fdde7c8249dd6e554474` |
| `linkr-finite-tcp-none-125mhz.json` | `a34084b253cc8a100581290e1966803eb966db26f5fe5506fe34437913ff32aa` |
| `linkr-finite-ws-continuous-1mhz-final2.json` | `4897f0d4007f34ad50b9a8cf4e73d8f015a9ef02f3cb63d3048b60191ccee20a` |
| `linkr-finite-ws-strict-start-100mhz.json` | `11d6f8f80e257a5dbc85e6da67782617af9e138f91acb312a2e84de0c9744532` |
| `linkr-finite-tcp-strict-start-100mhz.json` | `e18a1388d4a1b6c38c3df51d8a98f6018f36b1aced360ccb371a1b464b926239` |

## Pass Summaries

All bounded finite captures used pre=0, post=512 (or post=1 for NONE), SINGLE mode.

| Transport | Trigger | Rate | Result |
|---|---|---|---|
| WS | rising | 5, 25, 50, 100 MHz | PASS — exactly 512 samples, 0 gaps |
| WS | falling | 5, 25, 50, 100 MHz | PASS — exactly 512 samples, 0 gaps |
| WS | either | 5, 100 MHz | PASS — exactly 512 samples, 0 gaps |
| TCP | rising | 100 MHz | PASS — exactly 512 samples, 0 gaps |
| WS | NONE | 125.081 MHz (actual) | PASS — post=1, 0 gaps, restart true |
| TCP | NONE | 125.081 MHz (actual) | PASS — post=1, 0 gaps, restart true |
| WS | continuous | 1 MHz, 5 s | PASS — 4,997,120 samples, 999,340.8 samples/s, zero gaps, zero disconnects |
| WS | strict START ordering | 100 MHz | PASS |
| TCP | strict START ordering | 100 MHz | PASS |

HTTP and CDC BOOTSEL paths passed. Final restoration used only the combined UF2
(`radxa-linkr-debugger-rp2350.uf2`).

## Notes

The eight SHA-256 artifacts above cover the exact finite HIL completed before the
transport cleanup patch. The section below records the post-patch regression for the
current firmware.

## Post-Patch Transport-Cleanup Regression

Canonical sysbuild passed. Final app flash: 657020 B. RAM: 511856 B.
Only `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2` was flashed.

**Forced WS RST after 125MHz NONE/post=1 START**: Issued WebSocket RST immediately after
START on a 125MHz NONE/post=1 session. Two seconds later a fresh session was created,
acquired capture ownership, received exactly one sample at actual 125.081 MHz with
zero gaps, received STOP_RESP, and reported HTTP health.

**Normal WS SINGLE rising 100MHz bounded**: pre=0, post=512, requested 100 MHz.
Exactly 512 samples, trigger index 0, zero gaps. No DATA or EVENT frame appeared before
START_RESP. Immediate restart confirmed. HTTP health confirmed after stop.

**HTTP BOOTSEL**: `POST /api/v1/bootloader` succeeded (picotool lacked permissions;
combined UF2 copied through udisksctl to the RPI-RP2 mount).

**CDC ACM BOOTSEL**: `bootloader` issued on `/dev/ttyACM2`; serial read ended with expected
EIO when USB disconnected and ROM BOOTSEL took over. Same combined UF2 restored normal
HTTP startup.
