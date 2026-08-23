# 2026-08-18 Web GPIO Direct Gestures HIL

## Verdict

Real-board validation of the board-hosted Web GPIO direct-gesture interface:
**PASS**.

The final HIL runner reported 55/55 assertions passing with no mocked API and
no failed cleanup. GP13 was restored to input, every safe GPIO finished in
input mode, and the original persistent snapshot and hardware baseline were
preserved.

## Source And Artifacts

- Source HEAD: `97b24eb959d11e9baa65895e715e0913d3039dee`.
- The worktree was intentionally dirty; the tested firmware was built from the
  current working tree with the final Web GPIO implementation embedded.
- Canonical build command: `make firmware`.
- Combined MCUboot plus application UF2:
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`, 1,643,520
  bytes, SHA256
  `f2f9c2e7bb512af10dcc4d4a2519257f1f3a7a2e116148f98406a430bec14661`.
- MCUboot OTA BIN: 795,904 bytes, SHA256
  `54064fb65585267a2febde8c0798c6b6af82db81ecb8004683bf8fff465c3de4`.
- The application-only `zephyr.uf2` was never flashed.

## Flash And Startup

`POST /api/v1/bootloader` returned the expected RP2350 BOOTSEL response. The
existing `web-ota-hil.sh` helper timed out before the RPI disk appeared and its
partition matcher did not accept the padded `VENDOR="RPI     "` value. The
device subsequently enumerated as `/dev/sdc1`, vendor `RPI`, model/label
`RP2350`.

The partition was mounted with `udisksctl`; only the canonical combined UF2
above was copied. Normal NCM HTTP and CDC ACM enumeration returned immediately.
The embedded `/assets/app.js` contained the final `Tap: output low`,
`Double-tap: input`, and `Hold: output high` strings.

## Starting Baseline

Independent HTTP reads before and after flashing established:

- all safe GPIOs were input;
- `5v_out=on`, while `12v_out`, `vdd_5v`, and `20v_out` were off;
- SD and USB routes were `target`, TF-WP was `writable`, and VIN was `3.3v`;
- the v1 snapshot contained exactly `power/5v_out=on`, with `pending=0` and
  `apply_state=applied`;
- watchdog was healthy and armed;
- OTA was idle and the running image was confirmed.

## Board-Hosted Browser HIL

The Playwright runner opened `http://172.29.203.1/` in real Chromium and used
the board page's same-origin HTTP `/api/v1` path. No request interception or
mocking was active. Opening the page and navigating to GPIO emitted no GPIO
PUT and left GP13 input.

| Scenario | Result | Observed contract |
| --- | --- | --- |
| Short press GP13 | PASS | No GPIO PUT before the 220 ms double window; then exactly one `{"direction":"output","value":0}` request and authoritative output/LOW status. |
| Hold GP13 | PASS | Neutral track plus danger progress arc appeared mid-hold; at about 600 ms exactly one `{"direction":"output","value":1}` request was sent; release was inert; status became output/HIGH. |
| Double short press GP13 | PASS | Exactly one `{"direction":"input"}` request with no preceding output-LOW request; status returned input. |
| Keyboard Enter, Space, `0` | PASS | Each issued one output-LOW request after the prior request settled. |
| Keyboard `1` | PASS | Issued one output-HIGH request. |
| Keyboard `I` and `i` | PASS | Each issued one input request. |
| Movement beyond 8 px | PASS | Gesture cancelled with no GPIO write. |
| `pointercancel` | PASS | Gesture cancelled with no GPIO write. |
| Pending state | PASS | Target exposed `aria-busy=true` and a warn dashed busy ring; UI retained the pre-request state until authoritative refresh. |
| GP29 output attempt | PASS | Firmware returned HTTP 403 `input_only`; the inline alert showed the firmware error and GP29 remained input. |

For every successful mutation, the captured sequence showed the board PUT,
followed by an authoritative status response, followed by the matching UI
direction/level. The interface did not apply optimistic hardware state.

## CDC And BOOTSEL Coverage

- HTTP BOOTSEL entry and combined-UF2 recovery passed as described above.
- CDC ACM read-only fallback used
  `/dev/serial/by-id/usb-Radxa_Radxa_Linkr_Debugger_E6641C31A362C336-if00`.
  The `help` command returned the Zephyr shell command catalog, including
  `bootloader`, `config`, `task`, `tf_wp`, and `vin`.
- CDC BOOTSEL was not repeated in this focused run and is not claimed here.

## Final Cleanup

The HIL runner used a `finally` cleanup path that directly requested GP13
input. Independent final reads proved:

- every safe GPIO, including GP13 and GP29, was input;
- power, switches, VIN, and the persistent snapshot exactly matched the
  starting baseline;
- watchdog remained healthy and armed;
- OTA remained idle and confirmed;
- no browser or preview process remained;
- no RPI BOOTSEL block device or mount remained.

No power output, switch route, VIN setting, persistent snapshot, or newly
discovered dangerous value was modified or confirmed by this HIL.

## Offline And Visual Evidence

Before the board run, the Web lane passed:

- Node: 336 passed, 1 pre-existing skip, 0 failed;
- Vitest: 315 passed across 41 files;
- TypeScript and production Vite/WASM build;
- test registration and LSP error checks.

Separate production-loopback visual and interaction evidence is archived under
`.omo/evidence/gpio-direct-gestures/`: 74/74 assertions passed across
1280/768/375 px, English/Chinese, light/dark, hold, focus, pending, and error
states. Both independent visual Oracle reviews returned PASS with HIGH
confidence. That mock evidence validates responsive presentation; the 55/55
run documented here validates actual board behavior.

## Evidence Files

- `.omo/evidence/gpio-direct-gestures-hil/report.json`: 55/55 HIL assertions.
- `.omo/evidence/gpio-direct-gestures-hil/network.json`: complete browser
  request/response log, including GP13 PUTs and GP29 HTTP 403.
- `.omo/evidence/gpio-direct-gestures-hil/post-cleanup.json`: final independent
  state and cleanup proof.
- `.omo/evidence/gpio-direct-gestures-hil/cdc-transcript.txt`: CDC fallback
  transcript.
- `.omo/evidence/gpio-direct-gestures-hil/make-firmware.log`: canonical build.
- `.omo/evidence/gpio-direct-gestures-hil/artifacts.SHA256SUMS`: tested artifact
  identities.
- `.omo/evidence/gpio-direct-gestures-hil/http-bootsel.log`,
  `udisks-mount.log`, and `copy-combined-uf2.log`: BOOTSEL and flash evidence.
- `.omo/evidence/gpio-direct-gestures-hil/gpio-mounted.png`,
  `hold-mid-gp13.png`, `pending-gp13.png`, and `gp29-rejection.png`: real-board
  UI states.

Checksums are in
[`2026-08-18-web-gpio-direct-gestures-hil.SHA256SUMS`](2026-08-18-web-gpio-direct-gestures-hil.SHA256SUMS).
