# 2026-08-25 cmd-ng TUI Safety Final HIL

## Verdict

**Functional HIL: PASS. Release acceptance: FAIL because protected PID 3731563 was absent at the required final check.**

The current rebuilt `cmd-ng` release passed the focused real-board safety matrix
through task-owned node-pty, xterm.js, Chromium, and a deny-by-default loopback
proxy. The proxy forwarded only read-only GET requests and locally intercepted
every TUI mutation. No mutation was forwarded to the board.

The exact final command `ps -p 3731563 -o pid=` returned exit status 1 with empty
output. No input, signal, resize, attach, `/proc` inspection, tmux operation, or
protected terminal reuse targeted PID 3731563 or `pts/1`; no replacement protected
process was created.

## Scope

This run validates the latest host-side TUI safety fixes against a real G3 board:

1. GPIO keyboard `l` followed by one queued keyboard burst containing PageUp
   navigation and Enter/Space activation attempts on Power/Switch controls.
2. GPIO keyboard and mouse pending states followed by queued Power/Switch input.
3. Saved Config Save and Clear busy states followed by queued row/tab/key input.
4. Hardware confirmation invalidation after authoritative status removes the
   Power target or Switch route.
5. Saved Config confirmation invalidation after authoritative item removal and
   danger-classification change.
6. Dynamic C0, DEL, ESC/OSC, and CJK strings rendered through the real terminal.
7. Existing modal-before-request, redraw, stale-hit, and stale-refresh cases.

No firmware flash, BOOTSEL, OTA, task mutation, persistent-config mutation, or
historical report/evidence edit was performed.

## Target And Artifacts

| Item | Value |
| --- | --- |
| Board | G3 RP2350A, HTTP `http://172.29.203.1` |
| Firmware CDC | `/dev/serial/by-id/usb-Radxa_Radxa_Linkr_Debugger_E6641C31A362C336-if00` -> `/dev/ttyACM2` |
| Git HEAD | `a03ef6206d38fe797d3e874c37d7431f263a9ca5` |
| cmd-ng source inputs | 154 Cargo/Rust files |
| Source manifest SHA-256 | `8a90ea3be17253ff124cf7c5db60c1fe77eb6563ccd0035ad5d4c0d72cf01bc0` |
| Tracked cmd-ng diff SHA-256 | `d6b8b43c8ade14296d2d78f5ff9f6bc885328a41835aacf66d73619e08ffd7bc` |
| Current release | `cmd-ng/target/release/radxa-linkr-debuggerctl`, version `0.2.1` |
| Release SHA-256 | `2f82a35ae72ad9fdfb5a720044a0b03de325d8bddb14d1cc857d77778f5572a9` |
| Release size | 12,971,896 bytes |
| Combined UF2 SHA-256 | `f08c7580cc3aa634996fc702e5219a2e49826a572ee97193c1a5ae284320fb06` |
| Combined UF2 size | 1,594,368 bytes |
| Evidence | `.omo/evidence/20260825-tui-safety-final-release/` |

The release was rebuilt with:

```text
cargo build --release --locked --manifest-path cmd-ng/Cargo.toml
```

The combined UF2 was verified byte-identical to the previous frozen artifact and
was not used for flashing.

## Preserved Baseline

The pretest board projection reported:

- Power: `12v_out=off`, `5v_out=on`, `vdd_5v=on`, `20v_out=off`.
- Switches: `sd=usb-reader`, `usb=pc`, `tf_wp=protected`, `vin=3.3v`.
- GPIO: `GP10=output/0`; every other advertised GPIO was `input/0`.
- Watchdog: supported, automatic, healthy, armed, 5,000 ms timeout, no failing service.
- Persistent configuration: v1 snapshot present, six selected entries, `pending=0`.
- Tasks: zero stored tasks, zero-byte task blob, and the firmware task catalog captured separately.

The initial GP10 read found `input/1`. The only permitted restoration action was
one direct `PUT /api/v1/gpio/GP10` to `{"direction":"output","value":0}`;
the read-back converged to `output/0`. No persistent configuration or other
hardware output was touched.

| Projection | Pretest SHA-256 | Final SHA-256 |
| --- | --- | --- |
| Hardware/config/task projection | `4b1aa04843207d1399575e9043fd71fa5d84dabcd46fab18ad26f4c906f48bcd` | same |
| Normalized config GET | `84a7e9ac83705cde82b15991dfd44382f30006a61581e43b10fbd0ac9e29e7d7` | same |
| Normalized stored tasks GET | `320524b11c7ecac0929fb8d60bd186ee0c8999f1769ce0789bcfd7e3bd7c1e2b` | same |
| Normalized task catalog GET | `7f56c75dfe3f585a2437058974cc94fd939c3197b4a48304dc709fc127a922c8` | same |
| Decoded task blob | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` | same |

`final/comparison.json` reports `baselineEqual=true`, `taskBlobEqual=true`,
`sourceEqual=true`, `sourceDiffEqual=true`, `releaseEqual=true`,
`combinedUf2Equal=true`, `watchdogHealthy=true`, and `watchdogArmed=true`.

## Real Terminal Safety Matrix

The release ran through task-owned node-pty `1.1.0`, xterm.js `6.0.0`, Playwright
Core `1.62.1`, and Chromium `151.0.7922.173`. Chromium keyboard and CDP pointer
events were converted by xterm.js into real terminal bytes. Queued browser bursts
were delivered as single PTY writes. No tmux, ANSI-to-HTML substitute, pasted
screen, direct model call, or mock board data was used.

| Group | Cases | Result | Evidence |
| --- | ---: | --- | --- |
| GPIO keyboard pending | 2 | PASS | `l` + queued Power/Switch keyboard activation; one local GPIO PUT, no Power/Switch mutation |
| GPIO mouse pending | 1 | PASS | queued Power/Switch SGR Down remained inert during `[HIGH...]` pending state |
| Saved Config busy | 2 | PASS | Save/Clear busy redraw blocked queued row/tab/key input; one local request each |
| Stale hardware confirmation | 2 | PASS | authoritative status removed Power/Switch target; modal cleared and queued confirm was inert |
| Stale Saved Config confirmation | 2 | PASS | authoritative item removal and danger change cleared pending confirmation |
| Dynamic strings | 1 | PASS | C0/DEL/ESC/OSC injected in real status; screen text had no control bytes and CJK rendered |
| Existing modal/redraw/stale-hit matrix | 4 | PASS | Power/Switch modal boundary and transformed stale Power/Switch hit cases |

The harness produced 14 PASS cases, 41 full-height PNG/text/ANSI frame sets, and
11 trusted browser-generated queued PTY bursts. All PTYs exited with code zero,
none used `/dev/pts/1`, and the task-owned runtime and browser were closed after
the run.

## Request Safety Audit

The final proxy recorded 87 exact TUI requests:

| Class | Count | Board effect |
| --- | ---: | --- |
| Forwarded GET | 80 | Read-only status, ADC, config, and switch authority |
| Intercepted GPIO PUT | 3 | Local synthetic success; not forwarded |
| Intercepted Config DELETE/PUT | 2 | Local synthetic success; not forwarded |
| Intercepted Power/Switch PUT | 2 | Local synthetic success; not forwarded |
| Forwarded mutation | 0 | None |
| Unexpected/denied request | 0 | None |

The exact ledger is `tui/all-requests.ndjson`; each request records method, path,
body hash, timestamps, forwarding decision, response status/hash, and duration.
No Power, Switch, GPIO, Config Save/Clear, task, OTA, BOOTSEL, or other TUI
mutation reached the board.

## Visual Evidence

`validate-safety-evidence.mjs` checked all 41 fresh frames using the visual-QA
`tui-check` command. Every frame had the expected full-screen PNG dimensions,
complete row count, nonblank keybar, ANSI capture, no overflow, and
`borderMisaligned=false`. The dynamic CJK frame reported expected wide-character
columns; its screen text contained no C0, DEL, ESC, or other control byte.

## HTTP, WebSocket, CDC, And Final Equality

- Final curl status: HTTP 200, 5,211 bytes, valid `radxa-linkr-debugger.v1` success envelope.
- Final release CLI status: valid `radxa-linkr-debugger.v1` success envelope.
- Final bounded release WS read: one telemetry record with four readings,
  `sample_sequence=13`, `device_t_mono_us=76610844600`.
- Final CDC at 115200 baud: read-only `vin get` returned `vin=3.3v`.
- Final hardware/config/task projection and zero-byte task blob remained equal to pretest.
- Watchdog remained healthy and armed.

## Protected Process Postcondition

The required final command was run exactly once after the matrix and before
checksum freeze:

```text
ps -p 3731563 -o pid=
```

It returned exit status 1 with empty stdout/stderr. The required protected PID
was absent. No attempt was made to inspect, restart, replace, signal, attach to,
resize, or send input to that process, and no operation reused `pts/1`.
Therefore the functional product result is PASS, but release acceptance is FAIL.

## Evidence Index

- Pretest source/artifact/baseline: `pretest/summary.json`,
  `pretest/source-files.SHA256SUMS`, `pretest/cmd-ng-source.diff`, endpoint
  captures, GP10 restoration record, and normalized hardware/config/task projection.
- Real TUI matrix: `tui/harness-summary.json`, `tui/validation-summary.json`,
  `tui/all-requests.ndjson`, and the 14 case directories.
- Visual checks: per-frame `*-tui-check.json`, original PNG/text/ANSI files,
  and `validate-safety-evidence.mjs`.
- Transport/equality: `final/health-summary.json`, CDC captures,
  `final/comparison.json`, and final endpoint captures.
- Cleanup/protected process: `final/cleanup-summary.json` and
  `final/user-pid-check.json`.
- Integrity: `SHA256SUMS` and `checksum-validation.txt`.
