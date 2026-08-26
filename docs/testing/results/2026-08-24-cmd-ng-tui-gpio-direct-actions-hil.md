# 2026-08-24 cmd-ng TUI GPIO Direct Actions HIL

> Historical evidence for the retired Enter/Space/`0`/`1` and three-button
> mouse contract. It is superseded by the current
> [GPIO Gestures HIL](2026-08-24-cmd-ng-tui-gpio-gestures-hil.md).

## Scope

This run validates the current `cmd-ng` TUI GPIO contract against a real G3
RP2350A Linkr Debugger: explicit LOW, HIGH, and Input actions; authoritative
HTTP readback; keyboard and browser-pointer input through a real PTY/xterm.js;
HTTP and WebSocket health; CDC ACM fallback; and both HTTP and CDC ROM BOOTSEL
entry paths with combined-UF2 recovery.

The run does not treat local tests or mock-board screenshots as hardware
evidence. Local artifacts separately cover the 47/48/80/120-column CJK layout;
the real-board release run also captures an in-flight pending tag.

## Environment

- Date: 2026-08-24
- Board: Radxa Linkr Debugger G3, RP2350A
- Board URL: `http://172.29.203.1`
- Firmware CDC by-id:
  `/dev/serial/by-id/usb-Radxa_Radxa_Linkr_Debugger_E6641C31A362C336-if00`
  (`/dev/ttyACM2` during the run)
- Host binary: current `cmd-ng/target/release/radxa-linkr-debuggerctl`
- Host binary SHA-256:
  `f334968c4b545897a2086c75c9f6974d7ffd14201d9d9cefa0f0a784f6023216`
- Host binary size: 12,976,216 bytes
- Visual transport: current release binary -> node-pty -> xterm.js -> Chromium
- Combined UF2:
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`
- Combined UF2 size: 1,710,080 bytes
- Combined UF2 SHA-256:
  `37d48dd7ecac90834bbf3ef28bf66e7ce6a852f3e62543e1acb2d8e43ee23e71`

## Build And Local Gates

The canonical `make firmware` build passed and embedded the current Web UI.
The application reported FLASH 828,948/847,832 bytes (97.77%) and RAM
508,744/532,480 bytes (95.54%). The build generated both the application-only
`zephyr.uf2` and the combined UF2; only the combined UF2 was used for ROM
BOOTSEL recovery.

Local affected gates also passed:

- Rust fmt, Clippy with `-D warnings`, and 412/412 tests
- Web Node tests 392 passed / 1 skipped, Vitest 427/427, production build
- `nix flake check -L`
- repository, nightly, test-registration, document-layout, skill-boundary,
  and persistent-configuration checks
- fresh xterm.js visual checks at 47, 48, 80, and 120 columns

## GPIO Direct-Action Results

GP13 (`J16_PIN7`) began as `input/0`. The current CLI transport was first used
as a request/readback smoke test, then the current TUI was driven through a real
xterm.js terminal.

| Surface | Action | HTTP authoritative readback | Result |
| --- | --- | --- | --- |
| CLI | `gpio set GP13 0` | `output/0` | PASS |
| CLI | `gpio set GP13 1` | `output/1` | PASS |
| CLI | `gpio input GP13` | `input` | PASS |
| TUI keyboard | Enter | `output/0` | PASS |
| TUI keyboard | Space | `output/0` | PASS |
| TUI keyboard | `0` | `output/0` | PASS |
| TUI keyboard | `1` | `output/1` | PASS |
| TUI keyboard | lowercase `i` | `input` | PASS |
| TUI keyboard | uppercase `I` | `input` | PASS |
| TUI mouse | left-click GP13 cell | `output/0` | PASS |
| TUI mouse | middle-click GP13 cell | `output/1` | PASS |
| TUI mouse | right-click GP13 cell | `input` | PASS |

Each TUI run used `g` and four projected-row Down actions to keep GP13 visible
and selected. Settled terminal captures showed `○ OUT LOW`, `● OUT HIGH`, and
`◌ IN LOW` respectively, with the selection confined to the left GP13 cell and
the GPIO-specific keybar visible. The Enter case ran at 120x32 against the real
board; the remaining action matrix ran at 80x24.

The pending case placed a loopback proxy between the release TUI and the real
board. Status and ADC GETs were forwarded immediately; only the GP13 PUT was
held for two seconds. The 80x24 capture showed the action status
`gpio GP13=1…` and `GP13 ◌ IN LOW [HIGH…]` simultaneously. A direct HTTP GET
confirmed that GP13 was still `input/0` during that frame. The capture then
terminated the TUI and proxy before forwarding the delayed mutation. A normal
release action subsequently completed, refreshed the authoritative board state,
and rendered without the pending tag.

The first mouse automation probe used a fixed row before scrolling GP13 into
view and therefore left-clicked GP14, producing an observed `GP14 output/0`.
No result from that probe was counted as a GP13 pass. The corrected runs first
navigated to GP13, all three GP13 pointer cases passed, and final cleanup
restored GP14 and every other advertised GPIO to input.

Final release evidence is under
`.omo/evidence/20260824-tui-gpio-hil-release/`:

- `enter-low-120/`: real-board Enter LOW at 120x32
- `space-low/`, `key-0-low/`, `key-1-high/`, `lower-i-input/`,
  `upper-i-input/`: the remaining real-board keyboard matrix at 80x24
- `mouse-high/`, `mouse-low/`, `mouse-input/`: real Chromium pointer actions
- `pending-high/`: delayed-PUT real-board pending presentation
- each directory contains `terminal.png`, `terminal.txt`,
  `terminal-ansi.txt`, and `metadata.json` with PTY cleanup receipts

The earlier `.omo/evidence/20260824-tui-gpio-hil/` debug-binary runs were
preliminary and are not used as final release-binary HIL evidence.

## Transport And Recovery

- HTTP status and GPIO endpoints returned valid
  `radxa-linkr-debugger.v1` envelopes throughout the control cases.
- The current CLI recorded one 1 Hz ADC WebSocket sample before flashing,
  after the second recovery, and after the final CDC check.
- CDC `vin get` returned `vin=3.3v` through the identified firmware by-id
  device. Opening CDC caused the expected USB bus re-enumeration; HTTP returned
  under a bounded recovery check.
- HTTP `POST /api/v1/bootloader` returned `ok=true`; ROM enumerated as vendor
  `RPI`, model `RP2350`. The first `udisksctl` mount attempt hit an object
  lookup race for `/dev/sdc1`; no file had been copied. Re-discovery found the
  same strict `VENDOR=RPI` device, it mounted at `/run/media/chen/RP2350`, and
  the combined UF2 restored HTTP and CDC.
- CDC shell `bootloader` independently enumerated the same ROM target. The
  same combined UF2 hash restored HTTP, WebSocket telemetry, CDC, and the
  healthy watchdog a second time.
- The application-only
  `build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.uf2` was never
  used.

## Final State

Final authoritative status after cleanup and the last recovery:

- GPIO: all 15 advertised GPIOs were `direction=input`, `value=0`, including
  GP13, the accidental GP14 probe, and ADC-owned GP29.
- Power: baseline retained as `12v_out=off`, `5v_out=on`, `vdd_5v=on`, and
  `20v_out=off`; this run did not mutate power outputs.
- Switches: baseline retained as `sd=usb-reader`, `usb=pc`,
  `tf_wp=writable`, and `vin=3.3v`; this run did not mutate switch routes.
- Watchdog: supported, automatic, armed, healthy, no failing service.
- Persistent configuration: available, `saved_count=6`, `pending_count=0`.
- HTTP/NCM, one-sample WebSocket recording, and firmware CDC were available.
- No RPI BOOTSEL disk, HIL-owned TUI, visual mock, proxy, or PTY process
  remained. An independent debug TUI on `pts/17` was owned by a long-lived
  user shell, had `/home/chen` as its cwd, and was deliberately left untouched;
  it was not launched or controlled by this HIL run.

## Verdict

PASS. The current TUI performs explicit GP13 LOW, HIGH, and Input actions from
both keyboard and mouse paths, reflects firmware-authoritative state, recovers
through both required BOOTSEL entry paths using only the combined UF2, and
leaves the board in the audited safe GPIO state.
