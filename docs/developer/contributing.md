# Contributing

[中文](contributing.zh-CN.md)

## Running unit tests

```sh
./apps/radxa_linkr_debugger/tests/run_unit_tests.sh
```

The test runner covers host C tests for the shared board model, OTA parser,
captive portal behavior, HTTP request-body handling, and logic analyzer
validation. Rust host CLI checks live under `cmd-ng/` and are run with Cargo.

## Rust host CLI checks

```sh
cargo fmt --manifest-path cmd-ng/Cargo.toml --all --check
cargo clippy --manifest-path cmd-ng/Cargo.toml --all-targets -- -D warnings
cargo test --manifest-path cmd-ng/Cargo.toml --all-targets
```

## HIL functional test requirements

Firmware and host control logic changes must not be considered complete based
only on compilation, static analysis, or unit tests. When a change affects real
hardware behavior, the author must also perform a board-level HIL functional test.

A change requires HIL when it touches any of the following:

- RP2350 firmware logic
- host CLI/TUI control logic that talks to real hardware
- power outputs, switch routing, ADC monitoring, safe GPIO, watchdog, or BOOTSEL behavior

Before concluding such a change, the following must be verified on real hardware:

- full canonical firmware build
- flashing and normal board startup
- HTTP/WS control path
- USB CDC ACM serial fallback path
- BOOTSEL entry path
- corresponding functional tests for new features when practical

Detailed checklist and procedures are maintained in
[doc/testing/hil-functional-test-spec.md](../../doc/testing/hil-functional-test-spec.md).

## CI validation

Do not declare CI-ready after validating only the firmware lane. For repository
changes, reproduce every affected GitHub Actions lane locally when practical:

- Rust host CLI formatting: `cargo fmt --manifest-path cmd-ng/Cargo.toml --all --check`
- Rust host CLI checks when touched: `cargo clippy --manifest-path cmd-ng/Cargo.toml --all-targets -- -D warnings` and `cargo test --manifest-path cmd-ng/Cargo.toml --all-targets`
- PowerShell installer parsing/dry-run:
  `pwsh -NoLogo -NoProfile -NonInteractive -Command '[scriptblock]::Create((Get-Content ./skills/radxa-linkr-debugger/scripts/install.ps1 -Raw)) | Out-Null'`
  and, when available, `./skills/radxa-linkr-debugger/scripts/install.ps1 -DryRun`
- Shell installer/test scripts: `sh -n ...` and `shellcheck ...`
- Firmware changes: full canonical build only (one at a time into the shared
  `build/radxa_linkr_debugger/` directory):
  `scripts/build-firmware.sh`

Do not run single-object or single-driver firmware compile checks; they disturb
the user's build/cache workflow. Use the full firmware/package workflow instead.

## Code conventions

- **Device Tree for hardware descriptions**: Keep board-level hardware
  descriptions in Device Tree whenever Zephyr bindings and the board model can
  express them. Only define hardware facts in firmware C code when they cannot be
  represented cleanly in Device Tree. Never define board-level hardware
  descriptions, pin maps, rail maps, ADC channel maps, or schematic-derived
  hardware facts in the host CLI.

- **Standard, consistent, elegant implementations**: Keep software
  implementation standard, consistent, and elegant; avoid ad hoc patterns that
  make maintenance, automation, or documentation harder to follow.

- **MCU-side raw values, host-side interpretation**: Keep MCU-side output as
  close as practical to raw interface values; prefer host-side interpretation,
  calibration, and presentation when that preserves the raw firmware contract.

- **Update docs and skills in the same change**: Any code change must update the
  related skill and documentation in the same change.

- **Preserve BOOTSEL fallback**: Firmware changes must verify and preserve the
  USB CDC ACM serial BOOTSEL fallback path before finishing.
