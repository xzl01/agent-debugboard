# 2026-08-30 Logic Analyzer v1 SINGLE Fast-Path HIL

Date: 2026-08-30

## Verdict

**GO for the repository standard SINGLE continuous-stream contract.** The
candidate restores sustained 1 MHz on WebSocket and raw TCP under the strict
95%-duration, STOP response, zero-gap, zero-disconnect, zero-overrun,
immediate-restart, and HTTP-health contract.

**NO-GO for claiming worst-case incompressible 1 MHz transport headroom.** The
available GP10 UART stimulus path was not used because /dev/ttyACM1 was held
exclusively by a user-owned rustty session. The HIL did not kill, interrupt, or
inject into that session. All standard continuous cases observed highly
compressible input, so a separate high-transition run remains required before
claiming lossless 1 MHz for adversarial or near-random input.

The board was restored to the archived canonical production image after HIL.
No commit, push, fault injection, power-output mutation, switch mutation, or
application-only UF2 flash was performed.

## Target and artifacts

- Board: Radxa Linkr Debugger G3, RP2350A
- HTTP: http://172.29.203.1
- Firmware CDC: /dev/serial/by-id/usb-Radxa_Radxa_Linkr_Debugger_E6641C31A362C336-if00
- Candidate profile/version: production, 0.3.0+0
- Candidate build id: v0.2.1-215-g96d2aa896e31-dirty
- Candidate combined UF2: 1,593,344 bytes
- Candidate combined UF2 SHA-256:
  050f7cb87a4e8cd67a99fc30770aa00c075b35578fbeba372d53bd6a8f5b14eb
- Candidate source patch SHA-256:
  499afd2da61ef6b143a2d515100e9056750483f9a4c1f975d025f4b779668d82
- Production restore combined UF2: 1,592,832 bytes
- Production restore SHA-256:
  8d0bc34dfdbcdbe91fa4e5902772965751fbddd626c3c964f3aad5bd7fa0219d
- Production restore archive:
  2026-08-28-watchdog-rollback-fault-injection-hil/production/radxa-linkr-debugger-rp2350.uf2

Exact identities are retained in
[artifact-identities.txt](./2026-08-30-logic-analyzer-v1-fastpath-hil/artifact-identities.txt).
The source patch that produced the candidate is retained in
[candidate-source.patch](./2026-08-30-logic-analyzer-v1-fastpath-hil/candidate-source.patch).

## Preflight and candidate startup

The online pre-HIL image matched the archived canonical production baseline:

- build profile/version: production, 0.3.0+0
- CDC uname build id: v0.2.1-208-gc72104d89ca1-dirty
- watchdog healthy and armed
- fault-injection endpoint: HTTP 404
- OTA idle, current image confirmed, no test marker
- persisted configuration count: 6
- power outputs: 12 V off, 5 V on, VDD 5 V on, 20 V off
- routes: SD USB reader, USB PC, TF writable, VIN 3.3 V

HTTP POST /api/v1/bootloader returned success and entered ROM BOOTSEL. The first
UDisks mount attempt hit the known object-registration race:

    Error looking up object for device /dev/sdc1

The bounded retry mounted /dev/sdc1 at <REMOVABLE_MEDIA>/RP2350 and copied only
the candidate combined UF2. The candidate then re-enumerated normally. Its
first CDC boot log contains:

- controller ready over NCM HTTP with CDC ACM fallback
- build id v0.2.1-215-g96d2aa896e31-dirty
- watchdog healthy=1 and armed=1
- no warning, error, fatal, panic, or assertion lines

Evidence:

- [pre-status.json](./2026-08-30-logic-analyzer-v1-fastpath-hil/pre-status.json)
- [candidate-status.json](./2026-08-30-logic-analyzer-v1-fastpath-hil/candidate-status.json)
- [candidate-first-boot-cdc.txt](./2026-08-30-logic-analyzer-v1-fastpath-hil/candidate-first-boot-cdc.txt)
- [candidate-udisks-mount.txt](./2026-08-30-logic-analyzer-v1-fastpath-hil/candidate-udisks-mount.txt)
- [candidate-udisks-mount-retry.txt](./2026-08-30-logic-analyzer-v1-fastpath-hil/candidate-udisks-mount-retry.txt)

## Strict continuous matrix

Each case requested 5 seconds with SINGLE mode, trigger NONE, pre=0, post=0. A
PASS requires at least 95% requested duration, STOP_RESP, zero sample-index
gaps, zero disconnects, zero protocol errors, zero overruns, immediate restart,
and HTTP health after stop.

| Transport | Requested | Samples | Effective samples/s | Frames | Payload bytes | Result |
|---|---:|---:|---:|---:|---:|---|
| WS | 300 kHz | 1,497,088 | 299,339 | 731 | 2,193 | PASS |
| WS | 400 kHz | 1,996,800 | 399,296 | 975 | 2,925 | PASS |
| WS | 500 kHz | 2,498,560 | 499,146 | 1,220 | 3,660 | PASS |
| WS | 1 MHz | 4,995,072 | 998,423 | 2,439 | 7,317 | PASS |
| TCP | 300 kHz | 1,513,472 | 302,344 | 739 | 2,217 | PASS |
| TCP | 400 kHz | 2,017,280 | 403,283 | 985 | 2,955 | PASS |
| TCP | 500 kHz | 2,521,088 | 504,105 | 1,231 | 3,693 | PASS |
| TCP | 1 MHz | 5,038,080 | 1,007,128 | 2,460 | 7,380 | PASS |

All eight cases recorded zero gaps, disconnects, overrun events, and error
events. Every case received STOP_RESP, passed immediate restart, and retained
HTTP health.

The small payload totals show that one-byte RLE compressed the observed static
or low-transition GP10 input strongly. This confirms that the SINGLE unpack
fast path and equal-priority WS/TCP handoff remove the prior approximately
300-350 ksample/s CPU/scheduling ceiling. It does not saturate the full
byte-aligned v1 transport payload budget.

Evidence:

- [candidate-summary.json](./2026-08-30-logic-analyzer-v1-fastpath-hil/candidate-summary.json)
- [ws-1000khz.json](./2026-08-30-logic-analyzer-v1-fastpath-hil/ws-1000khz.json)
- [tcp-1000khz.json](./2026-08-30-logic-analyzer-v1-fastpath-hil/tcp-1000khz.json)
- Remaining per-rate JSON files are retained in the same evidence directory.

## Bounded and embedded-Web regressions

After the continuous matrix, TCP and WS both captured exactly 65,535 samples at
100 kHz with pre=0 and post=65535. Both had zero gaps, disconnects, overruns,
and protocol errors, then passed STOP, immediate restart, and HTTP health.

Evidence:
[bounded-post65535.json](./2026-08-30-logic-analyzer-v1-fastpath-hil/bounded-post65535.json).

The candidate embedded WebUI was opened in Playwright Chromium. The Logic
Analyzer sample-rate select exposed 100 kHz, 300 kHz, 500 kHz, 1 MHz, and the
higher rates. Selecting 300 kHz produced DOM value 300000 with no browser
console error. Embedded decoder JS/WASM also passed HTTP 200, expected MIME
types, gzip content encoding, JavaScript glue reference, and WASM magic.

Evidence:

- [embedded-webui-300khz.png](./2026-08-30-logic-analyzer-v1-fastpath-hil/embedded-webui-300khz.png)
- [decoder-js-headers.txt](./2026-08-30-logic-analyzer-v1-fastpath-hil/decoder-js-headers.txt)
- [decoder-wasm-headers.txt](./2026-08-30-logic-analyzer-v1-fastpath-hil/decoder-wasm-headers.txt)

## High-transition limitation

The documented reduced stimulus path uses /dev/ttyACM1 TX connected to GP10.
During this HIL, /dev/ttyACM1 was held exclusively by a long-running user-owned
rustty -b 115200 /dev/ttyACM1 session. The session was left untouched.
Consequently no 1 Mbaud 0x55 or equivalent high-transition stimulus was
applied.

Evidence:
[high-transition-blocked.txt](./2026-08-30-logic-analyzer-v1-fastpath-hil/high-transition-blocked.txt).

## BOOTSEL recovery and final state

The candidate CDC bootloader command returned:

    Entering rp2350 BOOTSEL in 250 ms...

The firmware CDC device disconnected and ROM BOOTSEL enumerated as the RPI disk.
The first UDisks mount again hit the known registration race; the bounded retry
mounted the partition and copied only the archived canonical production
combined UF2.

After restoration:

- profile/version: production, 0.3.0+0
- CDC uname: v0.2.1-208-gc72104d89ca1-dirty
- app build-version: v0.2.1-208-gc72104d89ca1
- watchdog healthy and armed, fault injection unavailable and unarmed
- fault endpoint HTTP 404
- OTA idle, current image confirmed, test marker absent
- system heap allocation returned to zero
- persisted configuration count remained 6
- build, MCU, USB, power outputs, routes, and watchdog match both the pre-HIL
  snapshot and archived production restoration snapshot exactly
- no RPI BOOTSEL disk remains
- a production WS one-sample capture passed with zero gaps/errors, STOP_RESP,
  immediate restart, and HTTP health

Evidence:

- [restore-cdc-bootsel.txt](./2026-08-30-logic-analyzer-v1-fastpath-hil/restore-cdc-bootsel.txt)
- [restored-first-boot-cdc.txt](./2026-08-30-logic-analyzer-v1-fastpath-hil/restored-first-boot-cdc.txt)
- [restored-uname.txt](./2026-08-30-logic-analyzer-v1-fastpath-hil/restored-uname.txt)
- [restored-build-version.txt](./2026-08-30-logic-analyzer-v1-fastpath-hil/restored-build-version.txt)
- [restored-final-status.json](./2026-08-30-logic-analyzer-v1-fastpath-hil/restored-final-status.json)
- [restoration-comparison.json](./2026-08-30-logic-analyzer-v1-fastpath-hil/restoration-comparison.json)
- [restored-ws-smoke.json](./2026-08-30-logic-analyzer-v1-fastpath-hil/restored-ws-smoke.json)

All retained evidence hashes are listed in
[SHA256SUMS](./2026-08-30-logic-analyzer-v1-fastpath-hil/SHA256SUMS).
