# 安装主机侧 CLI

[English](install.md)

## 从 GitHub Releases 下载

从 GitHub Releases 下载匹配你平台的归档文件：

| 系统 / CPU | 产物 |
| --- | --- |
| Windows x64 | `radxa-linkr-debuggerctl-rust_windows_amd64.zip` |
| Linux x64 | `radxa-linkr-debuggerctl-rust_linux_amd64.tar.gz` |
| macOS Apple Silicon | `radxa-linkr-debuggerctl-rust_darwin_arm64.tar.gz` |

## 安装脚本

在仓库 checkout 内安装指定 release 版本：

```sh
./skills/radxa-linkr-debugger/scripts/install.sh --version <tag>
```

### 私有仓库

私有仓库 release 下载需要先提供 GitHub token：

```sh
export GH_TOKEN="$(gh auth token)"
./skills/radxa-linkr-debugger/scripts/install.sh --version <tag>
```

如果已登录 GitHub CLI，可直接使用 `gh auth token`。

### Windows PowerShell

```powershell
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\skills\radxa-linkr-debugger\scripts\install.ps1
```

私有仓库 PowerShell release 下载：

```powershell
$env:GH_TOKEN = gh auth token
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\skills\radxa-linkr-debugger\scripts\install.ps1 -Version <tag>
```

## macOS Gatekeeper

macOS 上未签名的 release 二进制可能触发 Gatekeeper 提示。安装脚本会先校验 `SHA256SUMS.txt`，再自动移除 quarantine 标记。如果你手动解压 release 归档，请先校验 SHA256，再执行：

```sh
xattr -dr com.apple.quarantine ./radxa-linkr-debuggerctl
```

## 从源码构建

如果你在开发 `cmd-ng` 本身：

```sh
cargo build --manifest-path cmd-ng/Cargo.toml
./cmd-ng/target/debug/radxa-linkr-debuggerctl --help
```

## 首次使用

安装后验证 CLI 是否正常工作：

```sh
radxa-linkr-debuggerctl --help
radxa-linkr-debuggerctl --version
radxa-linkr-debuggerctl doctor
```

不带子命令直接运行 CLI 会启动交互式 TUI。需要传统命令行模式时使用 `status`、`adc read`、`power set` 等子命令。
