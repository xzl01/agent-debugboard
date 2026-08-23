# Final Review HIL - 2026-08-21

## Identity

- Repository `HEAD`: `d464e5b` (`origin/dev` / `dev`)
- Final combined UF2: `radxa-linkr-debugger-rp2350.uf2`
- Final UF2 SHA-256: `f825ccf817bbed9df919868f9a3c14eff39781f81cb35d1b1026c20c5ba1a943`
- Final firmware usage: FLASH `829112 / 847832` (97.79%), RAM `508744 / 532480` (95.54%)
- Dirty-source patch SHA-256: `bf57827e8b84aafd773def1f8fc379fa86ab5d748a7159bb2cde6bad970190af`
- Final untracked-path manifest SHA-256: `7718abffe2deb5d2f6e155719e000bbafac86d344e6a9eaa9a1b62097ed10830`
- Evidence checksums: `2026-08-21-final-review-hil/SHA256SUMS`

`final-tracked-diff.patch`, `final-untracked-source.tar`, and
`final-untracked.paths0` make the source binding self-contained in this
report directory. Earlier dated HIL reports reference older revisions and are
historical evidence only for their own scopes.

## Results

| Area | Result | Evidence |
| --- | --- | --- |
| Firmware catalog | PASS | HTTP catalog contains v1 envelope and 6 tasks |
| Stored task preservation | PASS | `/api/v1/tasks` matched the pre-flash response byte-for-byte |
| Rust CLI catalog | PASS | `task list --json` reports `catalog_available=true` and 6 built-ins |
| Rust CLI execution | PASS | `builtin/maskrom/5v_out --confirm` completed 5 catalog requests |
| EDL execution | PASS | `builtin/edl/5v_out --confirm` completed 5 catalog requests and cleanup |
| Web catalog | PASS | Board-hosted Automation/Tasks rendered all 6 firmware built-ins; `ultimate2-tasks.png`; no dedicated recovery card |
| HTTP/WS | PASS | Status and final WS snapshot returned 15 GPIO entries |
| CDC ACM | PASS | `task show available=true task_count=0` |
| GPIO jitter | PASS | Final board-hosted Web transitions returned 200/200/403; `svgTop=214` and `pinoutTop=200` remained constant across rest, success, restored, and error |
| Responsive GPIO view | PASS | Final embedded Web captures at 375 and 1280 pixels (`ultimate2-gpio.png`): 15 pins, one workspace nav, no body overflow |
| BOOTSEL | PASS | HTTP bootloader entry, `RPI-RP2` mount, and recovery with the same combined UF2 |
| Final hardware state | PASS | 5V and 12V on, all GPIO directions input, watchdog healthy and armed |

## Scope

The run also exercised the catalog-provided MASKROM/EDL request sequence,
including firmware cleanup. GP29 output remained firmware-rejected with HTTP
403 `input_only`; no GPIO was left in output mode. The catalog and stored task
responses, CLI output, status snapshots, WS snapshot, CDC transcript, BOOTSEL
response, and final screenshots are stored beside this report.

## Offline Gates

- Web: Node lane `392 pass / 1 skip`; Vitest `419 pass`
- Rust `cmd-ng`: `369 passed`
- Host tools: `45 passed`
- Firmware/offline model runner: `88 tests`, all passed
- Repository/CI/test-registration contracts: `132 passed`
- Canonical `make firmware`: passed
- Nix, rustfmt, clippy, shellcheck, PowerShell parse, and TypeScript checks: passed
