# Install Host Tools

[中文](install.zh-CN.md)

## Recommended: unified desktop install

The desktop release archive installs the Web UI, Serial Broker, resident HTTP
MCP, CLI/TUI, and tray supervisor together:

| OS / CPU | Unified desktop archive |
| --- | --- |
| Windows x64 | `radxa-linkr-desktop_windows_amd64.zip` |
| Linux x64 / AMD64 | `radxa-linkr-desktop_linux_amd64.tar.gz` |
| macOS Apple Silicon | `radxa-linkr-desktop_darwin_arm64.tar.gz` |

Download the archive and `SHA256SUMS.txt` from the same GitHub Release, verify
it, extract it, and run the bundled installer:

```sh
./install.sh
```

On Windows PowerShell:

```powershell
.\install.ps1
```

The installer does not use sudo or modify `PATH`. It writes to the current
user's application-data directory and registers login autostart. The tray icon
uses green, amber, and red states for running, starting, and unavailable
Web/Broker/MCP services. Its menu opens the Web console, Host JSON status, and
Host UART archives, restarts its managed services, or quits. UART sessions
opened through Bridge are archived as raw RX on this computer by default;
direct Web Serial sessions remain browser-local. The default policy uses
64 MiB segments, a 2 GiB unpinned quota, and 30-day retention.

Resident endpoints:

- Web: <http://127.0.0.1:8787/>
- MCP (Streamable HTTP): <http://127.0.0.1:8787/mcp>
- Status: <http://127.0.0.1:8787/host/api/v1/status>
- UART archive status: <http://127.0.0.1:8787/host/api/v1/serial-logging/status>

The Linux desktop archive requires GTK 3 and Ayatana AppIndicator 3 runtime
libraries. On a headless machine without a system tray, run the installed
`linkr-host serve` directly.

To install the same stack from a source checkout:

```sh
./scripts/install-host.sh
```

Use `./scripts/install-host.ps1` on PowerShell. Optional switches are
`--no-start`, `--no-autostart`, and `--prefix DIR`; PowerShell uses `-NoStart`,
`-NoAutostart`, and `-Prefix`.

## Download from GitHub Releases

Download the matching archive for your platform from the
[project's GitHub Releases](https://github.com/xzl01/agent-debugboard/releases):

| OS / CPU | Artifact |
| --- | --- |
| Windows x64 | `radxa-linkr-debuggerctl-rust_windows_amd64.zip` |
| Linux x64 / AMD64 | `radxa-linkr-debuggerctl-rust_linux_amd64.tar.gz` |
| Linux ARM64 / AArch64 | `radxa-linkr-debuggerctl-rust_linux_arm64.tar.gz` |
| macOS Apple Silicon | `radxa-linkr-debuggerctl-rust_darwin_arm64.tar.gz` |

The Linux archives are statically linked with musl and do not depend on the
host glibc version. They are compatible with Debian 11 and newer distributions
on the matching CPU architecture.

## Skill-local install script

From a repository checkout, download a specific release version into the
repository skill directory:

```sh
./skills/radxa-linkr-debugger/scripts/install.sh --version <tag>
```

The script intentionally does not modify `PATH`. Run the downloaded CLI with
its full path:

```sh
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --help
```

The installer downloads from `xzl01/agent-debugboard` by default. Use
`--repo OWNER/REPO` only when installing from a fork or release mirror.

### Private repository

For a private repository release download, export a GitHub token first:

```sh
export GH_TOKEN="$(gh auth token)"
./skills/radxa-linkr-debugger/scripts/install.sh --version <tag>
```

`gh auth token` works if the GitHub CLI is logged in.

### Windows PowerShell

```powershell
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\skills\radxa-linkr-debugger\scripts\install.ps1
```

The PowerShell script is also skill-local. Run the downloaded executable with:

```powershell
.\skills\radxa-linkr-debugger\scripts\bin\radxa-linkr-debuggerctl.exe --help
```

Private repository PowerShell release download:

```powershell
$env:GH_TOKEN = gh auth token
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\skills\radxa-linkr-debugger\scripts\install.ps1 -Version <tag>
```

## macOS Gatekeeper

On macOS, unsigned release binaries may trigger a Gatekeeper warning. The installer verifies `SHA256SUMS.txt` first and then removes the quarantine flag automatically. If you unpack a release archive manually, verify the checksum and remove the quarantine flag:

```sh
xattr -dr com.apple.quarantine ./radxa-linkr-debuggerctl
```

## Install on `PATH`

To use `radxa-linkr-debuggerctl` without a path prefix, extract the release
archive, verify it against `SHA256SUMS.txt`, and place the executable in a
directory already on your `PATH`. For example, on Linux or macOS:

```sh
sudo install -m 0755 ./radxa-linkr-debuggerctl /usr/local/bin/radxa-linkr-debuggerctl
```

On Windows, copy `radxa-linkr-debuggerctl.exe` into a directory listed in
`$env:PATH`, or add its containing directory to the user `PATH`.

## Building from source

If you are developing `cmd-ng` itself from source:

```sh
cargo build --manifest-path cmd-ng/Cargo.toml
./cmd-ng/target/debug/radxa-linkr-debuggerctl --help
```

## Run the Web UI, shared Serial Broker, and MCP manually

Developers can also build the stack manually from a checkout:

```sh
npm --prefix web ci
npm --prefix web run build
cargo build --release --manifest-path host-tools/Cargo.toml
```

Start the combined Web host, board gateway, and shared Serial Broker:

```sh
host-tools/target/release/linkr-host serve
```

Then open <http://127.0.0.1:8787/>. To verify Web assets, the loopback Host,
the board API, and CH347F serial discovery together, run:

```sh
host-tools/target/release/linkr-host doctor
```

`serve` also exposes the resident MCP URL at
<http://127.0.0.1:8787/mcp>. Agent clients that support only stdio can still
start the compatibility adapter with `mcp`; it adopts or supervises the
loopback Host automatically:

```sh
host-tools/target/release/linkr-host mcp
```

See the [MCP setup guide](../../doc/mcp-server.md) for the Codex/OpenCode HTTP and stdio
configuration. The current packaging and first-run paths are summarized in the
[installation flow diagram](../../doc/current-installation-flow.png).

## Install or update debugger firmware

For a fresh ROM BOOTSEL flash, use only the combined
`radxa-linkr-debugger-rp2350.uf2`, which contains MCUboot and the application.
Never use the application-only `zephyr.uf2` for a ROM BOOTSEL flash. An existing
MCUboot installation can be updated with
`radxa-linkr-debugger-rp2350-ota.bin`, followed by OTA test and confirm.

See the [firmware flashing procedures](../../AGENTS.md#flashing-procedures) for
the complete recovery and OTA boundaries.

## First commands

After placing the executable on `PATH`, verify the CLI is working:

```sh
radxa-linkr-debuggerctl --help
radxa-linkr-debuggerctl --version
radxa-linkr-debuggerctl doctor
```

Running the CLI without a subcommand starts the interactive TUI. Use subcommands such as `status`, `adc read`, or `power set` when you want the traditional command-line mode.
