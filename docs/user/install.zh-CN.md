# 安装主机侧工具

[English](install.md)

## 推荐：统一桌面安装

桌面 release 包一次安装 Web UI、Serial Broker、常驻 HTTP MCP、CLI/TUI 和
托盘监督器：

| 系统 / CPU | 统一桌面包 |
| --- | --- |
| Windows x64 | `radxa-linkr-desktop_windows_amd64.zip` |
| Linux x64 / AMD64 | `radxa-linkr-desktop_linux_amd64.tar.gz` |
| macOS Apple Silicon | `radxa-linkr-desktop_darwin_arm64.tar.gz` |

从 GitHub Release 下载后先用 `SHA256SUMS.txt` 校验，解压并运行包内安装器：

```sh
./install.sh
```

Windows PowerShell：

```powershell
.\install.ps1
```

安装器不使用 sudo、不修改 PATH，只写入当前用户的应用数据目录，并注册登录
自启动。启动后托盘图标用绿色、黄色、红色显示 Web/Broker/MCP 的运行、启动中、
不可用状态。菜单可打开 Web 控制台、查看 Host JSON 状态和 UART 归档、重启托管
服务或退出。通过 Bridge 打开的 UART 默认会把原始 RX 归档到本机；直接 Web Serial
仍只保存在浏览器内。默认按 64 MiB 分段，未固定日志总配额 2 GiB，保留 30 天。

常驻入口：

- Web：<http://127.0.0.1:8787/>
- MCP（Streamable HTTP）：<http://127.0.0.1:8787/mcp>
- 状态：<http://127.0.0.1:8787/host/api/v1/status>
- UART 归档状态：<http://127.0.0.1:8787/host/api/v1/serial-logging/status>

Linux 桌面包需要系统提供 GTK 3 和 Ayatana AppIndicator 3 运行库。无桌面托盘的
服务器环境可直接运行安装目录中的 `linkr-host serve`。

从源码 checkout 安装同一套内容：

```sh
./scripts/install-host.sh
```

PowerShell 使用 `./scripts/install-host.ps1`。可加 `--no-start`、
`--no-autostart` 或 `--prefix DIR`；PowerShell 对应 `-NoStart`、
`-NoAutostart` 和 `-Prefix`。

## 从 GitHub Releases 下载

从[项目 GitHub Releases](https://github.com/xzl01/agent-debugboard/releases)
下载匹配你平台的归档文件：

| 系统 / CPU | 产物 |
| --- | --- |
| Windows x64 | `radxa-linkr-debuggerctl-rust_windows_amd64.zip` |
| Linux x64 / AMD64 | `radxa-linkr-debuggerctl-rust_linux_amd64.tar.gz` |
| Linux ARM64 / AArch64 | `radxa-linkr-debuggerctl-rust_linux_arm64.tar.gz` |
| macOS Apple Silicon | `radxa-linkr-debuggerctl-rust_darwin_arm64.tar.gz` |

Linux 归档使用 musl 静态链接，不依赖主机 glibc 版本；在 CPU 架构匹配时，
可用于 Debian 11 及更新的发行版。

## Skill 本地安装脚本

在仓库 checkout 内把指定 release 版本下载到仓库的 skill 目录：

```sh
./skills/radxa-linkr-debugger/scripts/install.sh --version <tag>
```

脚本不会修改 `PATH`。请用完整路径运行下载后的 CLI：

```sh
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --help
```

安装脚本默认从 `xzl01/agent-debugboard` 下载。仅当使用 fork 或 release
镜像时才需要通过 `--repo OWNER/REPO` 覆盖。

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

PowerShell 脚本同样只安装到 skill 目录。下载后用以下命令运行：

```powershell
.\skills\radxa-linkr-debugger\scripts\bin\radxa-linkr-debuggerctl.exe --help
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

## 安装到 `PATH`

如需不带路径前缀直接运行 `radxa-linkr-debuggerctl`，请解压 release 归档，
用 `SHA256SUMS.txt` 校验后，把可执行文件放入已有的 `PATH` 目录。例如在
Linux 或 macOS 上：

```sh
sudo install -m 0755 ./radxa-linkr-debuggerctl /usr/local/bin/radxa-linkr-debuggerctl
```

Windows 上请把 `radxa-linkr-debuggerctl.exe` 复制到 `$env:PATH` 已包含的目录，
或把其所在目录加入用户 `PATH`。

## 从源码构建

如果你在开发 `cmd-ng` 本身：

```sh
cargo build --manifest-path cmd-ng/Cargo.toml
./cmd-ng/target/debug/radxa-linkr-debuggerctl --help
```

## 手动运行 Web UI、共享 Serial Broker 与 MCP

开发者也可以从 checkout 手动构建：

```sh
npm --prefix web ci
npm --prefix web run build
cargo build --release --manifest-path host-tools/Cargo.toml
```

启动组合的 Web Host、板端网关和共享 Serial Broker：

```sh
host-tools/target/release/linkr-host serve
```

然后打开 <http://127.0.0.1:8787/>。如需同时检查 Web 资产、本地 Host、
板端 API 和 CH347F 串口发现，运行：

```sh
host-tools/target/release/linkr-host doctor
```

`serve` 同时提供常驻 MCP 地址 <http://127.0.0.1:8787/mcp>。只支持 stdio
的 Agent 客户端可以继续用 `mcp` 参数启动兼容适配器：

```sh
host-tools/target/release/linkr-host mcp
```

Codex/OpenCode 的 HTTP 和 stdio 配置参见 [MCP 配置指南](../../doc/mcp-server.md)。
当前打包和首次启动路径见
[安装流程图](../../doc/current-installation-flow.png)。

## 安装或更新调试器固件

全新 ROM BOOTSEL 线刷只能使用包含 MCUboot 和应用的合成完整文件
`radxa-linkr-debugger-rp2350.uf2`。不得用仅应用的 `zephyr.uf2`
执行 ROM BOOTSEL 线刷。已安装 MCUboot 的设备可使用
`radxa-linkr-debugger-rp2350-ota.bin` 更新，然后执行 OTA test 和 confirm。

完整恢复和 OTA 边界参见
[固件刷写流程](../../AGENTS.md#flashing-procedures)。

## 首次使用

把可执行文件放入 `PATH` 后，验证 CLI 是否正常工作：

```sh
radxa-linkr-debuggerctl --help
radxa-linkr-debuggerctl --version
radxa-linkr-debuggerctl doctor
```

不带子命令直接运行 CLI 会启动交互式 TUI。需要传统命令行模式时使用 `status`、`adc read`、`power set` 等子命令。
