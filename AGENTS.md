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
route must match `vdd_5v` regulator state at boot), make them consistent in the
firmware boot path, not in the client.

## Upstream/public repository boundaries

Keep fixes repo-local. Do not modify Zephyr itself, Rust toolchain crates, west
modules, or any shared/public upstream repository code unless the user
explicitly asks for upstream work. In particular, never solve this repository's
problems by patching sibling `zephyr/`, `modules/`, or other shared checkout
code when the intended change belongs in this repository.

## Skills and documentation ownership

Skills under `skills/` are Agent operating runbooks for driving Linkr against
a real target device. They may contain:

- target-device operation and debugging through MCP, CLI, TUI, Web UI, or curl
- confirmation rules and dangerous-value gates the firmware enforces
- safety-recovery workflows (BOOTSEL, watchdog, CDC ACM fallback, OTA recovery)
- concise troubleshooting for the most common hardware-facing failures

Skills must not contain Linkr Debugger self-development content:

- Linkr source builds, linker config, or implementation internals
- test-HIL procedures or board-level test specifications
- release engineering, changelogs, or release-artifact handling
- historical measurements, dated HIL reports, or self-debug narratives

Canonical destinations for these topics:

- development, build, flash, and contribution: `docs/developer/`
- protocol and wire contracts, including the v1 persistent-configuration
  snapshot: `docs/reference/`
- HIL procedures, the functional-test spec, and dated board-level evidence:
  `docs/testing/`

When a skill needs any of those topics, it links to the canonical doc rather
than copying the content. Update the canonical doc first; the skill links,
not the other way around.

## Flashing procedures

**⚠️ 重要：线刷（ROM BOOTSEL）必须使用合成完整 UF2 文件 `radxa-linkr-debugger-rp2350.uf2`，不能使用应用 UF2 文件 `zephyr.uf2`。使用应用 UF2 会导致板子变砖。**

### UF2 文件说明

| 文件 | 用途 | 说明 |
|------|------|------|
| `radxa-linkr-debugger-rp2350.uf2` | 线刷（ROM BOOTSEL） | 合成完整 UF2，包含 MCUboot + 应用固件 |
| `build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.uf2` | 仅应用固件 | 不能单独刷写，会导致板子变砖 |
| `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350-ota.bin` | OTA 更新 | MCUboot 格式应用固件 |

### 线刷流程

1. 将板子进入 BOOTSEL 模式（按住 BOOT 按钮后上电，或通过 HTTP/Shell 命令）
2. 等待板子挂载为 `RPI-RP2` USB 驱动器
3. 使用合成完整 UF2 文件刷写：

```sh
# 方法 1：使用 picotool
picotool load -v -x radxa-linkr-debugger-rp2350.uf2

# 方法 2：使用 udisksctl（Linux）
RPI_RP2=$(udisksctl mount -b /dev/sdX1 | awk -F" at " '{print $2}' | tr -d '[:space:]')
cp radxa-linkr-debugger-rp2350.uf2 "$RPI_RP2/"
```

### OTA 流程

1. 确保板子已安装 MCUboot 引导加载程序
2. 使用 OTA 命令更新：

```sh
radxa-linkr-debuggerctl ota upload build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350-ota.bin
radxa-linkr-debuggerctl ota test
# 验证测试启动成功后：
radxa-linkr-debuggerctl ota confirm
```

### 常见错误

**错误：使用 `zephyr.uf2` 进行线刷**
- 症状：板子变砖，无法启动
- 原因：`zephyr.uf2` 不包含 MCUboot 引导加载程序
- 解决：使用合成完整 UF2 文件 `radxa-linkr-debugger-rp2350.uf2` 重新刷写

**错误：OTA 上传 `.uf2` 文件**
- 症状：OTA 上传失败
- 原因：OTA 期望 MCUboot 格式应用固件，不是 UF2 格式
- 解决：使用 `radxa-linkr-debugger-rp2350-ota.bin` 文件

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

Detailed checklist and procedures are maintained in `docs/testing/hil-functional-test-spec.md`.

## Test organization and registration

Every automated test must be reachable from a canonical repository test entry.
Do not add ad-hoc test commands to a workflow while leaving the corresponding
test out of the local entry point.

- Web tests use `cd web && npm test`. The runner discovers `*.test.js`,
  `*.test.mjs`, `*.test.cjs`, `*.test.ts`, and `*.test.tsx` below `web/src/`
  and `web/scripts/`; do not maintain a manual filename list in `package.json`
  or GitHub Actions.
- A Web test must directly import either `node:test` or `vitest`, but not both.
  Node-only tests must not be routed through jsdom, and browser/component tests
  must not be routed through the Node test runner.
- Firmware host-model C tests and offline Python unit tests use
  `apps/radxa_linkr_debugger/tests/run_unit_tests.sh`. Every new
  `model_host/test_*.c` file and every offline `unittest`-based `test_*.py` file
  must be registered there.
- Live HIL tests that require a connected debugger are not part of the offline
  unit runner. They must state their hardware prerequisites and follow
  `docs/testing/hil-functional-test-spec.md`.
- Run `node scripts/check-test-registration.mjs --root .` after adding,
  moving, or renaming tests. The registration check must remain in the complete
  validation workflow.
- A bug fix should include a regression test that fails for the original bug
  when practical. Do not delete, skip, weaken, or silently reclassify a test
  merely to make a gate pass.
- Unit and contract tests must be deterministic and must not depend on public
  network services or physical hardware unless explicitly classified as HIL.

## CI validation

Do not declare CI-ready after validating only the firmware lane. For repository
changes, reproduce every affected GitHub Actions lane locally when practical:

- Test registration and workflow contracts:
  `node --test scripts/check-nightly-workflow.test.mjs scripts/check-repository-gates.test.mjs scripts/check-test-registration.test.mjs`
  followed by `node scripts/check-nightly-workflow.mjs --root .`,
  `node scripts/check-repository-gates.mjs --root .`, and
  `node scripts/check-test-registration.mjs --root .`
- Web tests and production build: `cd web && npm test && npm run build`
- Firmware/offline model tests:
  `apps/radxa_linkr_debugger/tests/run_unit_tests.sh`
- Before completing a commit task that changes `cmd-ng` Cargo dependency inputs
  (`Cargo.toml` or `Cargo.lock`) or Nix packaging, refresh `cargoHash` in
  `nix/package.nix` when needed using the hash reported by a native Nix build,
  then run `nix flake check -L`.
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

### Required GitHub gate

- `.github/workflows/build.yml` is the single reusable source of complete
  repository validation. Pages, nightly, and release workflows must call it
  and must not publish or deploy when it fails.
- `Required repository gate` is the stable aggregate status check. It must
  depend on every required validation job and accept only `success`; skipped,
  cancelled, or failed jobs must fail the aggregate gate.
- GitHub branch protection or repository rulesets for both `main` and `dev`
  must require pull requests, reject direct pushes, and require
  `Required repository gate`. They must also block force pushes and branch
  deletion. A workflow file alone does not enforce any of these restrictions.
- Do not rename the aggregate check without updating the remote ruleset in the
  same change. Never temporarily remove the required check to merge a failing
  change.
- Release artifacts and GitHub Pages deployments must be built from the exact
  revision that passed the complete reusable validation workflow.

The 2026-05-25 CI run `26400376587` is the reference failure: the firmware job
was green, but CI still failed because `cmd-ng/src/app.rs` had rustfmt drift and
`skills/radxa-linkr-debugger/scripts/install.ps1` contained invalid PowerShell
function names with spaces, such as `Get-AgentLinkr DebuggerArch`.
