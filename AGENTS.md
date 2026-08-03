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

Detailed checklist and procedures are maintained in `doc/testing/hil-functional-test-spec.md`.

## CI validation

Do not declare CI-ready after validating only the firmware lane. For repository
changes, reproduce every affected GitHub Actions lane locally when practical:

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

The 2026-05-25 CI run `26400376587` is the reference failure: the firmware job
was green, but CI still failed because `cmd-ng/src/app.rs` had rustfmt drift and
`skills/radxa-linkr-debugger/scripts/install.ps1` contained invalid PowerShell
function names with spaces, such as `Get-AgentLinkr DebuggerArch`.

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
