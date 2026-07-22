# 贡献指南

[English](contributing.md)

## 运行单元测试

```sh
./apps/radxa_linkr_debugger/tests/run_unit_tests.sh
```

测试脚本覆盖共享板级模型的 host C 单元测试。Rust 主机侧 CLI 检查位于
`cmd-ng/` 目录下，使用 Cargo 运行。

## Rust 主机侧 CLI 检查

```sh
cargo fmt --manifest-path cmd-ng/Cargo.toml --all --check
cargo clippy --manifest-path cmd-ng/Cargo.toml --all-targets -- -D warnings
cargo test --manifest-path cmd-ng/Cargo.toml --all-targets
```

## HIL 功能测试要求

固件和主机控制逻辑的修改不能仅凭编译、静态分析或单元测试就视为完成。
当修改影响真实硬件行为时，必须在真实硬件上完成 HIL 功能测试。

以下类型的修改需要 HIL：

- RP2350 固件逻辑
- 与真实硬件交互的主机侧 CLI/TUI 控制逻辑
- 电源输出、switch 路由、ADC 监测、安全 GPIO、watchdog 或 BOOTSEL 行为

结束此类修改前，必须在真实硬件上验证：

- 完整的规范固件构建
- 刷写和正常板端启动
- HTTP/WS 控制路径
- USB CDC ACM 串口 fallback 路径
- BOOTSEL 入口路径
- 只要实际可行，新功能对应的功能测试

详细检查清单和流程维护在
[doc/testing/hil-functional-test-spec.md](../../doc/testing/hil-functional-test-spec.md)。

## CI 验证

不要只验证固件 lane 就声称 CI 就绪。对于仓库修改，应在本地复现所有受影响的
GitHub Actions lane：

- Rust 主机侧 CLI 格式化：`cargo fmt --manifest-path cmd-ng/Cargo.toml --all --check`
- Rust 主机侧 CLI 检查（修改时）：`cargo clippy --manifest-path cmd-ng/Cargo.toml --all-targets -- -D warnings` 和 `cargo test --manifest-path cmd-ng/Cargo.toml --all-targets`
- PowerShell 安装脚本解析/dry-run：
  `pwsh -NoLogo -NoProfile -NonInteractive -Command '[scriptblock]::Create((Get-Content ./skills/radxa-linkr-debugger/scripts/install.ps1 -Raw)) | Out-Null'`
  以及（可用时）`./skills/radxa-linkr-debugger/scripts/install.ps1 -DryRun`
- Shell 安装/测试脚本：`sh -n ...` 和 `shellcheck ...`
- 固件修改：仅使用完整的规范构建（一次一个，输出到共享的
  `build/radxa_linkr_debugger/` 目录）：
  `west build -p always -b rpi_pico2/rp2350a/m33/mcuboot --sysbuild apps/radxa_linkr_debugger -d build/radxa_linkr_debugger`

不要运行单目标或单驱动的固件编译检查；它们会干扰用户的构建/缓存流程。
请使用完整的固件/包工作流。

## 代码规范

- **Device Tree 描述硬件**：只要 Zephyr 绑定和板级模型能自然表达，
  硬件描述就优先使用 Device Tree，而不是固件里的硬编码表。不要在主机
  CLI 中定义板级硬件描述、引脚映射、电源轨映射、ADC 通道映射或原理图
  派生的硬件信息。

- **标准、统一、优雅的实现**：软件实现应保持标准、统一、优雅，避免引入
  让维护、自动化或文档理解变得困难的临时性写法。

- **MCU 侧输出贴近原值，host 侧做解释**：MCU 侧输出应尽量贴近接口原值；
  只要不破坏原始固件契约，解释、校准和展示优先放在 host 侧完成。

- **同步更新文档和 skill**：任何代码改动都必须在同一个改动中同步更新
  对应的 skill 和文档。

- **保持 BOOTSEL fallback**：修改固件时，必须检查并保持 USB CDC ACM
  串口的 BOOTSEL fallback 路径可用。
