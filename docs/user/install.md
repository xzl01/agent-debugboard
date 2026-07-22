# Install Host CLI

[中文](install.zh-CN.md)

## Download from GitHub Releases

Download the matching archive for your platform from GitHub Releases:

| OS / CPU | Artifact |
| --- | --- |
| Windows x64 | `radxa-linkr-debuggerctl-rust_windows_amd64.zip` |
| Linux x64 | `radxa-linkr-debuggerctl-rust_linux_amd64.tar.gz` |
| macOS Apple Silicon | `radxa-linkr-debuggerctl-rust_darwin_arm64.tar.gz` |

## Install script

From a repository checkout, install a specific release version:

```sh
./skills/radxa-linkr-debugger/scripts/install.sh --version <tag>
```

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

## Building from source

If you are developing `cmd-ng` itself from source:

```sh
cargo build --manifest-path cmd-ng/Cargo.toml
./cmd-ng/target/debug/radxa-linkr-debuggerctl --help
```

## First commands

After installation, verify the CLI is working:

```sh
radxa-linkr-debuggerctl --help
radxa-linkr-debuggerctl --version
radxa-linkr-debuggerctl doctor
```

Running the CLI without a subcommand starts the interactive TUI. Use subcommands such as `status`, `adc read`, or `power set` when you want the traditional command-line mode.
