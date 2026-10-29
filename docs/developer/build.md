# Build Guide

[中文](build.zh-CN.md)

Detailed build workflows, artifact descriptions, and CI/release asset
reference for the Radxa Linkr Debugger firmware.

## Nix Workflow

The repo provides a `shell.nix` with the Zephyr SDK, cmake, ninja, dtc, gperf,
pkg-config, udev, Python with Zephyr packages, Node.js 22, Cargo, rustc, clippy,
rustfmt, clang, lld, wasm-bindgen-cli, Zopfli 1.0.3, the pinned CH347-enabled
OpenOCD
(`openocd-latest`), picotool, and udisks2. The Zephyr SDK install path is
exported by the shell hook, so interactive builds pick up `gdb`, `objdump`,
and the rest of the SDK toolchain automatically.

`rustup` remains external because `shell.nix` consumes nixpkgs-managed `cargo`
and `rustc` directly. Install the `wasm32-unknown-unknown` target before
building.

1. Enter the shell from the repository root:

   ```sh
   nix-shell
   ```

2. Update the west workspace to the pinned manifest (once):

   ```sh
   make workspace
   ```

   This runs `west update --narrow -o=--depth=1` inside the existing west
   workspace that contains this repository (`west.yml` at the repo root).

3. Build:

   ```sh
   make firmware
   ```

### One-shot nix-shell commands

Every build step can also run without entering an interactive shell. Run from
the repository root with a single `nix-shell --run "..."`:

```sh
# Firmware (canonical directory, full rebuild)
nix-shell --run "west build -p always -b rpi_pico2/rp2350a/m33/mcuboot --sysbuild apps/radxa_linkr_debugger -d build/radxa_linkr_debugger"

# Firmware host-model unit tests
nix-shell --run "apps/radxa_linkr_debugger/tests/run_unit_tests.sh"

# Rust host CLI (build / test / clippy / fmt check)
nix-shell --run "cargo build --manifest-path cmd-ng/Cargo.toml"
nix-shell --run "cargo test --manifest-path cmd-ng/Cargo.toml"
nix-shell --run "cargo clippy --manifest-path cmd-ng/Cargo.toml --all-targets -- -D warnings"
nix-shell --run "cargo fmt --manifest-path cmd-ng/Cargo.toml --all --check"

# Web UI tests and production build
nix-shell --run "cd web && npm test"
nix-shell --run "cd web && npm run build"
```

The west workspace (`west init` / `west update`) is a one-time prerequisite;
`make workspace` refreshes it to the pinned manifest.

## Local Validation Entry Points

Run `make check` for the local CI subset. It runs each lane once, sequentially
even with `make -j`: repository gate contracts/checkers, persistent-configuration
docs and loopback examples, version synchronization, CLI fmt/clippy/tests,
Host/Broker/MCP/tray fmt/clippy/locked all-target tests, native decoder
fmt/all-feature clippy/locked all-target tests, firmware host-model tests, and
Web tests/production build. The Web build already generates the WASM decoder;
there is no second standalone WASM build in this entry.

Production decoder WASM omits the optional `name` section to reduce embedded
flash use without changing executable sections or the ABI. Browser debug stacks
lose internal symbol names; source and the raw Cargo WASM remain available.

Focused entries are `make gates`, `make persistent-configuration-docs`,
`make version-check`, `make host-fmt host-clippy host-test`, and
`make decoder-fmt decoder-clippy decoder-test`. The existing `cli-*`,
`unit-tests`, `web-test`, and `web` entries remain available. Use the project
Nix shell for the Host native GTK/AppIndicator/udev dependencies.
The persistent-docs entry fetches locked CLI dependencies before the tests;
the tests themselves build with `--locked --offline` and execute only loopback
requests. For direct `node --test` use, prepare those dependencies separately.

`make check` does not run firmware sysbuild, Nix packaging, cross-platform
desktop/archive/install checks, or HIL, and is not the complete reusable CI
workflow. Run `make firmware` separately when required, or `make all` for one
canonical firmware build followed by the local subset. `make all` serializes
these phases even under `-j`; do not start a second firmware build or another
Web generator in parallel against the shared build outputs. HIL requires its
own authorization and the [functional test procedure](../testing/hil-functional-test-spec.md).

## Embedded Web Compression And Resource Gates

The firmware Web bundle is embedded as **standard gzip** produced by
**Zopfli 1.0.3**. CMake locates a real `zopfli` executable and runs
`apps/radxa_linkr_debugger/scripts/zopfli_compress.py` with `--i15`; the script
verifies that the stream is gzip with no optional headers and `mtime=0`,
decompresses it back to the exact source bytes, and checks reproducibility.
`generate_inc_file_for_target` only hex-encodes the already verified gzip
bytes; it must not compress again with a different codec. Do not add Brotli or
another content encoding to this embedded path.

The canonical link keeps hard resource gates: at least **8 KiB Flash** and
**32 KiB RAM** must remain free. The ROM/RAM linker assertions enforce
`_flash_used <= 0xCCFD8` (8192 bytes free of 847832) and
`_image_ram_size <= 0x7A000` (32768 bytes free of 532480). A build that
exceeds either gate fails instead of silently consuming the reserve.
`zopfli` must be available in the build shell and CI; a clean firmware build
requires Node.js 22, npm, the Rust `wasm32-unknown-unknown` target,
`wasm-bindgen-cli 0.2.121`, and Zopfli 1.0.3.

## Nix Package Boundary

The flake exposes two user-facing packages and one development overlay:

| Package | What it is | Source of truth |
|---|---|---|
| `radxa-linkr-debuggerctl` | One Rust binary plus a relative symlink `rdb -> radxa-linkr-debuggerctl` | `nix/package.nix`, exported via `nix/overlay.nix` and `flake.nix` |
| `openocd-latest` | A pinned CH347-enabled OpenOCD build (upstream commit `da3920b0a52dc2d394afb222c688dac7e57acc1b`); executable name is `openocd` | `nix/openocd-latest.nix`, exported via `nix/overlay.nix` and `flake.nix` |
| `overlays.default` | Adds both packages to a Nixpkgs import | `nix/overlay.nix` |

There is no separate `rdb` derivation: the CLI package installs a relative
`rdb` symlink alongside the primary `radxa-linkr-debuggerctl` binary, and
both names run the same executable. Do not split `rdb` into its own package
or alias `openocd-latest` under a different name such as `openocd-ch347`.

The `openocd-latest` package name is stable while the upstream revision is
pinned to the commit that included CH347 support at update time. It does not
float on every evaluation; bumps happen through a Nix update in this
repository.

Use the pinned OpenOCD package from the repository:

```sh
nix shell .#openocd-latest -c openocd --version
nix shell .#openocd-latest -c openocd -c "adapter list" -c shutdown
```

`adapter list` must include `ch347`. External consumers can use the flake
reference:

```sh
nix shell github:xzl01/agent-debugboard#openocd-latest -c openocd --version
```

Or pull either package through a flake input, the canonical consumer
pattern:

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
# then use pkgs.radxa-linkr-debuggerctl and pkgs.openocd-latest
```

See `docs/user/openocd.md` for the OpenOCD workflow and reset caveats.

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
   west update --narrow -o=--depth=1
   pip install -r zephyr/scripts/requirements.txt
   pip install -r bootloader/mcuboot/scripts/requirements.txt
   ```

3. Install the Zephyr SDK if not already present. The current local build has
   been verified with Zephyr SDK `1.0.1`.

4. Install Node.js 22, npm, Rust toolchain, `wasm32-unknown-unknown` target,
   `wasm-bindgen-cli 0.2.121`, and Zopfli 1.0.3 separately — these are not covered by the
   Zephyr requirements files.

5. Build:

   ```sh
   source .venv/bin/activate
   make firmware
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
| `radxa-linkr-debuggerctl-rust_linux_amd64.tar.gz` | Statically linked Rust CLI/TUI for Linux x64 / AMD64 |
| `radxa-linkr-debuggerctl-rust_linux_arm64.tar.gz` | Statically linked Rust CLI/TUI for Linux ARM64 / AArch64 |
| `radxa-linkr-debuggerctl-rust_darwin_arm64.tar.gz` | Rust CLI/TUI for macOS Apple Silicon |
| `radxa-linkr-desktop_windows_amd64.zip` | Unified Windows x64 desktop bundle: Web UI, Host gateway, Serial Broker, resident MCP, CLI, tray supervisor, and installer |
| `radxa-linkr-desktop_linux_amd64.tar.gz` | Unified Linux x64 / AMD64 desktop bundle; requires GTK 3 and Ayatana AppIndicator 3 runtime libraries |
| `radxa-linkr-desktop_darwin_arm64.tar.gz` | Unified macOS Apple Silicon desktop bundle |
| `skills-radxa-linkr-debugger.tar.gz` | Agent skill bundle |
| `SHA256SUMS.txt` | SHA256 checksums for all release assets |

Each CLI archive carries the same executable under both command names. Unix
archives contain the primary `radxa-linkr-debuggerctl` file and a relative
`rdb` symlink; the Windows archive contains matching hard-linked `.exe` names.
The Rust package still has one binary target.

The formal workflow publishes exactly 13 assets. The rolling nightly channel
intentionally publishes a smaller nine-asset canary set and does not include
desktop bundles or the Linux ARM64 CLI archive.
