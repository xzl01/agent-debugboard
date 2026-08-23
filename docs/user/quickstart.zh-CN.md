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

每份归档都包含 `radxa-linkr-debuggerctl` 及其短命令别名 `rdb`。解压两个名称后
放到 PATH 下：

```sh
# Linux / macOS
sudo install -m 0755 ./radxa-linkr-debuggerctl /usr/local/bin/
sudo ln -sfn radxa-linkr-debuggerctl /usr/local/bin/rdb

# macOS — 如果 Gatekeeper 弹警告
xattr -dr com.apple.quarantine ./radxa-linkr-debuggerctl
```

验证：

```sh
radxa-linkr-debuggerctl --version
rdb --version
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

交互式终端界面默认打开 Controls 页。无边框表格中每个电源、switch 或 GPIO 对象
各占一行，当前项整行高亮；只有收到数据时才在表格上方显示实时电流行，最后一行
是 htop 式键位/操作块。用方向键选中 `5v_out`；Tab/Shift+Tab 用于在 Controls、
Saved Config、Status 三页间切换。第一次按 Space 只会打开红色边框确认框，不会改变电源，
三秒内再次按 Space 才会打开该电源轨。

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
