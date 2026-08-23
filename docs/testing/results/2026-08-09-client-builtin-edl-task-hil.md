# 2026-08-09 Client Built-In EDL Task HIL

## Verdict

The client-owned `builtin/edl/12v_out` task: **PASS** on real hardware.

This run verifies that the current Rust CLI executes the EDL template through
five ordinary GPIO/power requests, leaves no task in firmware storage, and
returns the debugger and target to the captured steady state.

## Fixture And Baseline

- Debugger: RP2350 Radxa Linkr Debugger at `http://172.29.203.1`.
- Target: Qualcomm device enumerated as USB `05c6:9008` QDL mode.
- Rail identification: `12v_out` measured about 106 mA; `5v_out` and
  `20v_out` each measured about 9.7 mA.
- Power baseline: `12v_out`, `5v_out`, `vdd_5v`, and `20v_out` on.
- GPIO baseline: `CON_MAS` (`GP7`) input, value 0.
- Firmware task baseline: `task_count=0`, empty blob.
- Watchdog baseline: healthy and armed.

The MASKROM variants and the EDL variants on 5 V or 20 V were not executed:
no compatible target or rail wiring was identified for those combinations.

## Execution

The current source was run through the repository Nix shell:

```sh
cargo run --quiet --manifest-path cmd-ng/Cargo.toml -- \
  --url http://172.29.203.1 --json \
  task run builtin/edl/12v_out
```

The CLI returned:

```json
{"action":"run","command":"task","ok":true,"requests_executed":5,"schema":"radxa-linkr-debugger.v1","task_id":"builtin/edl/12v_out"}
```

## Post-Run Evidence

| Check | Result |
| --- | --- |
| Target mode | PASS: USB `05c6:9008` Qualcomm QDL remained enumerated. |
| Rail state | PASS: `12v_out` returned to on. |
| Target load | PASS: `12v_out` measured about 100 mA after the run. |
| Recovery GPIO | PASS: `CON_MAS` returned to input, value 0. |
| Firmware task storage | PASS: `/api/v1/tasks` remained empty with `task_count=0`. |
| Debugger health | PASS: watchdog remained healthy and armed. |

The task's successful final state matched the captured hardware baseline, so no
additional restoration request was required.

## Local Validation Boundary

The HIL result is distinct from local validation. The same worktree passed the
canonical firmware build, Rust formatting/Clippy/tests, Web tests and production
build, repository gate checks, and `nix flake check -L path:.`.
