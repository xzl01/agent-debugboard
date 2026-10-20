# 安装主机侧工具

[English](install.md)

## 推荐：统一桌面安装

桌面 release 包一次安装 host Web/API gateway、常驻 HTTP MCP、CLI/TUI、托盘
监督器，以及仍随包提供的 legacy Serial Broker：

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
自启动。启动后托盘图标用绿色、黄色、红色显示 Web/Host/MCP 的运行、启动中、
不可用状态。菜单可打开 host Web 控制台、查看 Host JSON 状态和 legacy UART
归档、重启托管服务或退出。归档默认按 64 MiB 分段，未固定日志总配额 2 GiB，
保留 30 天。

host gateway 与 MCP 仍是实际打包功能；其中 `uart0`/`uart1` CH347 broker、串口归档
和 MCP serial tools 是等待单独移除的 legacy 非 J4 路径。生产 J4 终端不得使用这些
入口。J4 由 MCU 固件通过板载 xterm.js 页面、CDC ACM 和 raw TCP 5555 提供，不需要
浏览器 chooser 或 Host broker。

常驻入口：

- Web：<http://127.0.0.1:8787/>
- MCP（Streamable HTTP）：<http://127.0.0.1:8787/mcp>
- 状态：<http://127.0.0.1:8787/host/api/v1/status>
- legacy 非 J4 UART 归档状态：
  <http://127.0.0.1:8787/host/api/v1/serial-logging/status>

Linux 桌面包的图形图标需要系统提供 GTK 3 和 Ayatana AppIndicator 3 运行库。
无桌面托盘的服务器环境中，CLI/TUI 与 `-d` / `--desktop` 会把同一个
`linkr-tray` 守护改以 `--headless` 启动；Host 保持在该单一进程中，且不会构造
AppIndicator UI。若 GTK 可加载但缺少 AppIndicator 实现，也采用同一退化路径。
Linux 的 Tray 可执行文件本身仍链接 GTK；完全未安装 GTK 的最小系统应使用
`linkr-host serve`，standalone CLI/TUI 则在没有桌面守护时继续工作。后台
图形环境之后恢复时，再运行 CLI 或 `-d` 会让 headless 守护优雅退出并由单个图形
Tray 守护接管。后台守护会在 CLI 退出后继续运行。需要显式独立 Host 时，仍可直接运行安装目录中的
`linkr-host serve`。

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

每份归档都以两个名称提供同一个程序：`radxa-linkr-debuggerctl` 和短命令 `rdb`。
Unix 归档中的 `rdb` 是相对符号链接，Windows 归档中的 `rdb.exe` 是对应硬链接；
两者的参数、输出和退出状态完全相同。

## Skill 本地安装脚本

在仓库 checkout 内把指定 release 版本下载到仓库的 skill 目录：

```sh
./skills/radxa-linkr-debugger/scripts/install.sh --version <tag>
```

脚本不会修改 `PATH`。请用完整路径运行下载后的 CLI：

```sh
./skills/radxa-linkr-debugger/scripts/bin/radxa-linkr-debuggerctl --help
./skills/radxa-linkr-debugger/scripts/bin/rdb --help
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
.\skills\radxa-linkr-debugger\scripts\bin\rdb.exe --help
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
sudo ln -sfn radxa-linkr-debuggerctl /usr/local/bin/rdb
```

Windows 上请把 `radxa-linkr-debuggerctl.exe` 和 `rdb.exe` 一起复制到
`$env:PATH` 已包含的目录，或把其所在目录加入用户 `PATH`。

## 从源码构建

如果你在开发 `cmd-ng` 本身：

```sh
cargo build --manifest-path cmd-ng/Cargo.toml
./cmd-ng/target/debug/radxa-linkr-debuggerctl --help
```

## 手动运行 host Web gateway 与 MCP

开发者也可以从 checkout 手动构建：

```sh
npm --prefix web ci
npm --prefix web run build
cargo build --release --manifest-path host-tools/Cargo.toml
```

启动组合的 Web Host、板端 gateway、MCP endpoint 和仍随包提供的 legacy
Serial Broker：

```sh
host-tools/target/release/linkr-host serve
```

然后打开 <http://127.0.0.1:8787/>。如需检查 Web 资产、本地 Host 和板端 API，运行：

```sh
host-tools/target/release/linkr-host doctor
```

`doctor` 仍可能报告 CH347 串口发现，因为 legacy 非 J4 子系统仍在编译；该结果
不构成 J4 验证。J4 应通过板载页面、MCU WebSocket `serial.j4`、CDC ACM 或 raw
TCP 5555 验证。

`serve` 同时提供常驻 MCP 地址 <http://127.0.0.1:8787/mcp>。只支持 stdio
的 Agent 客户端可以继续用 `mcp` 参数启动兼容适配器：

```sh
host-tools/target/release/linkr-host mcp
```

Codex/OpenCode 的 HTTP 和 stdio 配置参见 [MCP 配置指南](../reference/mcp-server.md)。
当前打包和首次启动路径见
[安装流程图](../assets/architecture/current-installation-flow.png)。

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
rdb doctor
```

不带子命令直接运行 CLI 会启动交互式 TUI。需要传统命令行模式时使用 `status`、`adc read`、`power set` 等子命令。
