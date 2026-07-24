[English](quickstart.md)

# 快速入门

5 分钟把调试板跑起来。

## 1. 安装 CLI

从 [GitHub Releases](https://github.com/xzl01/agent-debugboard/releases) 下载对应平台的归档：

| 系统 | 归档 |
|------|------|
| Windows x64 | `radxa-linkr-debuggerctl-rust_windows_amd64.zip` |
| Linux x64 / AMD64 | `radxa-linkr-debuggerctl-rust_linux_amd64.tar.gz` |
| Linux ARM64 / AArch64 | `radxa-linkr-debuggerctl-rust_linux_arm64.tar.gz` |
| macOS Apple Silicon | `radxa-linkr-debuggerctl-rust_darwin_arm64.tar.gz` |

解压后放到 PATH 下：

```sh
# Linux / macOS
sudo install -m 0755 ./radxa-linkr-debuggerctl /usr/local/bin/

# macOS — 如果 Gatekeeper 弹警告
xattr -dr com.apple.quarantine ./radxa-linkr-debuggerctl
```

验证：

```sh
radxa-linkr-debuggerctl --version
```

完整安装指南：[安装 CLI](install.zh-CN.md)

## 2. 连接调试板

USB-C 线一头插板子，一头插 PC。板子会枚举为复合 USB 设备——Linux 和 macOS 无需装驱动。

等几秒让网络接口起来，然后检查：

```sh
radxa-linkr-debuggerctl doctor
```

应该看到所有检查通过。如果 `doctor` 报连接问题，参考[安装 CLI](install.zh-CN.md) 或[调试板概览](board-overview.zh-CN.md)。

## 3. 打开 TUI

不带参数运行：

```sh
radxa-linkr-debuggerctl
```

交互式终端界面会显示电源输出、switch 路由和 GPIO 状态。试试看：用方向键选到 `5v_out`，按 Space 切换电源，观察状态从 `off` 变成 `on`。

更多：[TUI 指南](tui.zh-CN.md)

## 4. 打开 Web UI

浏览器打开：

```
http://172.29.203.1/
```

大多数系统会自动弹出板子的 captive portal 提示。如果没有，手动导航即可。

仪表盘有电源控制、ADC 读数、switch 路由和 GPIO。试试看：在 Power Controls 卡片点 `5v_out` 的开关，然后看 ADC 卡片的电流读数变化。切到 **Terminal** 工作区可以找到逻辑分析仪和串口控制台。

Linux 上如果 Web Serial 或 Bridge 无法打开 `/dev/ttyUSB*` 或
`/dev/ttyACM*`，请参阅 [Linux 串口设备权限](webui.zh-CN.md#linux-串口设备权限)。

更多：[Web UI 指南](webui.zh-CN.md)

## 5. 试几条命令

```sh
# 查看板子状态
radxa-linkr-debuggerctl status
# → 显示电源状态、switch 路由、GPIO 值、uptime

# 给目标板上电
radxa-linkr-debuggerctl power set 5v_out on
# → "5v_out: on"

# 读电流
radxa-linkr-debuggerctl adc read
# → "5v_out=0.123000A  12v_out=0.000000A  20v_out=0.000000A"

# SD 卡路由到主机读取
radxa-linkr-debuggerctl switch route sd usb-reader
# → SD 卡现在作为 USB 存储出现在 PC 上

# 驱动 GPIO 高电平
radxa-linkr-debuggerctl gpio set GP13 1
# → "GP13: 1 (output)"
```

## 接下来

- [CLI 参考](cli.zh-CN.md) — 所有子命令
- [HTTP API 参考](api.zh-CN.md) — 脚本和自动化
- [逻辑分析仪](logic-analyzer.zh-CN.md) — 高速 GPIO 捕获
- [电源分析仪](power-analyzer.zh-CN.md) — 触发式电流捕获
