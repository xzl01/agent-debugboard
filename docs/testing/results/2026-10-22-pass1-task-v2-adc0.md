# Pass 1 task-v2 and ADC0 source-voltage HIL — 2026-10-22

## Status

**Draft / ADC0 HIL pending.** Task-v2 and network-identity portions are now
covered by [the 2026-10-22 HIL report](2026-10-22-task-v2-network-identity-hil.md).
ADC0 source-voltage HIL and the full Web layout matrix remain pending; this file
is not yet a complete Pass 1 PASS report.

The final combined UF2 hash, flash/RAM resource report, test totals, and
board observations are intentionally left pending in this draft.

## Scope

- Task-v2 storage contract: firmware-owned 128 KiB Settings NVS partition, four
  slots, 4000-byte canonical source and full-set IR limits, source/IR banks,
  manifest revision/UUID/catalog fingerprint/digests, and atomic
  upsert/delete/clear/import.
- Task-v2 HTTP surface: metadata-only `GET /api/v1/tasks`, raw
  `GET /api/v1/tasks/ir`, per-slot source reads, and the strict JSON/base64
  `POST /api/v1/tasks/mutations` endpoint. The removed legacy
  `PUT`/`DELETE /api/v1/tasks` and `DELETE /api/v1/tasks/{id}` are not part of
  this surface.
- Web workflow library and task editor: multiple local workflows, separate
  workflow and task-IR import/export, fresh-browser reopen rules, local-only
  sources over 4000 bytes, and IR-only/built-in limitations.
- ADC0 source-side canonical voltage: Device Tree 115/15 reconstruction of
  DC20V; `raw`/`mv` remain pad diagnostics; `role` is `pd_input` for ADC0 and
  `native` for ADC1-3. No host-side divider conversion.
- Pass 1 resource gate: at least 8 KiB flash and 32 KiB RAM retained by the
  canonical build.

## Required evidence

The executable checklist is
[the HIL functional test spec](../hil-functional-test-spec.md). The final run
must record at least:

- canonical build, combined UF2 flash, normal startup, and the mandatory
  5 V output then fixed PD 20 V preflight;
- Task A and Task B atomic create, revision changes, reboot persistence,
  fresh-profile source reopen, source/IR digest verification, and the
  local-only/IR-only/builtin boundaries;
- CLI task list/store/import/export/source/delete/clear/run behavior and the
  CDC `task show`/`task clear` fallback;
- combined-UF2 BOOTSEL recovery without inventing task migration;
- fixed 5/9/12/15/20 V ADC0 HTTP, WebSocket single, and batch consistency,
  with DMM comparison when available; otherwise a recorded calibration
  limitation;
- Web layout with ADC0 in the PD card and ADC1-3 above routes, with no
  host-side `115/15` multiplication.

## Historical boundary

The 2026-10-21 x1.2 HIL report remains historical and is not rewritten by this
Pass 1 change. Its ADC0 pad-value statement describes that earlier image; the
current canonical ADC0 contract is in
[the ADC telemetry contract](../../reference/adc-telemetry.md).

## Results

Pending final Pass 1 HIL and final artifact identification.
