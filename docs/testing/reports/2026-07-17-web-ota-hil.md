# Web OTA HIL Execution Report — 2026-07-17

## Board and Build Identity

| Field | Value |
|---|---|
| Hardware | Radxa Linkr Debugger G3 (RP2350A) |
| Firmware | Canonical sysbuild, `build/radxa_linkr_debugger/` |
| Combined UF2 | `radxa-linkr-debugger-rp2350.uf2` |
| OTA payload | `build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin` |
| HIL date | 2026-07-17 |
| Board URL | `http://172.29.203.1` (port 80) |

## HIL Summary

| Step | Result |
|---|---|
| Canonical sysbuild passed | PASS |
| Combined UF2 flashed via BOOTSEL | PASS |
| HTTP API preflight read-only checks | PASS |
| Web UI auto-confirm flow | PASS |
| Web UI manual-confirm flow | PASS |
| Bad SHA256: HTTP 400 `sha256_mismatch` | PASS |
| Bad Content-Type: HTTP 415 `unsupported_content_type` | PASS |
| Upload interruption recovery | PASS |
| HTTP BOOTSEL entry | PASS |
| CDC ACM BOOTSEL entry | PASS |
| Final board idle and confirmed | PASS |
| Watchdog armed and healthy | PASS |
| Watchdog rollback | **BLOCKED** |

## Detail

### Build and flash

The canonical RP2350 sysbuild completed successfully. The combined MCUboot-plus-application UF2 was flashed via ROM BOOTSEL using the RPI vendor discovery method (VENDOR=RPI via `lsblk`, not a hard-coded `/dev/sdX`). The board rebooted normally and HTTP API responded within the bounded retry window.

### HTTP API preflight

Read-only preflight against port 80 endpoints returned valid JSON for `/api/v1/status`, `/api/v1/watchdog`, and `/api/v1/ota`. No side effects.

### Web UI auto-confirm flow

The browser runner (`web/scripts/ota-hil.mjs --execute --flow auto`) opened the board Web UI at `http://172.29.203.1/`, navigated to **Advanced & recovery**, selected the MCUboot-format `.bin` OTA payload, computed SHA-256 locally in the browser, uploaded it via `POST /api/v1/ota/upload`, triggered **Start test boot** with auto-confirm dialog acceptance, and polled until the OTA state reached `idle` with `current_image_confirmed=true`. The ~16-second watchdog health gate elapsed without a reset and firmware auto-confirmed the image.

### Web UI manual-confirm flow

`node scripts/ota-hil.mjs --execute --flow manual` exercised the same upload and test-boot sequence but manually clicked **Confirm image** after the `pending_test` state appeared, instead of waiting for the auto-confirm gate. State reached `idle` with `current_image_confirmed=true` after manual confirmation.

### Negative upload: bad SHA256

HTTP POST with `X-Linkr-Ota-Sha256: 0000...0000` (all-zero) returned HTTP 400 with `error.code: "sha256_mismatch"`. OTA state remained `idle`; no corrupted state was written.

### Negative upload: bad Content-Type

HTTP POST with `Content-Type: text/plain` returned HTTP 415 with `error.code: "unsupported_content_type"`. A subsequent `GET /api/v1/ota` confirmed `last_error.code` was preserved as `unsupported_content_type` and was not overwritten by a subsequent successful upload or idle transition.

### Upload interruption recovery

An upload was started and interrupted mid-transfer (Ctrl+C equivalent). Subsequent `GET /api/v1/ota` showed `state` as `idle` or `failed`, not `uploading`. The board did not retain partial upload state.

### HTTP BOOTSEL entry

`curl -fsS -X POST http://172.29.203.1/api/v1/bootloader` triggered ROM BOOTSEL entry. The board's USB connection dropped and re-enumerated as an RPI vendor disk within the bounded retry window.

### CDC ACM BOOTSEL entry

Writing `bootloader\n` to the CDC ACM tty (`/dev/ttyACM0`) triggered the same ROM BOOTSEL entry path. The board re-enumerated as an RPI vendor disk.

### Final board state

After recovery flashing of the canonical UF2, the board returned to `idle` OTA state, `current_image_confirmed=true`, and `/api/v1/watchdog` reported the watchdog as armed and healthy. GPIO25 heartbeat LED blinked at approximately 1 Hz.

### Watchdog rollback

**BLOCKED**: No safe fault-injection path exists to intentionally wedge core liveness and trigger a watchdog reset during the unconfirmed test window. The current runners can confirm the happy-path auto-confirm and manual-confirm flows, but cannot produce a controlled rollback event without ad hoc destructive methods. Do not imply rollback behavior itself was HIL-verified.

## Runners Used

| Runner | Path | Mode |
|---|---|---|
| Shell API runner | `docs/testing/scripts/web-ota-hil.sh` | Dry-run plans + executable gates |
| Browser runner | `web/scripts/ota-hil.mjs` | Playwright-driven Chromium, `--execute --flow auto` / `manual` / `both` |
| Unit tests | `web/scripts/ota-hil.test.mjs` | `npm test` (included in standard web test flow) |

## Notes

- All API runner examples in documentation use port 80 at `http://172.29.203.1` as the default board URL.
- The browser runner dynamically loads playwright-core and accepts `--playwright-module` and `--chromium-executable`; it does not require global installation.
- The `flash_uf2` function uses RPI vendor discovery (`VENDOR=RPI` via `lsblk`) and parses the udisksctl mount point with `xargs printf '%s'` to preserve spaces in valid paths, rather than stripping all whitespace.
- The `web-ota-hil.sh` shell runner was added to CI shellcheck and `sh -n` in this same change.
