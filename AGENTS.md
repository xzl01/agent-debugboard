# Agent Development Notes

## Board hardware description ownership

Keep board-level hardware descriptions in Device Tree whenever Zephyr bindings
and the board model can express them. Only define hardware facts in firmware C
code when they cannot be represented cleanly in Device Tree. Never define
board-level hardware descriptions, pin maps, rail maps, ADC channel maps, or
schematic-derived hardware facts in the host CLI.

### Hardware default state

Boot-time default state (power rails on/off, switch/mux routes, GPIO directions)
belongs to the firmware side — either in Device Tree (`regulator-boot-off`,
`gpio` initial states) or in firmware init code. The host CLI/TUI must read and
reflect the actual hardware state via status polling instead of imposing its own
defaults. When defaults are coordinated across multiple outputs (e.g. USB mux
route must match `5v_ws` regulator state at boot), make them consistent in the
firmware boot path, not in the client.

## Upstream/public repository boundaries

Keep fixes repo-local. Do not modify Zephyr itself, Rust toolchain crates, west
modules, or any shared/public upstream repository code unless the user
explicitly asks for upstream work. In particular, never solve this repository's
problems by patching sibling `zephyr/`, `modules/`, or other shared checkout
code when the intended change belongs in this repository.

## HIL functional test requirements

Firmware and host control logic changes must not be considered complete based only on compilation, static analysis, or unit tests. When a change affects real hardware behavior, the author must also perform a board-level HIL functional test.

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

Detailed checklist and procedures are maintained in `doc/testing/hil-functional-test-spec.md`.

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
  `west build -p always -b rpi_pico2/rp2350a/m33/mcuboot --sysbuild apps/radxa_linkr_debugger -d build/radxa_linkr_debugger`

Do not run single-object or single-driver firmware compile checks; they disturb
the user's build/cache workflow. Use the full firmware/package workflow instead.

The 2026-05-25 CI run `26400376587` is the reference failure: the firmware job
was green, but CI still failed because `cmd-ng/src/app.rs` had rustfmt drift and
`skills/radxa-linkr-debugger/scripts/install.ps1` contained invalid PowerShell
function names with spaces, such as `Get-AgentLinkr DebuggerArch`.
