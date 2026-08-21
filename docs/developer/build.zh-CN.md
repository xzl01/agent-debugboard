# 构建指南

[English](build.md)

Radxa Linkr Debugger 固件的详细构建流程、产物说明和 CI/release asset 参考。

## Nix 工作流

仓库提供的 `shell.nix` 包含 cmake、ninja、dtc、gperf、含 Zephyr 包的
Python、Node.js 22、wasm-bindgen-cli 和 picotool。Zephyr SDK 以及由
rustup 管理的 stable Rust toolchain 仍是外部前置条件；构建前需安装
`wasm32-unknown-unknown` target。

1. 准备 Rust、设置 Zephyr SDK 路径并进入 nix-shell：

   ```sh
   rustup toolchain install stable
   rustup target add wasm32-unknown-unknown
   export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-1.0.1
   nix-shell
   ```

2. 初始化 west 工作区（仅需一次）：

   ```sh
   scripts/setup-zephyr.sh
   ```

   脚本会创建仓库内隔离的 `.zephyr-workspace/`，只下载 RP2350 编译
   所需模块，并执行 `west zephyr-export`。

3. 构建：

   ```sh
   scripts/build-firmware.sh
   ```

## 手动工作流（不使用 Nix）

1. 创建 Python 环境并安装 west：

   ```sh
   python3 -m venv .venv
   source .venv/bin/activate
   pip install -U pip west
   ```

2. 初始化 west 工作区：

   ```sh
   scripts/setup-zephyr.sh
   pip install -r .zephyr-workspace/zephyr/scripts/requirements.txt
   pip install -r .zephyr-workspace/bootloader/mcuboot/scripts/requirements.txt
   ```

3. 如果还没有安装 Zephyr SDK，需要先安装。当前本地构建已用 Zephyr SDK
   `1.0.1` 验证过。

4. 需要单独安装 Node.js 22、npm、Rust toolchain、`wasm32-unknown-unknown`
   target 和 `wasm-bindgen-cli 0.2.121` — 这些不包含在 Zephyr requirements
   文件中。

5. 构建：

   ```sh
   source .venv/bin/activate
   scripts/build-firmware.sh
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
| `radxa-linkr-desktop_windows_amd64.zip` | Windows x64 统一桌面包：Web UI、Host 网关、Serial Broker、常驻 MCP、CLI、托盘进程与安装脚本 |
| `radxa-linkr-desktop_linux_amd64.tar.gz` | Linux x64 / AMD64 统一桌面包；运行时需要 GTK 3 与 Ayatana AppIndicator 3 |
| `radxa-linkr-desktop_darwin_arm64.tar.gz` | macOS Apple Silicon 统一桌面包 |
| `skills-radxa-linkr-debugger.tar.gz` | Agent skill 打包 |
| `SHA256SUMS.txt` | 所有 release assets 的 SHA256 校验文件 |

正式 Release workflow 固定发布 13 个资产。滚动 nightly 只发布精简的 9 项
canary 资产，不包含桌面包和 Linux ARM64 CLI 压缩包。
