# agent-debugboard

[English](README.md)

`agent-debugboard` 是 **Agent DebugBoard** 的 RP2040 固件。它把一块硬件
调试板变成 PC 侧 Agent/AI 可以直接操作的 USB 控制接口，用于控制目标开发板
或 SBC 的供电、刷机模式、TF/SD 路由、电流监测 ADC 和一组安全 GPIO。

![Agent Debugger 宣传图](doc/marketing/agent-debugger-promo.png)

## 项目简介

Agent DebugBoard 面向自动化 bring-up、远程恢复、产测和 AI agent 调试链路。
固件会枚举为复合 USB 设备：以 USB NCM 网络接口作为主控制面，并保留一个
USB CDC ACM 串口用于 Zephyr 通用 cmdline 和 BOOTSEL fallback；主机侧当前主力
命令路径是 `cmd-ng/` 下的 Rust CLI/TUI，用于脚本和 Agent 直接调用。

本仓库包含 Zephyr 应用、主力 Rust 主机侧 CLI/TUI、保留作弃用参考的旧 Go 主机
侧实现、单元测试、原理图副本和项目文档。

当前主力主机侧命令路径是 [`cmd-ng/`](cmd-ng/)。旧的 Go
`cmd/agent-debugboardctl` + `internal/hostcli` 已进入 deprecated/legacy 维护状
态，仅保留作参考实现与回归对照。

## 功能范围

| 模块 | 当前固件支持 |
| --- | --- |
| USB 控制 | 复合 USB 设备：NCM HTTP/WS 主控制面 + CDC ACM fallback 控制台 |
| 主机自动化 | Rust `cmd-ng` CLI/TUI，支持 JSON 输出和 `doctor` 诊断 |
| 实时遥测 | `/api/v1/ws` 上的双向 WebSocket 长连接 |
| 电源输出 | `12v_out`、`5v_out`、`5v_ws`、`20v_out` |
| ADC 监测 | 读取 `5v_out`、`12v_out`、`20v_out` 的电流监测通道 |
| 调试板自监控 | `/api/v1/status` 和 WebSocket 状态快照会报告板自身 CPU/runtime/heap/temperature 的可用性，并在 Zephyr 暴露可靠来源时给出数值；watchdog supervisor 也会周期性打印 heap 诊断，方便排查短复位 |
| TF/SD 路由 | 在 `target` 和 `usb-reader` 之间切换 |
| GPIO | 安全白名单：`GP4`、`GP7`、`GP8`、`GP13`-`GP24` |
| 固件自主 watchdog 恢复 | 固件监督 watchdog；当核心服务不再上报健康状态时自动复位并进入 ROM BOOTSEL |
| 固件更新 | 通过 USB 命令让 RP2040 进入 BOOTSEL |

`5V_FIN` 会被当作独立的输入/来源电源处理，不作为可控输出暴露给主机。

## 给 AI Agent 的使用入口

AI Agent 在操作硬件前，应先读取
[skills/agent-debugboard/SKILL.md](skills/agent-debugboard/SKILL.md)。这份 skill
是仓库内面向 Agent 的权威操作规程，包含主力主机侧 CLI 的构建/运行、连接诊
断、JSON 命令使用和有副作用操作的安全规则。

在修改仓库文件之前，AI Agent 还应先读取 [AGENTS.md](AGENTS.md)。仓库内的
默认规则如下：

- 任何代码改动都必须在同一个改动中同步更新对应的 skill 和文档。
- 修改固件行为或主机侧 CLI 逻辑时，必须同步更新相关说明并运行对应测试。
- 修改固件时，必须检查并保持 USB CDC ACM 串口的 BOOTSEL fallback 路径可用。
- 修改 skill 时，必须执行一次 subagent 验证/测试。
- 添加新功能时，只要实际可行，就应同步添加对应的功能测试。
- 只要 Zephyr 绑定和板级模型能自然表达，硬件描述就优先使用 Device Tree，而不是固件里的硬编码表。
- 软件实现应保持标准、统一、优雅，避免引入让维护、自动化或文档理解变得困难的临时性写法。
- MCU 侧输出应尽量贴近接口原值；只要不破坏原始固件契约，解释、校准和展示优先放在 host 侧完成。

推荐 Agent 最小流程：

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- --version
cargo run --manifest-path cmd-ng/Cargo.toml -- --json doctor
cargo run --manifest-path cmd-ng/Cargo.toml -- --json status
```

如果主机侧 CLI 尚未构建或安装，先按 skill 中的命令处理。自动化场景优先使用
`--json`，解析 `schema`、`ok`、`command` 和 `error.code`，不要解析面向人看的文
本输出。

## 安装主机侧 CLI

当前主力主机侧 CLI 是 Rust `cmd-ng` 实现。旧 Go `agent-debugboardctl` 路径已弃
用。

在 checkout 内，直接构建/运行当前主力 Rust 主机 CLI：

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- --help
cargo run --manifest-path cmd-ng/Cargo.toml -- --version
cargo run --manifest-path cmd-ng/Cargo.toml --
```

旧 Go 安装脚本仅保留作兼容迁移用途。若确有需要，也可以指定 legacy release 版
本：

```sh
./skills/agent-debugboard/scripts/install.sh --version <tag>
```

私有仓库 release 下载需要先提供 GitHub token，并显式指定 release 版本。
已经登录 GitHub CLI 的机器可以直接使用 `gh auth token`：

```sh
export GH_TOKEN="$(gh auth token)"
./skills/agent-debugboard/scripts/install.sh --version <tag>
```

Windows PowerShell：

```powershell
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\skills\agent-debugboard\scripts\install.ps1
```

私有仓库 PowerShell release 下载：

```powershell
$env:GH_TOKEN = gh auth token
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\skills\agent-debugboard\scripts\install.ps1 -Version <tag>
```

也可以从 GitHub Release 手动下载匹配 OS 和 CPU 的产物：

| 系统 / CPU | 产物 |
| --- | --- |
| Windows x64 | `agent-debugboardctl-rust_windows_amd64.zip` |
| Windows arm64 | `agent-debugboardctl-rust_windows_arm64.zip` |
| Linux x64 | `agent-debugboardctl-rust_linux_amd64.tar.gz` |
| Linux arm64 | `agent-debugboardctl-rust_linux_arm64.tar.gz` |
| macOS Intel | `agent-debugboardctl-rust_darwin_amd64.tar.gz` |
| macOS Apple Silicon | `agent-debugboardctl-rust_darwin_arm64.tar.gz` |

如果需要保留作对照或迁移用途的旧 Go host CLI，GitHub Release 中还会额外提供
`agent-debugboardctl_<os>_<arch>.*` 兼容归档；主力 Rust CLI/TUI 则会以
`agent-debugboardctl-rust_<os>_<arch>.*` 的名字一并发布，skill 安装脚本也会优先下载它。

macOS 上未签名的 release 二进制可能触发 Gatekeeper，提示 Apple 无法验证软件。
安装脚本会先校验 `SHA256SUMS.txt`，再移除安装后二进制的 quarantine 标记。
如果手动安装，请先校验 SHA256，再执行：

```sh
xattr -dr com.apple.quarantine ./skills/agent-debugboard/scripts/bin/agent-debugboardctl
```

构建/安装后验证：

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- --help
cargo run --manifest-path cmd-ng/Cargo.toml -- --version
cargo run --manifest-path cmd-ng/Cargo.toml -- doctor
cargo run --manifest-path cmd-ng/Cargo.toml --
```

直接运行 Rust 主机 CLI（不带子命令）会启动交互式 TUI。需要传统命令行
模式时，再使用 `status`、`adc read`、`power set` 等子命令。

TUI 本身只维持较温和的 60 Hz 重绘节奏，并改为通过 HTTP 轮询状态与 ADC 数据，
因此多个 TUI 实例可以稳定同时打开。电源输出、switch 控制和安全 GPIO 现在统一进
控制面：power 保持独立分区，`Switch` 分区会同时显示 `switch sd [target|usb-reader]`
与 `switch usb [pc|target]`，GPIO 仍单独显示。方向键或 Tab 移动选择，Space/Enter
切换当前项，`i` 把当前 GPIO 切回输入模式；`t` / `u` 快捷键仍然直接作用于 SD switch。
状态区现在会同时显示 switch 的 `desired`（本地目标状态）与 `actual`（后端回读状态），
方便现场区分“看起来稳定”和“真实稳定”。高频采样改由 `adc record` 负责：它会创建 live websocket
session，并把 telemetry 记录为 NDJSON 文件。默认订阅速率为 1000Hz，也可用
`--rate-hz HZ` 指定更低速率。每条输出记录都会包含主机接收时间戳和
`metadata.requested_rate_hz`；如果固件 telemetry 带有设备侧 timing 字段，recorder
会把它们写入 `metadata.device_timing`。当前 ADC telemetry 有 `sequence`，但没有
显式设备时间戳，因此 `device_timing` 可能不存在。

## 构建固件

创建 Python 环境并拉取 Zephyr：

```sh
python3 -m venv .venv
source .venv/bin/activate
pip install -U pip west

west init -l .
west update
west zephyr-export
pip install -r zephyr/scripts/requirements.txt
```

如果还没有安装 Zephyr SDK，需要先安装。当前本地构建已用 Zephyr SDK
`1.0.1` 验证过。

构建 RP2040 固件：

```sh
source .venv/bin/activate
west build -p always -b rpi_pico/rp2040 apps/agent_debugboard -d build/agent_debugboard
```

生成的 UF2 文件位置：

```text
build/agent_debugboard/zephyr/zephyr.uf2
```

本仓库的固件 build/flash 路径应固定不变：始终构建到
`build/agent_debugboard/`，始终刷写
`build/agent_debugboard/zephyr/zephyr.uf2`。不要切换到其它 build 目录，也不要
使用临时挂载点里残留的旧 UF2。

## 刷写

如果板子当前已经运行本固件，可以先让它进入 BOOTSEL，再刷写新的 UF2：

```sh
agent-debugboardctl bootloader
picotool load -v -x build/agent_debugboard/zephyr/zephyr.uf2
```

每次修改固件后，都应把这条 BOOTSEL 流程和下方 RP2040 CDC ACM 串口 shell 的
fallback 路径当作必做验收项；确认串口 fallback 仍然可用后，才能结束该改动。

如果 HTTP/WS 控制面不可用，但 RP2040 的 CDC ACM shell 还在，也可以直接在本地
Zephyr shell 里进入同一条 BOOTSEL 路径：

```text
debugboard:~$ bootloader
```

如果板子已经以 `RPI-RP2` 磁盘方式挂载，只需要执行：

```sh
picotool load -v -x build/agent_debugboard/zephyr/zephyr.uf2
```

如果改用 `RPI-RP2` 盘符拖拽复制，而不是 `picotool`，也只能复制这一个固定产物：

```text
build/agent_debugboard/zephyr/zephyr.uf2
```

## GitHub Actions 产物

`Build` workflow 会检查每次 push 和 pull request。推送 `v*` tag 会触发
`Release` workflow，自动构建固件、打包主机 CLI、创建 GitHub Release，并上传
固定命名的 release assets。

- `agent-debugboard-rp2040.uf2`：用于拖拽刷写或 `picotool` 的 RP2040 固件。
- `agent-debugboard-rp2040.elf`：用于调试的 RP2040 ELF。
- `agent-debugboard-rp2040.map`：RP2040 链接 map。
- `agent-debugboardctl-rust_windows_amd64.zip`：Windows x64 主力 Rust CLI/TUI。
- `agent-debugboardctl-rust_windows_arm64.zip`：Windows arm64 主力 Rust CLI/TUI。
- `agent-debugboardctl-rust_linux_amd64.tar.gz`：Linux x64 主力 Rust CLI/TUI。
- `agent-debugboardctl-rust_linux_arm64.tar.gz`：Linux arm64 主力 Rust CLI/TUI。
- `agent-debugboardctl-rust_darwin_amd64.tar.gz`：macOS Intel 主力 Rust CLI/TUI。
- `agent-debugboardctl-rust_darwin_arm64.tar.gz`：macOS Apple Silicon 主力 Rust CLI/TUI。
- `agent-debugboardctl_<os>_<arch>.*`：弃用 Go CLI 的兼容归档。
- `skills-agent-debugboard.tar.gz`：`skills/agent-debugboard/` 的 Agent skill 打包。
- `SHA256SUMS.txt`：所有 release assets 的 SHA256 校验文件。

开发者可以从源码构建当前主力 host CLI：

```sh
cargo build --manifest-path cmd-ng/Cargo.toml
./cmd-ng/target/debug/agent-debugboardctl --help
```

Rust `cmd-ng` 现在是主力开发路径，可直接构建运行：

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- --help
cargo run --manifest-path cmd-ng/Cargo.toml -- --json status
cargo run --manifest-path cmd-ng/Cargo.toml --
```

不带子命令时会进入 Rust TUI。

## 主机侧使用

查询调试板状态：

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- status
cargo run --manifest-path cmd-ng/Cargo.toml -- doctor
```

Agent 或自动化程序推荐优先使用 JSON 输出。JSON 响应固定包含
`schema: "agent-debugboard.v1"`、`ok`、`command`，成功时返回命令相关字段，
失败时返回 `error: {code, message}`：

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- --json doctor
cargo run --manifest-path cmd-ng/Cargo.toml -- --json status
cargo run --manifest-path cmd-ng/Cargo.toml -- --json power list
cargo run --manifest-path cmd-ng/Cargo.toml -- --json adc read
cargo run --manifest-path cmd-ng/Cargo.toml -- --json gpio list
cargo run --manifest-path cmd-ng/Cargo.toml -- --json watchdog status
```

`GP13` 这类 GPIO 名称由固件按 RP2040 引脚号派生；每个 allowlist GPIO 还带有
板级 `note`，例如 `CON_MAS` 或 `J17_PIN1`。CLI/TUI 会同时显示二者，HTTP/CLI
控制同时接受 `GPxx`、原始数字引脚（例如 `4`）以及精确 note（例如 `CON_MAS`）。

状态 JSON 还包含 `board_monitoring`。其中 `temperature`、`heap`、`runtime` 和
`cpu` 每一类都会带 `available` 和机器可读的 `reason`。固件只会在 Zephyr 设备或
runtime stats API 已启用且可读取时报告真实数值；当前默认 RP2040 配置会启用
内部 CPU die temperature、system heap runtime 统计、真实板端 uptime
（`uptime_ms` / `uptime_seconds`）以及 CPU utilization delta。只有在读取失败或
CPU 统计窗口尚不足以推导百分比时，相关字段才会继续返回 `reason`（例如
`insufficient_runtime_window`），而不会伪造数据。WebSocket `snapshot/status`
与 `GET /api/v1/status` 使用同一个 `board_monitoring` 对象。

为了排查短时间复位，watchdog supervisor 线程还会在固件日志中周期性打印内存
诊断。日志优先包含 system heap 已分配、空闲、总量和峰值字节数，并同时输出
真实 uptime。该路径只读，不会推进 `board_monitoring` 使用的 CPU utilization
采样窗口。

同样按 1 Hz 节奏，固件还会输出一条 watchdog trace，明确记录 supervisor 是否
还活着、这一次喂硬件 watchdog 是成功、失败还是被跳过，以及当前是 `core`、
`api` 还是 `cmdline` 在阻止继续喂狗。它的目的就是在短时复位排查中直接回答
\u201c到底是谁让 watchdog 没有被持续喂到\u201d。

启动时固件会打印上次 reset 原因；当通过 watchdog 恢复路径进入 ROM BOOTSEL
时，也会记录此次进入是来自显式 bootloader 请求还是 watchdog 不健康停止喂狗。
USB 设备生命周期事件同样会被记录，便于与 CDC ACM 断连做诊断关联。

控制电源输出：

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- power set 12v_out on
cargo run --manifest-path cmd-ng/Cargo.toml -- power set 12v_out off
cargo run --manifest-path cmd-ng/Cargo.toml -- power set 5v_out on
cargo run --manifest-path cmd-ng/Cargo.toml -- power set 5v_out off
cargo run --manifest-path cmd-ng/Cargo.toml -- power set 5v_ws on
cargo run --manifest-path cmd-ng/Cargo.toml -- power set 20v_out on
```

读取电流监测 ADC 通道：

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- adc read
cargo run --manifest-path cmd-ng/Cargo.toml -- adc read 5v_out
cargo run --manifest-path cmd-ng/Cargo.toml -- adc record /tmp/adc.ndjson 1000 --rate-hz 250
cargo run --manifest-path cmd-ng/Cargo.toml -- adc read -v 5v_out
cargo run --manifest-path cmd-ng/Cargo.toml -- adc read 12v_out
cargo run --manifest-path cmd-ng/Cargo.toml -- adc read 20v_out
```

人类可读 ADC 输出默认保持简洁，例如 `5v_out=0.540000A`。需要调试字段时使用
`-v` / `--verbose`，会额外输出 `signal`、`mv` 等信息。`agent-debugboardctl --json adc read`
返回完整诊断链路字段：`raw`、`mv`、`current_ua`、`sensor_value` 和电源状态。
主机侧不再附加 ADC 校准表或零点修正。

切换 switch 路由：

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- switch list
cargo run --manifest-path cmd-ng/Cargo.toml -- switch get sd
cargo run --manifest-path cmd-ng/Cargo.toml -- switch get usb
cargo run --manifest-path cmd-ng/Cargo.toml -- switch route sd usb-reader
cargo run --manifest-path cmd-ng/Cargo.toml -- switch route usb target
```

做 mux/switch 功能验证时，建议严格顺序执行：先运行 `switch route ...`，再给硬件
一点稳定时间（本地验证时通常等待几秒），然后再执行 `switch get ...`。如果目标是验证
稳定性，不要对同一块板并行或交错执行相反方向的切换命令。

使用安全 GPIO：

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- gpio list
cargo run --manifest-path cmd-ng/Cargo.toml -- gpio set GP13 1
cargo run --manifest-path cmd-ng/Cargo.toml -- gpio set CON_MAS 1
cargo run --manifest-path cmd-ng/Cargo.toml -- gpio input J17_PIN1
cargo run --manifest-path cmd-ng/Cargo.toml -- gpio input GP13
```

使用固件自主 watchdog 恢复：

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- watchdog status
```

watchdog 由固件自身管理，而不是由主机喂狗。固件会自动 arm RP2040 硬件
watchdog，并且只有在核心固件循环、HTTP/API 服务、以及 CDC ACM cmdline
fallback 都持续上报本地存活时才继续喂狗。WebSocket 会话静默、订阅超时、以及
会话过期都不会被当作 watchdog 失败条件。如果核心固件卡死、API 服务停止响应、
或 CDC ACM cmdline fallback 不再上报存活，固件就会停止喂狗，RP2040 随后复位，
并在下一次最早启动路径通过保留恢复标记进入标准 ROM BOOTSEL。直接
`bootloader` 命令和 CDC ACM shell fallback 仍是独立路径，行为不变。周期性内存
诊断只是日志输出，不会增加 watchdog 参与者，也不会改变 BOOTSEL marker/reset
语义。watchdog trace 也只是日志，不会改变实际喂狗策略。

## OpenOCD / JTAG

Agent DebugBoard 可以和 OpenOCD 配合使用：DebugBoard 负责目标板供电和恢复
控制，板载 CH347F 路径负责目标板 JTAG/SWD。CH347F 直连目标调试口，RP2040
不在 JTAG/SWD 数据链路中，也不会把 DebugBoard 自己模拟成 CMSIS-DAP 或 JTAG
probe。

安装 OpenOCD 后先确认版本：

```sh
openocd --version
```

先给目标板供电，然后使用本机 OpenOCD 安装里的 CH347F interface 配置和目标板
对应的 target 配置启动 OpenOCD：

```sh
agent-debugboardctl --json power set 5v_out on
openocd -f interface/<ch347-interface>.cfg -f target/<target>.cfg
```

CH347F 支持取决于 OpenOCD 构建版本。如果系统包没有 CH347F interface script，
需要使用 WCH/vendor OpenOCD 构建，或补充匹配的 interface 配置。

OpenOCD 通常会在 TCP `3333` 暴露 GDB server，并在 TCP `4444` 暴露 telnet
控制接口。目标板重启优先使用 OpenOCD 的 `reset halt`、`reset run` 或目标系统
自己的软重启；只有软重启不可行时，才使用电源输出断电再上电作为硬重启 fallback。

完整流程见 [doc/openocd/README.md](doc/openocd/README.md)。

## NCM 网络接口

固件枚举为复合 USB 设备。主机 CLI 通过 USB NCM 接口上的 HTTP 与调试板
通信。默认设备 URL 为 `http://172.29.203.1:8080`。调试板会在 NCM 链路上运行
一个小型 DHCPv4 server，让 host 自动拿到兼容地址；如果你修改了默认地址规划，
可用 `agent-debugboardctl --url ...` 显式指定。

所有控制均通过 `agent-debugboardctl` 的 HTTP JSON 请求，或直接通过
`curl` / 任意 HTTP 客户端完成。RP2040 USB CDC ACM 串口会继续保留，作为
Zephyr 通用 cmdline 和 BOOTSEL fallback 的辅助通道，但它不是主控制面。只要
CDC ACM shell 可用，本地 `bootloader` shell 命令就会走与 HTTP API 相同的
RP2040 ROM BOOTSEL 路径。

如果需要在一条长连接上同时做实时遥测和双向控制，固件还提供
`ws://172.29.203.1:8080/api/v1/ws` WebSocket 端点。
WebSocket 客户端可以从状态快照里观察 watchdog 状态，但不能手动喂狗；watchdog
监督完全由固件自主完成。

mDNS 暂时不作为首发必需项。现在 DHCP 已经解决跨平台即插即用的地址获取问题；
mDNS 只是后续可选的“名字更友好”增强，不影响正常使用。

## 硬件映射

| 功能 | 固件名称 | 原理图信号 |
| --- | --- | --- |
| 12 V 输出使能 | `12v_out` | `GP02_12V_EN` |
| 5 V 输出使能 | `5v_out` | `GP05_5V_EN` |
| 5 V WS 使能 | `5v_ws` | `GP09_5V_WS_EN` |
| 20 V 输出使能 | `20v_out` | `GP10_20V_EN` |
| TF/SD 路由切换 | `switch sd` | `GP06_TF_SW` |
| USB mux 切换 | `switch usb` | `GP03_USB_MUX` |
| 5 V 电流监测 | `adc read 5v_out` | `S_C_5V` |
| 12 V 电流监测 | `adc read 12v_out` | `S_C_12V` |
| 20 V 电流监测 | `adc read 20v_out` | `S_C_20V` |

电流监测通道使用 INA139、10 mOhm 采样电阻、51 kOhm 输出负载和
1000 uA/V 跨导。MCU 同时上报原始 ADC 调试值，以及通过 Zephyr
`current-sense-amplifier` 标准接口得到的电流值；主机侧现在直接展示这些值，
不再做 ADC 校准表或零点修正。
传感器传输函数参考公开的
[TI INA139 规格书](https://www.ti.com/product/INA139)。

当前原理图副本放在
[doc/agent-debugboard-schematic.pdf](doc/agent-debugboard-schematic.pdf)。

## 开发

运行单元测试：

```sh
./apps/agent_debugboard/tests/run_unit_tests.sh
```

测试脚本覆盖：

- 共享板级模型的 host C 单元测试。
- 主机侧 CLI 辅助工具的 Go 测试。仓库内测试脚本会只跑真实 Go 源码目录
  （`./cmd/...` 与 `./internal/...`），而不会在仓库根目录直接执行
  `go test ./...`，以避免 Zephyr/CMake 在 `build/` 下生成的产物树被误扫进
  Go 包发现流程。

## 仓库结构

```text
apps/agent_debugboard/        Zephyr 应用
apps/agent_debugboard/src/    固件源码和共享板级模型
apps/agent_debugboard/tests/  单元测试
cmd/agent-debugboardctl/      Go 主机侧 CLI 入口
internal/hostcli/             Go 主机侧 CLI 实现
doc/                          硬件文档、OpenOCD 配置和宣传素材
skills/agent-debugboard/      面向 Agent 的 skill 和操作规程
.goreleaser.yaml              GoReleaser 主机侧 CLI 打包配置
go.mod, go.sum                主机侧 CLI Go module
west.yml                      Zephyr workspace manifest
```
