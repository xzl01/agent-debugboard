# 构建指南

[English](build.md)

Radxa Linkr Debugger 固件的详细构建流程、产物说明和 CI/release asset 参考。

## Nix 工作流

仓库提供的 `shell.nix` 包含 Zephyr SDK、cmake、ninja、dtc、gperf、pkg-config、
udev、含 Zephyr 包的 Python、Node.js 22、Cargo、rustc、clippy、rustfmt、clang、lld、
wasm-bindgen-cli、固定版本的启用 CH347 的 OpenOCD（`openocd-latest`）、
picotool 和 udisks2。Zephyr SDK 的安装路径由 shell hook 导出，交互式构建
会自动获得 `gdb`、`objdump` 等 SDK 工具。

由于 `shell.nix` 直接使用 nixpkgs 提供的 `cargo` 和 `rustc`，`rustup` 仍是
外部前置；构建前需要安装 `wasm32-unknown-unknown` target。

1. 在仓库根目录进入 shell：

   ```sh
   nix-shell
   ```

2. 更新 west 工作区到清单锁定版本（仅需一次）：

   ```sh
   make workspace
   ```

   该命令在包含本仓库的现有 west 工作区中执行
   `west update --narrow -o=--depth=1`（`west.yml` 位于仓库根）。

3. 构建：

   ```sh
   make firmware
   ```

### 一行式 nix-shell 命令

所有构建步骤也可以不进入交互式 shell，在仓库根目录用单个
`nix-shell --run "..."` 直接执行：

```sh
# 固件（canonical 目录，全量重建）
nix-shell --run "west build -p always -b rpi_pico2/rp2350a/m33/mcuboot --sysbuild apps/radxa_linkr_debugger -d build/radxa_linkr_debugger"

# 固件 host-model 单元测试
nix-shell --run "apps/radxa_linkr_debugger/tests/run_unit_tests.sh"

# Rust 主机 CLI（构建 / 测试 / clippy / fmt 检查）
nix-shell --run "cargo build --manifest-path cmd-ng/Cargo.toml"
nix-shell --run "cargo test --manifest-path cmd-ng/Cargo.toml"
nix-shell --run "cargo clippy --manifest-path cmd-ng/Cargo.toml --all-targets -- -D warnings"
nix-shell --run "cargo fmt --manifest-path cmd-ng/Cargo.toml --all --check"

# Web UI 测试与生产构建
nix-shell --run "cd web && npm test"
nix-shell --run "cd web && npm run build"
```

west 工作区（`west init` / `west update`）只需一次性准备，
`make workspace` 可将其刷新到清单锁定版本。

## Nix 包边界

flake 对外暴露两个用户级包与一个开发 overlay：

| 包 | 含义 | 单一来源 |
|---|---|---|
| `radxa-linkr-debuggerctl` | 一个 Rust 二进制，外加相对符号链接 `rdb -> radxa-linkr-debuggerctl` | `nix/package.nix`，通过 `nix/overlay.nix` 与 `flake.nix` 导出 |
| `openocd-latest` | 固定版本、启用 CH347 的 OpenOCD 构建（上游 commit `da3920b0a52dc2d394afb222c688dac7e57acc1b`），可执行文件名是 `openocd` | `nix/openocd-latest.nix`，通过 `nix/overlay.nix` 与 `flake.nix` 导出 |
| `overlays.default` | 向 Nixpkgs 导入同时追加上述两个包 | `nix/overlay.nix` |

不存在独立的 `rdb` 派生：CLI 包在主二进制 `radxa-linkr-debuggerctl` 旁安装
一个相对符号链接 `rdb`，两个名字运行的是同一个可执行文件。不要把 `rdb`
拆成独立包，也不要把 `openocd-latest` 改名为 `openocd-ch347` 之类的别名。

`openocd-latest` 包名是稳定的，上游版本被锁定在包含 CH347 支持的 commit
（更新时确定）。它不会在每次求值时浮动；版本更新通过本仓库的 Nix 更新
提交完成。

在仓库内使用固定版本的 OpenOCD 包：

```sh
nix shell .#openocd-latest -c openocd --version
nix shell .#openocd-latest -c openocd -c "adapter list" -c shutdown
```

`adapter list` 必须包含 `ch347`。外部消费者可以使用 flake 引用：

```sh
nix shell github:xzl01/agent-debugboard#openocd-latest -c openocd --version
```

也可以按 flake 输入模型（推荐消费者写法）拉取任一包：

```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    agent-debugboard.url = "github:xzl01/agent-debugboard";
  };

  outputs = { self, nixpkgs, agent-debugboard, ... }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        inherit system;
        overlays = [ agent-debugboard.overlays.default ];
      };
    in {
      packages.${system}.default = pkgs.radxa-linkr-debuggerctl;
    };
}
# 之后使用 pkgs.radxa-linkr-debuggerctl 与 pkgs.openocd-latest
```

OpenOCD 工作流与复位注意事项见 `docs/user/openocd.zh-CN.md`。

## 手动工作流（不使用 Nix）

1. 创建 Python 环境并安装 west：

   ```sh
   python3 -m venv .venv
   source .venv/bin/activate
   pip install -U pip west
   ```

2. 初始化 west 工作区：

   ```sh
   west init -l .
   west update --narrow -o=--depth=1
   pip install -r zephyr/scripts/requirements.txt
   pip install -r bootloader/mcuboot/scripts/requirements.txt
   ```

3. 如果还没有安装 Zephyr SDK，需要先安装。当前本地构建已用 Zephyr SDK
   `1.0.1` 验证过。

4. 需要单独安装 Node.js 22、npm、Rust toolchain、`wasm32-unknown-unknown`
   target 和 `wasm-bindgen-cli 0.2.121` — 这些不包含在 Zephyr requirements
   文件中。

5. 构建：

   ```sh
   source .venv/bin/activate
   make firmware
   ```

## 构建产物

RP2350 sysbuild 的应用产物位于：

```text
build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin
build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.hex
```

`zephyr.signed.bin` 是 Zephyr/MCUboot 的格式化文件名。在本项目配置下，它是
**无签名** MCUboot 格式应用二进制，用于 OTA，并非加密签名镜像。release 产物
`radxa-linkr-debugger-rp2350-ota.bin` 即为此文件的副本。

## 固定构建目录策略

始终构建到 `build/radxa_linkr_debugger/`。不要切换到其它 build 目录，也不要使用
临时挂载点里残留的旧 UF2。RP2350 初次安装或恢复刷写 release 发布的合并型
MCUboot 加应用 UF2：`radxa-linkr-debugger-rp2350.uf2`。

## GitHub Actions 产物

`Build` workflow 会检查每次 push 和 pull request。推送 `v*` tag 会触发
`Release` workflow，自动构建固件、打包主机 CLI、创建 GitHub Release，并上传
固定命名的 release assets。

| 产物 | 说明 |
|---|---|
| `radxa-linkr-debugger-rp2350.uf2` | MCUboot + 应用合并固件，用于初次安装、恢复、拖拽刷写或 `picotool` |
| `radxa-linkr-debugger-rp2350-ota.bin` | OTA payload（无签名 MCUboot 格式），来自 sysbuild `zephyr.signed.bin` |
| `radxa-linkr-debugger-rp2350.elf` | 用于调试的 ELF |
| `radxa-linkr-debugger-rp2350.map` | 链接 map |
| `radxa-linkr-debuggerctl-rust_windows_amd64.zip` | Windows x64 Rust CLI/TUI |
| `radxa-linkr-debuggerctl-rust_linux_amd64.tar.gz` | Linux x64 / AMD64 静态链接 Rust CLI/TUI |
| `radxa-linkr-debuggerctl-rust_linux_arm64.tar.gz` | Linux ARM64 / AArch64 静态链接 Rust CLI/TUI |
| `radxa-linkr-debuggerctl-rust_darwin_arm64.tar.gz` | macOS Apple Silicon Rust CLI/TUI |
| `skills-radxa-linkr-debugger.tar.gz` | Agent skill 打包 |
| `SHA256SUMS.txt` | 所有 release assets 的 SHA256 校验文件 |

每份 CLI 归档都以两个命令名提供同一可执行文件。Unix 归档包含主文件
`radxa-linkr-debuggerctl` 和相对符号链接 `rdb`；Windows 归档包含对应的两个
硬链接 `.exe`。Rust package 仍只有一个 binary target。
