# 快速开始

[English](getting-started.md)

本文介绍如何搭建构建环境并生成第一份 Radxa Linkr Debugger 固件镜像。

## 前置依赖

固件构建包含 Web UI 和板载协议解码器。CMake 会自动运行 Web/WASM 构建
并将 gzip 压缩的输出嵌入固件。

| 工具 | 版本 | 用途 |
|---|---|---|
| cmake, ninja, dtc, gperf | — | Zephyr 构建系统 |
| python3 + west | ≥1.5 | Zephyr 元工具 |
| python3 intelhex, click, cbor2 | — | MCUboot 镜像工具 |
| nodejs 22 + npm | 22.x | Web UI 构建 |
| rustc + cargo | stable | Rust CLI + WASM 解码器 |
| wasm-bindgen-cli | 0.2.121 | WASM 解码器胶水层 |
| Zephyr SDK | 1.0.1 | ARM 交叉编译器 |

## Nix 配置（推荐）

仓库提供的 `shell.nix` 包含常用构建包。Zephyr SDK 和由 rustup 管理的
stable Rust toolchain 是外部前置条件。先准备 Rust，再设置
`ZEPHYR_SDK_INSTALL_DIR` 后进入：

```sh
rustup toolchain install stable
rustup target add wasm32-unknown-unknown
export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-1.0.1
nix-shell
```

在 nix-shell 内初始化 west 工作区（仅需一次）：

```sh
scripts/setup-zephyr.sh
```

然后构建：

```sh
west build -p always -b rpi_pico2/rp2350a/m33/mcuboot --sysbuild apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
```

## 手动配置（不使用 Nix）

创建 Python 环境并拉取 Zephyr：

```sh
python3 -m venv .venv
source .venv/bin/activate
pip install -U pip west

west init -l .
west update
west zephyr-export
pip install -r zephyr/scripts/requirements.txt
pip install -r bootloader/mcuboot/scripts/requirements.txt
```

如果还没有安装 Zephyr SDK，需要先安装。当前本地构建已用 Zephyr SDK
`1.0.1` 验证过。

构建：

```sh
source .venv/bin/activate
west build -p always -b rpi_pico2/rp2350a/m33/mcuboot --sysbuild apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
```

## 构建产物

RP2350 sysbuild 的应用产物位于：

```text
build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin
build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.hex
```

`zephyr.signed.bin` 是 Zephyr/MCUboot 的格式化文件名；在本项目配置下，它是
用于 OTA 的无签名 MCUboot 格式应用二进制，并不是加密签名镜像。

## 下一步

- [构建指南](build.zh-CN.md) — 详细构建流程、产物说明和 GitHub Actions release assets
- [刷写](flashing.zh-CN.md) — ROM BOOTSEL 和 OTA 更新流程
