# Getting Started

[中文](getting-started.zh-CN.md)

This guide walks through setting up the build environment and producing your
first firmware image for Radxa Linkr Debugger.

## Prerequisites

The firmware build includes the production Web UI and board-hosted protocol
decoder. CMake runs the locked Web/WASM build and embeds its gzip-compressed
output automatically.

| Tool | Version | Purpose |
|---|---|---|
| cmake, ninja, dtc, gperf | — | Zephyr build system |
| python3 + west | ≥1.5 | Zephyr meta-tool |
| python3 intelhex, click, cbor2 | — | MCUboot image tools |
| nodejs 22 + npm | 22.x | Web UI build |
| rustc + cargo | stable | Rust CLI + WASM decoder |
| wasm-bindgen-cli | 0.2.121 | WASM decoder glue |
| Zephyr SDK | 1.0.1 | ARM cross-compiler |

## Nix Setup (Recommended)

The repo provides a `shell.nix` with the common build packages. The Zephyr SDK
and a rustup-managed stable Rust toolchain are external prerequisites. Prepare
Rust, then set `ZEPHYR_SDK_INSTALL_DIR` before entering:

```sh
rustup toolchain install stable
rustup target add wasm32-unknown-unknown
export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-1.0.1
nix-shell
```

Inside the shell, initialize the west workspace once:

```sh
scripts/setup-zephyr.sh
```

Then build:

```sh
scripts/build-firmware.sh
```

## Manual Setup (Without Nix)

Create a Python environment and fetch Zephyr:

```sh
python3 -m venv .venv
source .venv/bin/activate
pip install -U pip west

scripts/setup-zephyr.sh
pip install -r .zephyr-workspace/zephyr/scripts/requirements.txt
pip install -r .zephyr-workspace/bootloader/mcuboot/scripts/requirements.txt
```

Install the Zephyr SDK if it is not already installed. The current local build
has been verified with Zephyr SDK `1.0.1`.

Build:

```sh
source .venv/bin/activate
scripts/build-firmware.sh
```

## Build Output

For RP2350 sysbuild, the application artifacts are under:

```text
build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin
build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.hex
```

The `zephyr.signed.bin` filename is Zephyr/MCUboot's format name; with this
project configuration it is an unsigned MCUboot-format application binary for
OTA, not a cryptographically signed image.

## Next Steps

- [Build Guide](build.md) — detailed build workflows, artifact descriptions,
  and GitHub Actions release assets
- [Flashing](flashing.md) — ROM BOOTSEL and OTA update procedures
