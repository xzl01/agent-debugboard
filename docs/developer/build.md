# Build Guide

[中文](build.zh-CN.md)

Detailed build workflows, artifact descriptions, and CI/release asset
reference for the Radxa Linkr Debugger firmware.

## Nix Workflow

The repo provides a `shell.nix` that bundles all dependencies (cmake, ninja,
dtc, gperf, Python with Zephyr packages, Node.js 22, Rust toolchain via
rustup, wasm-bindgen-cli, picotool).

1. Set the Zephyr SDK path and enter the shell:

   ```sh
   export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-1.0.1
   nix-shell
   ```

2. Initialize the west workspace (once):

   ```sh
   scripts/setup-zephyr.sh
   ```

   This runs `west init -l .`, `west update`, and `west zephyr-export`
   automatically. If `.west/` already exists, it skips `west init`.

3. Build:

   ```sh
   west build -p always -b rpi_pico2/rp2350a/m33/mcuboot --sysbuild apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
   ```

## Manual Workflow (Without Nix)

1. Create a Python environment and install west:

   ```sh
   python3 -m venv .venv
   source .venv/bin/activate
   pip install -U pip west
   ```

2. Initialize the west workspace:

   ```sh
   west init -l .
   west update
   west zephyr-export
   pip install -r zephyr/scripts/requirements.txt
   pip install -r bootloader/mcuboot/scripts/requirements.txt
   ```

3. Install the Zephyr SDK if not already present. The current local build has
   been verified with Zephyr SDK `1.0.1`.

4. Install Node.js 22, npm, Rust toolchain, `wasm32-unknown-unknown` target,
   and `wasm-bindgen-cli 0.2.121` separately — these are not covered by the
   Zephyr requirements files.

5. Build:

   ```sh
   source .venv/bin/activate
   west build -p always -b rpi_pico2/rp2350a/m33/mcuboot --sysbuild apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
   ```

## Build Artifacts

For RP2350 sysbuild, the application artifacts are under:

```text
build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin
build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.hex
```

The `zephyr.signed.bin` filename is Zephyr/MCUboot's format name. With this
project configuration it is an **unsigned** MCUboot-format application binary
for OTA, not a cryptographically signed image. The release asset
`radxa-linkr-debugger-rp2350-ota.bin` is a copy of this file.

## Fixed Build Directory Policy

Always build into `build/radxa_linkr_debugger/`. Do not switch to alternate
build directories or use stale UF2 copies from temporary mount points. For
RP2350 initial install or recovery, use the combined MCUboot plus application
UF2 published as `radxa-linkr-debugger-rp2350.uf2`.

## GitHub Actions Artifacts

The `Build` workflow checks every push and pull request. Tagging `v*` triggers
the `Release` workflow, which builds firmware, packages the host CLI, creates a
GitHub Release, and uploads the fixed release assets.

| Artifact | Description |
|---|---|
| `radxa-linkr-debugger-rp2350.uf2` | Combined MCUboot + application for initial install, recovery, drag-and-drop, or `picotool` |
| `radxa-linkr-debugger-rp2350-ota.bin` | OTA payload (unsigned MCUboot format), copied from sysbuild `zephyr.signed.bin` |
| `radxa-linkr-debugger-rp2350.elf` | ELF for debugging |
| `radxa-linkr-debugger-rp2350.map` | Linker map |
| `radxa-linkr-debuggerctl-rust_windows_amd64.zip` | Rust CLI/TUI for Windows x64 |
| `radxa-linkr-debuggerctl-rust_linux_amd64.tar.gz` | Rust CLI/TUI for Linux x64 |
| `radxa-linkr-debuggerctl-rust_darwin_arm64.tar.gz` | Rust CLI/TUI for macOS Apple Silicon |
| `skills-radxa-linkr-debugger.tar.gz` | Agent skill bundle |
| `SHA256SUMS.txt` | SHA256 checksums for all release assets |
