# 2026-08-25 cmd-ng TUI Saved Config Busy/GPIO Gesture Final HIL

## Verdict

**Functional HIL: PASS. Release acceptance: FAIL because protected PID 3731563 was absent at the final required check.**

The freshly rebuilt current `cmd-ng` release passed the real-board busy/gesture
matrix through task-owned `node-pty`, xterm.js, Chromium, Playwright, and a
deny-by-default loopback proxy. The proxy forwarded only allowlisted read-only
GET requests and intercepted every write locally. **Forwarded mutations: 0.**

The final `ps -p 3731563 -o pid=` check returned exit status 1 with empty output.
No input, signal, resize, attach, `/proc` inspection, tmux operation, or reuse of
the protected terminal was performed.

## Scope

This run validates the Saved Config busy/GPIO gesture fix on the real G3 board:

1. Existing GPIO `Down` followed by keyboard `Save`, `Clear`, and `Refresh`.
2. Existing GPIO `AwaitSecond` followed by keyboard `Save`, `Clear`, and `Refresh`.
3. Each of the six cases waited beyond the relevant 600 ms or 220 ms deadline
   and observed zero GPIO PUT requests.
4. Saved Config `Save`, `Clear`, and `Refresh` followed by one queued browser
   burst containing a row Down, Status-tab Down, and Space key.
5. Busy redraws were captured after settled repaint; no stale GPIO marker or
   action remained, and queued input did not change page or local selection.

No firmware flash, BOOTSEL, OTA, task mutation, persistent-config mutation,
power/switch/GPIO mutation, historical report edit, or protected terminal
operation was performed.

## Target And Artifacts

| Item | Value |
| --- | --- |
| Board | G3 RP2350A, HTTP `http://172.29.203.1` |
| Firmware CDC | `/dev/serial/by-id/usb-Radxa_Radxa_Linkr_Debugger_E6641C31A362C336-if00` -> `/dev/ttyACM2` |
| Git HEAD | `a03ef6206d38fe797d3e874c37d7431f263a9ca5` |
| Current release | `cmd-ng/target/release/radxa-linkr-debuggerctl`, version `0.2.1` |
| Release SHA-256 | `a8fbc4a38aa69b42de1c19c28b659f491f3902b9b2b01dbd49b547a81a0a5637` |
| Release build | `cargo build --release --locked --manifest-path cmd-ng/Cargo.toml` |
| Combined UF2 SHA-256 | `f08c7580cc3aa634996fc702e5219a2e49826a572ee97193c1a5ae284320fb06` |
| Combined UF2 use | Not flashed or used |
| Source manifest SHA-256 | `4ea18eb58645a60ac7b396c4a16375d91856c07f841894f6e6b483b26fc14c73` |
| Tracked cmd-ng diff SHA-256 | `c7c59dbc52641f54077e246b2bd398e127bdfbed10b0b7797d64039345167382` |
| Evidence | `.omo/evidence/20260825-tui-busy-gesture-final-release/` |

The runtime was task-owned `node-pty 1.1.0`, xterm.js `6.0.0`, Playwright Core
`1.62.1`, and Chromium `151.0.7922.173` at
`/etc/profiles/per-user/chen/bin/chromium`.

## Preserved Baseline

The exact observed pretest projection was recorded before the TUI matrix:

- Power: `12v_out=off`, `5v_out=on`, `vdd_5v=on`, `20v_out=off`.
- Switches: `sd=usb-reader`, `usb=pc`, `tf_wp=protected`, `vin=3.3v`.
- GPIO: `GP10=input/1`; the remaining advertised GPIOs were read from the
  firmware response and preserved as observed.
- Watchdog: supported, automatic, healthy, armed, 5,000 ms timeout, no failing
  service.
- Persistent configuration: v1 snapshot present, six selected entries,
  `pending=0`.
- Tasks: zero stored tasks, zero-byte task blob, and catalog captured separately.

The requested reference baseline was `GP10=output/0`, but the board readback was
`input/1` before the run. The no-write requirement prohibited restoration. This
was recorded as `observedBaselineMatchesRequested=false`; the exact observed
projection, config, task blob, source, release, and UF2 were equal before and
after the matrix. No baseline-changing request was forwarded.

| Projection | Pretest SHA-256 | Final SHA-256 |
| --- | --- | --- |
| Hardware/config/task projection | `f9b705660a762fa1baeaba6a937b58617f62f5cc9143e6d284be2cbd7b070fa1` | same |
| Config GET | `8d544c171737254393f863957c483f6de1b542d4e3eae149268c869a6df817e9` | same |
| Stored tasks GET | `320524b11c7ecac0929fb8d60bd186ee0c8999f1769ce0789bcfd7e3bd7c1e2b` | same |
| Task catalog GET | `7f56c75dfe3f585a2437058974cc94fd939c3197b4a48304dc709fc127a922c8` | same |
| Task blob | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` | same |

`final/comparison.json` reports `baselineEqual=true`, `taskBlobEqual=true`,
`sourceEqual=true`, `sourceDiffEqual=true`, `releaseEqual=true`,
`combinedUf2Equal=true`, `watchdogHealthy=true`, and `watchdogArmed=true`.

## Real Terminal Matrix

| Cases | Result | Proof |
| ---: | --- | --- |
| 6: Down/AwaitSecond x Save/Clear/Refresh | PASS | Settled busy redraw had no stale GPIO marker; 220/600 ms deadline produced zero GPIO PUTs |
| 3: queued Save/Clear/Refresh + row/tab/key | PASS | Busy redraw retained Saved Config page and row selection; zero hardware actions |

The nine sessions produced 27 fresh full-height PNG/text/ANSI frame sets. The
six 80x30 cases rendered 1120x1050 `.xterm-screen` PNGs; the three 80x24 cases
rendered 1120x840 PNGs. All 27 `tui-check` results reported `maxWidth=80`, no
overflow, `borderMisaligned=false`, and non-empty ANSI output.

## Request Safety Audit

The final proxy recorded 66 exact TUI requests:

| Class | Count | Board effect |
| --- | ---: | --- |
| Forwarded GET | 60 | Real status, ADC, config, and switch reads only |
| Intercepted Config PUT/DELETE | 6 | Local synthetic responses; no persistent storage change |
| Intercepted GPIO PUT | 0 | No gesture action reached the proxy |
| Forwarded mutation | 0 | None |
| Unexpected/denied request | 0 | None |

The ledger is `tui/all-requests.ndjson`. Every entry includes method, path,
body hash, timestamps, forwarding decision, response status/hash, and duration.
The Refresh cases used a delayed real `/api/v1/config` GET; the delay was local
proxy scheduling and did not alter the response body.

## HTTP, WebSocket, CDC, And Equality

- HTTP curl status: 200, valid `radxa-linkr-debugger.v1` success envelope.
- Current release CLI status: valid success envelope.
- Bounded release WebSocket read: one telemetry record with four readings,
  `sample_sequence=14`, `device_t_mono_us=81001275800`.
- CDC at 115200 baud: read-only `vin get` returned `vin=3.3v`.
- Watchdog remained healthy and armed.
- No BOOTSEL, flash, OTA, reboot, or task operation was attempted.

## Protected Process Postcondition

The final check was:

```text
ps -p 3731563 -o pid=
```

It returned exit status 1 with empty stdout/stderr. The required protected PID
was absent. No attempt was made to inspect, restart, replace, signal, attach to,
resize, or send input to that process, and no TUI session reused `/dev/pts/1`.
Therefore the functional product result is PASS, but release acceptance is FAIL.

## Evidence Index

- Pretest/final source, release, endpoint, and equality evidence: `pretest/`,
  `final/`, `capture-state.mjs`, and `health-check.mjs`.
- Real TUI matrix: `tui/harness-summary.json`, `tui/validation-summary.json`,
  `tui/all-requests.ndjson`, and the nine case directories.
- Each case contains fresh `before`, `busy`, and `final` PNG/text/ANSI captures.
- Visual checks: per-frame `*-tui-check.json` and `validate-evidence.mjs`.
- Protected process: `final/user-pid-check.json`.
- Final integrity freeze: `SHA256SUMS` and `checksum-validation.txt`.
