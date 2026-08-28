# 2026-08-28 Current Source Validation

## Scope

This receipt records the follow-up worktree state for the 2026-08-28 review.
The Saved Config mouse/UX interaction was implemented and tested in the
2026-08-26 typed-mouse and configuration-safety work; this follow-up keeps the
historical 2026-08-26 receipt unchanged and records the 2026-08-28 checks
separately.

## Results

- `apps/radxa_linkr_debugger/tests/run_unit_tests.sh`: **PASS**; the new
  `linkr_debugger_task_blob` host test is registered and passed.
- Web tests: **59 files, 433 tests passed**, with one existing skip; the new
  WebSocket cleanup-race regression test is included.
- Web production build: **PASS**.
- Full firmware build (`make firmware`): **PASS**; combined MCUboot+app UF2
  produced. FLASH 90.99%, RAM 88.16%.
- Repository governance and test-registration gates: **PASS**.

## HIL Status

The 2026-08-28 real-hardware HIL attempt started blocked by restricted device
nodes, then passed after host access was opened. The canonical combined UF2
was flashed and the exercised mandatory paths passed: normal boot, HTTP API,
WebSocket live snapshot, task store/list/run/clear, CDC ACM `config show`,
HTTP BOOTSEL, CDC BOOTSEL, and combined-UF2 recovery. See
[2026-08-28-hil.md](2026-08-28-hil.md) for the scoped PASS report and
[2026-08-28-hil-blocked.md](2026-08-28-hil-blocked.md) for the first blocked
attempt.

## Not Rerun

- Full Rust CLI `cargo test` and `nix flake check -L` were not rerun for this
  follow-up because no Rust source changed in the reviewed diff; the 2026-08-26
  receipt remains the reference for those lanes.
