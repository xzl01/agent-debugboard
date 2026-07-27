# Install Host CLI

[中文](install.zh-CN.md)

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

## First commands

After placing the executable on `PATH`, verify the CLI is working:

```sh
radxa-linkr-debuggerctl --help
radxa-linkr-debuggerctl --version
radxa-linkr-debuggerctl doctor
```

Running the CLI without a subcommand starts the interactive TUI. Use subcommands such as `status`, `adc read`, or `power set` when you want the traditional command-line mode.
