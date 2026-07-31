# 2026-08-01 Capture Arbiter Regression HIL

## Verdict

**PASS.** Candidate `332004ebb6ad4e6a6940dc33ffca32a79bf53389`
completed both required ROM BOOTSEL recovery legs using the repository-documented
`udisksctl` RPI mass-storage fallback. The candidate was then restored to the
immutable `41a62dcea83766395d636755ab659f639ecb6a8e` baseline with the same
combined UF2-only procedure.

Non-root `picotool` access remains unavailable on this host. This is an
environment note, not a validation blocker: each RPI mass-storage flash copied
only the canonical combined `radxa-linkr-debugger-rp2350.uf2` and completed
normal HTTP and CDC re-enumeration.

## Artifacts

- Candidate and restored-baseline combined UF2: `1455616` bytes,
  SHA-256 `9b06fcc4bb6877f7c04f2d0214a9b367b845e41ebd63f4800d033c1f3ac88e7d`.
- OTA payload: `701944` bytes,
  SHA-256 `fa14c853685ad8bb80ac741d58eb0542305b57ef6f063ce46781d2cb1b5c2c39`.
- Application resource use: `701904 / 847832 B` FLASH and
  `475896 / 532480 B` RAM.
- The application-only `zephyr.uf2` was never selected, copied, flashed, or
  uploaded.

## Validation Matrix

| Phase | Result | Evidence |
| --- | --- | --- |
| Candidate canonical build | PASS | Full canonical sysbuild from `332004eb` matched the saved combined-UF2 hash exactly. |
| HTTP BOOTSEL recovery | PASS | `POST /api/v1/bootloader`, bounded udev/RPI-partition discovery, bounded `udisksctl` mount retry, combined-UF2 copy plus `sync`, RPI disappearance, HTTP startup, and CDC `help` all completed. |
| Bounded WebSocket Sigrok | PASS | Existing HIL runner completed `SINGLE`, 100 kHz, 128-sample bounded capture with `overall_pass=true`, STOP/release semantics, fresh-session restart, and HTTP health `200`. |
| Invalid UF2 OTA | PASS | Temporary empty `.uf2` returned structured `invalid_file`; the combined UF2 was never uploaded and OTA remained `idle` with zero written bytes. |
| CDC BOOTSEL recovery | PASS | CDC returned `Entering rp2350 BOOTSEL in 250 ms...`; the current RPI partition was found with bounded retries, mounted, flashed with the canonical combined candidate UF2, and recovered to verified HTTP and CDC operation. |
| Baseline restoration | PASS | A clean no-hardlinks history-only clone at `41a62dce` was rebuilt, hash-checked, and flashed by the same robust HTTP BOOTSEL plus udisks path. Final HTTP and CDC checks passed. |
| Primary continuity | PASS | Task-1-format pre/post snapshots were byte-identical for HEAD/tree, index content, porcelain-v2 status, tracked and staged binary diffs, untracked paths/content manifest, stash, refs/remotes, and worktrees. The Task 1 baseline status, tracked-diff, index-content, and untracked-manifest hashes also matched. |

## Final Safe State

- Board runs the rebuilt immutable history-only baseline, not the candidate.
- HTTP at `http://172.29.203.1` and CDC ACM fallback both work.
- All controllable outputs are off; `sd=target`, `usb=target`,
  `tf_wp=writable`, and `vin=3.3v`.
- All exposed GPIOs are inputs; OTA is idle; no RPI BOOTSEL device or task
  WebSocket session remains.
- This run makes no persistent-configuration busy claim.

## Follow-up Evidence

Raw receipts are retained under `.omo/evidence/audit-la-history-and-dirty-changes/task-7/follow-up/`:

- `primary-pre/` and `primary-post/`
- `primary-pre-post-comparison.tsv`
- `candidate-build-artifacts.log`
- `candidate-http-bootsel-udisks.log`
- `candidate-sigrok-ws.json`
- `ota-negative-receipt.txt`
- `candidate-cdc-bootsel-udisks.log`
- `baseline-build-artifacts.log`
- `baseline-http-bootsel-udisks.log`
