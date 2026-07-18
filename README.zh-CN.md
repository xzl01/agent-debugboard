# radxa-linkr-debugger

[English](README.md)

`radxa-linkr-debugger` 是 **Radxa Linkr Debugger** 的 RP2350 固件。它把一块硬件
调试板变成 PC 侧 Agent/AI 可以直接操作的 USB 控制接口，用于控制目标开发板
或 SBC 的供电、刷机模式、TF/SD 路由、电流监测 ADC 和一组安全 GPIO。

![Radxa Linkr Debugger 宣传图](doc/marketing/radxa-linkr-debugger-promo.png)

## 项目简介

Radxa Linkr Debugger 面向自动化 bring-up、远程恢复、产测和 AI agent 调试链路。
固件会枚举为复合 USB 设备：以 USB NCM 网络接口作为主控制面，并保留一个
USB CDC ACM 串口用于 Zephyr 通用 cmdline 和 BOOTSEL fallback。普通用户通过 Web UI
操作；高级用户、Agent、自动化和 HIL 验证使用 release 发布的 Rust
`radxa-linkr-debuggerctl` CLI/TUI，其源码位于 `cmd-ng/`。

本仓库包含 Zephyr 应用、Web UI、Rust 主机侧 CLI/TUI、单元测试、原理图副本和项目文档。

高级主机侧工具路径是 [`cmd-ng/`](cmd-ng/)。release 发布的
`radxa-linkr-debuggerctl` 通过 USB NCM 上的 HTTP API 与调试板通信，并提供机器可读
自动化、诊断、录制和交互式 TUI。

## 功能范围

| 模块 | 当前固件支持 |
| --- | --- |
| USB 控制 | 复合 USB 设备：NCM HTTP/WS 主控制面 + CDC ACM fallback 控制台 |
| 主机自动化 | Rust `cmd-ng` CLI/TUI，支持 JSON 输出和 `doctor` 诊断 |
| 实时遥测 | live session 返回的 `/api/v1/ws/<slot>` 双向 WebSocket 长连接 |
| 逻辑分析仪 | RP2350 PIO2+DMA 高速单次捕获；1-125MHz 可调采样率；512 样本上限；安全引脚 GP7-GP20/GP29；none/rising/falling/either 边沿触发；边沿触发 ≤25 MHz 支持预触发采样；1-25 MHz 连续流式采样（SCPI-over-WebSocket 示波器协议帧驱动浏览器实时波形，`ws://<board>/api/v1/scpi`）；支持 CSV 和 PulseView (.sr) 导出；**PulseView 原生接入**：Rigol DS1102D SCPI 仿真（rigol-ds 驱动，端口 80 `tcp-raw`，15 路数字通道 + GP29 模拟 CH1，≤25 MHz 硬件预触发，>25 MHz 突发触发，无沿 AUTO 回退）；板载 Web UI 中有浏览器内 Rust/WASM 解码器，支持 UART/I2C/SPI 协议 |
| 电源输出 | `12v_out`、`5v_out`、`20v_out` |
| ADC 监测 | 读取 `5v_out`、`12v_out`、`20v_out` 的电流监测通道 |
| 调试板自监控 | `/api/v1/status` 和 WebSocket 状态快照会报告板自身 CPU/runtime/heap/temperature 的可用性，并在 Zephyr 暴露可靠来源时给出数值；watchdog supervisor 也会周期性打印 heap 诊断，方便排查短复位 |
| TF/SD 路由 | 在 `target` 和 `usb-reader` 之间切换 |
| VIN 控制 | 在 `1.8v` 和 `3.3v` 之间切换；固件使用 Device Tree VIO regulator |
| GPIO | `GP7`、`GP8`、`GP9`、`GP10`-`GP20`、`GP29` |
| 固件自主 watchdog 恢复 | 固件监督 watchdog；当核心服务不再上报健康状态时自动复位并进入 ROM BOOTSEL |
| 固件更新 | 通过 USB 命令让 RP2350 进入 BOOTSEL |
| MCUboot OTA | 无签名 RP2350 专用 MCUboot OTA；仅用 SHA256 做完整性校验；无签名、无认证、无安全启动、无防回滚；测试镜像在 16 秒 watchdog 健康门槛后自动确认；未确认前发生 watchdog 复位时，专用 retained marker 允许 MCUboot 回滚而非强制进入 ROM BOOTSEL |
| 强制门户发现 | DHCPv4 option 114、UDP 53 泛解析 A 记录、AAAA NOERROR/NODATA、HTTP port 80 监听最大化 OS 自动打开概率；port 80/DNS 为兼容性辅助；自动打开为 best-effort，不保证 |

`5V_FIN` 会被当作独立的输入/来源电源处理，不作为可控输出暴露给主机。

## 给 AI Agent 的使用入口

AI Agent 在操作硬件前，应先读取
[skills/radxa-linkr-debugger/SKILL.md](skills/radxa-linkr-debugger/SKILL.md)。这份 skill
是仓库内面向 Agent 的权威操作规程，并且有意保持 curl-first；它同时覆盖连接诊
断、需要时构建/运行主力 CLI、JSON 命令使用和有副作用操作的安全规则。

在修改仓库文件之前，AI Agent 还应先读取 [AGENTS.md](AGENTS.md)。仓库内的
默认规则如下：

- 任何代码改动都必须在同一个改动中同步更新对应的 skill 和文档。
- 修改固件行为或主机侧 CLI 逻辑时，必须同步更新相关说明并运行对应测试。
- 修改固件时，必须检查并保持 USB CDC ACM 串口的 BOOTSEL fallback 路径可用。
- 修改 skill 时，必须执行一次 subagent 验证/测试。
- 添加新功能时，只要实际可行，就应同步添加对应的功能测试。
- 修改固件或与真实硬件交互的主机侧逻辑时，结束前必须完成 HIL 功能测试；参见 `AGENTS.md` 和 `doc/testing/hil-functional-test-spec.md`。
- 只要 Zephyr 绑定和板级模型能自然表达，硬件描述就优先使用 Device Tree，而不是固件里的硬编码表。
- 软件实现应保持标准、统一、优雅，避免引入让维护、自动化或文档理解变得困难的临时性写法。
- MCU 侧输出应尽量贴近接口原值；只要不破坏原始固件契约，解释、校准和展示优先放在 host 侧完成。

推荐 Agent 最小流程：

```sh
radxa-linkr-debuggerctl --version
radxa-linkr-debuggerctl --json doctor
radxa-linkr-debuggerctl --json status
```

如果 release CLI 尚未下载或安装，先按下方 release 安装路径处理。只有在执行
Agent skill 本身时，才继续遵循 skill 的 curl-first 工作流。通过 CLI 做自动化时，
优先使用 `--json`，解析 `schema`、`ok`、`command` 和 `error.code`，不要解析面向
人看的文本输出。

## 安装高级用户/Agent 主机侧 CLI

高级用户和 Agent 可以安装 release 发布的 Rust `radxa-linkr-debuggerctl` CLI。可以
直接从 GitHub Releases 下载匹配平台的归档，或者在 checkout 内使用下方
repo-local installer，并显式指定版本，让它下载已发布的 release 产物而不是从源码构建。

安装指定 release 版本：

```sh
./skills/radxa-linkr-debugger/scripts/install.sh --version <tag>
```

私有仓库 release 下载需要先提供 GitHub token，并显式指定 release 版本。
已经登录 GitHub CLI 的机器可以直接使用 `gh auth token`：

```sh
export GH_TOKEN="$(gh auth token)"
./skills/radxa-linkr-debugger/scripts/install.sh --version <tag>
```

Windows PowerShell：

```powershell
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\skills\radxa-linkr-debugger\scripts\install.ps1
```

私有仓库 PowerShell release 下载：

```powershell
$env:GH_TOKEN = gh auth token
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File .\skills\radxa-linkr-debugger\scripts\install.ps1 -Version <tag>
```

也可以从 GitHub Release 手动下载匹配 OS 和 CPU 的产物：

| 系统 / CPU | 产物 |
| --- | --- |
| Windows x64 | `radxa-linkr-debuggerctl-rust_windows_amd64.zip` |
| Linux x64 | `radxa-linkr-debuggerctl-rust_linux_amd64.tar.gz` |
| macOS Apple Silicon | `radxa-linkr-debuggerctl-rust_darwin_arm64.tar.gz` |

macOS 上未签名的 release 二进制可能触发 Gatekeeper，提示 Apple 无法验证软件。
安装脚本会先校验 `SHA256SUMS.txt`，再移除安装后二进制的 quarantine 标记。
如果你是手动解压 release 归档，请先校验 SHA256，再对解压得到的
`radxa-linkr-debuggerctl` 二进制执行：

```sh
xattr -dr com.apple.quarantine ./radxa-linkr-debuggerctl
```

安装后，下面的示例默认假设 `radxa-linkr-debuggerctl` 已在 `PATH` 中，或者你用相同
命令名直接调用刚解压出来的 release 二进制：

```sh
radxa-linkr-debuggerctl --help
radxa-linkr-debuggerctl --version
radxa-linkr-debuggerctl doctor
radxa-linkr-debuggerctl
```

直接运行 release CLI（不带子命令）会启动交互式 TUI。需要传统命令行
模式时，再使用 `status`、`adc read`、`power set` 等子命令。

如果你是在开发 `cmd-ng` 自身，可直接从源码构建：

```sh
cargo build --manifest-path cmd-ng/Cargo.toml
./cmd-ng/target/debug/radxa-linkr-debuggerctl --help
```

TUI 本身只维持较温和的 60 Hz 重绘节奏，并改为通过 HTTP 轮询状态与 ADC 数据，
因此多个 TUI 实例可以稳定同时打开。电源输出、switch 控制和安全 GPIO 现在统一进
控制面：power 保持独立分区，`Switch` 分区会同时显示 `switch sd [target|usb-reader]`
与 `switch usb [pc|target]`，GPIO 仍单独显示。方向键或 Tab 移动选择，Space/Enter
切换当前项，`i` 把当前 GPIO 切回输入模式；`t` / `u` 快捷键仍然直接作用于 SD switch。
状态区现在会同时显示 switch 的 `desired`（本地目标状态）与 `actual`（后端回读状态），
方便现场区分“看起来稳定”和“真实稳定”。高频采样改由 `adc record` 负责：它会创建 live websocket
session，并把 telemetry 记录为 NDJSON 或 CSV 文件。默认订阅速率为 1000Hz，也可用
`--rate-hz HZ` 指定更低速率。每条输出记录都会包含主机接收时间戳和
`metadata.requested_rate_hz`。请求速率高于 100Hz 时，固件在线路上使用 batch JSON，
recorder 再把每个设备样本展开成独立 NDJSON 或 CSV 行。固件单样本 telemetry 保留
`sequence` 与 `uptime_us`，并同时提供 `sample_sequence` 和 `device_t_mono_us`；紧凑 batch
样本在线路上保留 `sequence` 与 `uptime_us`，recorder 会将其归一化为相同的别名，并兼容
显式提供别名的固件。设备时间字段会保留在 `metadata.device_timing` 中；CSV 的时间列优先使用
`device_t_mono_us`，缺失时回退到 `uptime_us`，再否则为 0；采样环发生覆盖时，首个受影响记录会携带
 `metadata.dropped_samples`。

 ## 逻辑分析仪

 固件逻辑分析仪使用 RP2350 PIO2+DMA 进行高速单次 GPIO 捕获。它不是轮询
 式诊断工具，不支持高采样率持续流式传输。Web UI 在 **Terminal 工作区**
 中集成了项目自主研发的 Rust/WASM 解码器，可直接在浏览器内解码采样数据。
 稳定的解码器 URL 为：
 - `/assets/decoder/logic-decoder.js`
 - `/assets/decoder/logic-decoder_bg.wasm`

 解码器以 `application/wasm` MIME 类型提供 gzip 压缩的 WASM 资源。
 该解码器仅支持 UART、I2C、SPI 协议，是项目自主研发的 Rust/WASM 实现，
 灵感来自 sigrok 解码器语义，但并非 libsigrokdecode Python 插件兼容层，
 也不提供完整的插件兼容性。

 HTTP 配置接受最多 16 个安全引脚（GP7、GP8、GP9、GP10-GP20、GP29）
 的通道数组，采样率从 1,000,000 到 125,000,000 Hz（1-125MHz），
 post_samples 从 1 到 512。捕获上限为 512 样本。

 支持的触发模式为 `none`、`rising`、`falling` 和 `either`。
 PIO 边沿触发等待所选边沿出现后才开始捕获；不支持预触发，
 因此 `pre_samples > 0` 会被 HTTP 400 拒绝。

 逻辑分析仪位于 Web UI 的 **Terminal 工作区**，与串口终端并列。
 捕获完成后可在波形视图中预览并导出为 CSV 或 PulseView (.sr) 格式。
 详细文档见 [doc/logic-analyzer.md](doc/logic-analyzer.md)。

 首次集成测量时，canonical 固件构建使用 605476 / 847832 字节（71.41%），
 无需 A/B 分区。实际用量因配置和构建选项而异。

## 构建固件

 固件构建包含 Web UI。Node.js 22、npm、Rust toolchain、`wasm32-unknown-unknown`
 target 和 `wasm-bindgen-cli 0.2.121` 都必须在构建前就位；CMake 会自动运行
 Web 构建并将 gzip 压缩资源嵌入 flash。

 创建 Python 环境并拉取 Zephyr：

```sh
python3 -m venv .venv
source .venv/bin/activate
pip install -U pip west

west init -l .
west update
west zephyr-export
pip install -r zephyr/scripts/requirements.txt
pip install -r bootloader/mcuboot/scripts/requirements.txt
```

如果还没有安装 Zephyr SDK，需要先安装。当前本地构建已用 Zephyr SDK
`1.0.1` 验证过。

构建 RP2350 固件：

```sh
source .venv/bin/activate
west build -p always -b rpi_pico2/rp2350a/m33/mcuboot --sysbuild apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
```

RP2350 sysbuild 的应用产物位置：

```text
build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin
build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.hex
```

`zephyr.signed.bin` 是 Zephyr/MCUboot 的格式化文件名；在本项目配置下，它是
用于 OTA 的无签名 MCUboot 格式应用二进制，并不是加密签名镜像。

本仓库的固件 build/flash 路径应固定不变：始终构建到
`build/radxa_linkr_debugger/`。RP2350 初次安装或恢复刷写 release
发布的合并型 MCUboot 加应用 UF2：`radxa-linkr-debugger-rp2350.uf2`。不要切换到其它
build 目录，也不要使用临时挂载点里残留的旧 UF2。

## 刷写

固件更新有两种方式，取决于板子当前状态。

### ROM BOOTSEL 刷写（初次安装或恢复）

如果板子当前已经运行本固件，可以先让它进入 BOOTSEL，再刷写新的 UF2：

```sh
radxa-linkr-debuggerctl bootloader
picotool load -v -x radxa-linkr-debugger-rp2350.uf2
```

每次修改固件后，都应把这条 BOOTSEL 流程和下方 CDC ACM 串口 shell 的
fallback 路径当作必做验收项；确认串口 fallback 仍然可用后，才能结束该改动。

如果 HTTP/WS 控制面不可用，但 MCU 的 CDC ACM shell 还在，也可以直接在本地
Zephyr shell 里进入同一条 BOOTSEL 路径：

```text
linkr-debugger:~$ bootloader
```

如果板子已经以 `RPI-RP2` 磁盘方式挂载，只需要执行：

```sh
picotool load -v -x radxa-linkr-debugger-rp2350.uf2
```

Linux 下也可以先用 `udisksctl` 挂载 `RPI-RP2`，再复制这一个固定 UF2：

```sh
RPI_RP2=$(udisksctl mount -b /dev/sdX1 | awk -F" at " '{print $2}' | tr -d '[:space:]')
cp radxa-linkr-debugger-rp2350.uf2 "$RPI_RP2/"
```

将 `/dev/sdX1` 替换为实际 BOOTSEL 块设备路径。

如果改用 `RPI-RP2` 盘符拖拽复制，而不是 `picotool`，也只能复制这一个固定产物：

```text
radxa-linkr-debugger-rp2350.uf2
```

### OTA 刷写（初次 MCUboot 安装后适用）

MCUboot 固件初次安装完成后，后续 RP2350 固件更新可通过 OTA 交付。准备好
MCUboot 格式的应用二进制文件后，按顺序执行：

```sh
radxa-linkr-debuggerctl ota upload /path/to/firmware.bin
radxa-linkr-debuggerctl ota test
# 验证测试启动成功后：
radxa-linkr-debuggerctl ota confirm
```

测试启动成功后，也可以等待 16 秒 watchdog 健康门槛自动确认镜像。若测试镜像未确认
时发生 watchdog 复位， retained marker 会驱动 MCUboot 回滚，而不会进入 ROM BOOTSEL。

不要通过 OTA 上传 `.uf2` 或 `.elf` 文件。OTA 接收的是 MCUboot 格式的应用二进制文件；
release 产物名为 `radxa-linkr-debugger-rp2350-ota.bin`，它来自 sysbuild 应用输出
`build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin`。虽然构建文件名
包含 `signed.bin`，但本项目配置下它是无签名 MCUboot 格式。

## GitHub Actions 产物

`Build` workflow 会检查每次 push 和 pull request。推送 `v*` tag 会触发
`Release` workflow，自动构建固件、打包主机 CLI、创建 GitHub Release，并上传
固定命名的 release assets。

- `radxa-linkr-debugger-rp2350.uf2`：用于初次安装、恢复、拖拽刷写或 `picotool` 的 RP2350 MCUboot 加应用合并固件。
- `radxa-linkr-debugger-rp2350-ota.bin`：RP2350 OTA 应用 payload，来自 sysbuild 的 `zephyr.signed.bin`；本项目配置下是无签名 MCUboot 格式。
- `radxa-linkr-debugger-rp2350.elf`：用于调试的 RP2350 ELF。
- `radxa-linkr-debugger-rp2350.map`：RP2350 链接 map。
- `radxa-linkr-debuggerctl-rust_windows_amd64.zip`：Windows x64 高级用户/Agent Rust CLI/TUI。
- `radxa-linkr-debuggerctl-rust_linux_amd64.tar.gz`：Linux x64 高级用户/Agent Rust CLI/TUI。
- `radxa-linkr-debuggerctl-rust_darwin_arm64.tar.gz`：macOS Apple Silicon 高级用户/Agent Rust CLI/TUI。
- `skills-radxa-linkr-debugger.tar.gz`：`skills/radxa-linkr-debugger/` 的 Agent skill 打包。
- `SHA256SUMS.txt`：所有 release assets 的 SHA256 校验文件。

高级用户和 Agent 可下载上面的 `radxa-linkr-debuggerctl-rust_*` 归档。若你在开发
`cmd-ng` 本身，可从源码构建：

```sh
cargo build --manifest-path cmd-ng/Cargo.toml
./cmd-ng/target/debug/radxa-linkr-debuggerctl --help
```

## 高级用户/Agent CLI 使用

查询调试板状态：

```sh
radxa-linkr-debuggerctl status
radxa-linkr-debuggerctl doctor
```

Agent 或自动化程序推荐优先使用 JSON 输出。JSON 响应固定包含
`schema: "radxa-linkr-debugger.v1"`、`ok`、`command`，成功时返回命令相关字段，
失败时返回 `error: {code, message}`：

```sh
radxa-linkr-debuggerctl --json doctor
radxa-linkr-debuggerctl --json status
radxa-linkr-debuggerctl --json power list
radxa-linkr-debuggerctl --json adc read
radxa-linkr-debuggerctl --json gpio list
radxa-linkr-debuggerctl --json watchdog status
```

`GP13` 这类 GPIO 名称由固件按 MCU 引脚号派生；每个 allowlist GPIO 还带有
板级 `note`，例如 `CON_MAS` 或 `J17_PIN1`。CLI/TUI 会同时显示二者，HTTP/CLI
控制同时接受 `GPxx`、原始数字引脚（例如 `4`）以及精确 note（例如 `CON_MAS`）。

状态 JSON 还包含 `board_monitoring`。其中 `temperature`、`heap`、`runtime` 和
`cpu` 每一类都会带 `available` 和机器可读的 `reason`。固件只会在 Zephyr 设备或
runtime stats API 已启用且可读取时报告真实数值；当前默认 RP2350 配置会启用
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
radxa-linkr-debuggerctl power set 12v_out on
radxa-linkr-debuggerctl power set 12v_out off
radxa-linkr-debuggerctl power set 5v_out on
radxa-linkr-debuggerctl power set 5v_out off
radxa-linkr-debuggerctl power set 20v_out on
```

板内 VDD_5V 电源轨不会出现在 CLI/TUI 的状态、电源列表或电源控制中。原始固件
API 仍保留兼容项，供底层诊断使用。

读取电流监测 ADC 通道：

```sh
radxa-linkr-debuggerctl adc read
radxa-linkr-debuggerctl adc read 5v_out
radxa-linkr-debuggerctl adc record /tmp/adc.ndjson 1000 --rate-hz 250
radxa-linkr-debuggerctl adc read -v 5v_out
radxa-linkr-debuggerctl adc read 12v_out
radxa-linkr-debuggerctl adc read 20v_out
```

人类可读 ADC 输出默认保持简洁，例如 `5v_out=0.540000A`。需要调试字段时使用
`-v` / `--verbose`，会额外输出 `signal`、`mv` 等信息。`radxa-linkr-debuggerctl --json adc read`
返回完整诊断链路字段：`raw`、`mv`、`current_ua`、`sensor_value` 和电源状态。
主机侧不再附加 ADC 校准表或零点修正。

切换 switch 路由：

```sh
radxa-linkr-debuggerctl switch list
radxa-linkr-debuggerctl switch get sd
radxa-linkr-debuggerctl switch get usb
radxa-linkr-debuggerctl switch route sd usb-reader
radxa-linkr-debuggerctl switch route usb target --confirm
```

VIN 控制：

```sh
radxa-linkr-debuggerctl switch get vin
radxa-linkr-debuggerctl switch route vin 3.3v --confirm
```

VIN 启动默认值为 3.3V。切换到 1.8V 属于专家操作：必须先确认外接目标支持
1.8V 信号电平，连接 VIO 物理测量设备，并明确接受硬件副作用。具体步骤见
[固件应用 README](apps/radxa_linkr_debugger/README.md#expert-g3-vin-18v-switching)
中的受控流程。

做 mux/switch 功能验证时，建议严格顺序执行：先运行 `switch route ...`，再给硬件
一点稳定时间（本地验证时通常等待几秒），然后再执行 `switch get ...`。如果目标是验证
稳定性，不要对同一块板并行或交错执行相反方向的切换命令。

使用安全 GPIO：

```sh
radxa-linkr-debuggerctl gpio list
radxa-linkr-debuggerctl gpio set GP13 1
radxa-linkr-debuggerctl gpio set CON_MAS 1
radxa-linkr-debuggerctl gpio input J16_PIN1
radxa-linkr-debuggerctl gpio input GP13
```

使用固件自主 watchdog 恢复：

```sh
radxa-linkr-debuggerctl watchdog status
```

watchdog 由固件自身管理，而不是由主机喂狗。固件会自动 arm MCU 硬件
watchdog，并且只有在核心固件循环、HTTP/API 服务、以及 CDC ACM cmdline
fallback 都持续上报本地存活时才继续喂狗。WebSocket 会话静默、订阅超时、以及
会话过期都不会被当作 watchdog 失败条件。如果核心固件卡死、API 服务停止响应、
或 CDC ACM cmdline fallback 不再上报存活，固件就会停止喂狗，MCU 随后复位，
并在下一次最早启动路径通过保留恢复标记进入标准 ROM BOOTSEL。直接
`bootloader` 命令和 CDC ACM shell fallback 仍是独立路径，行为不变。周期性内存
诊断只是日志输出，不会增加 watchdog 参与者，也不会改变 BOOTSEL marker/reset
语义。watchdog trace 也只是日志，不会改变实际喂狗策略。

## MCUboot OTA 固件更新（仅 RP2350）

RP2350 固件支持无签名 MCUboot OTA 空中更新。

**重要的安全说明**：此 OTA 路径不提供签名验证、不提供认证、不支持安全启动、不提供
防回滚保护。任何持有板子 USB NCM 访问权限的主机都可以提交固件镜像。SHA256 仅用于
校验上传负载的完整性，不用于认证发送方。

### 初次安装

MCUboot 固件的初次安装需要使用 ROM BOOTSEL 刷写合并型可启动镜像。初次安装完成后，
后续更新可以通过 OTA 使用 MCUboot 格式的应用二进制文件交付。

### OTA 工作流程

OTA 工作流程分三步：

1. 将 MCUboot 格式的应用二进制文件上传到板子。
2. 请求对新上传的镜像执行测试启动。
3. 验证镜像启动正常后手动确认，或等待 16 秒 watchdog 健康门槛自动确认。

如果测试镜像未经确认且发生了 watchdog 复位，专用 retained marker 会让 MCUboot 执行
回滚到上一个已确认镜像，而不会强制进入 ROM BOOTSEL。显式 `bootloader` 命令和普通
非 OTA watchdog 复位仍正常进入 ROM BOOTSEL。

### CLI 命令

```sh
radxa-linkr-debuggerctl ota status
radxa-linkr-debuggerctl ota upload /path/to/firmware.bin
radxa-linkr-debuggerctl ota test
radxa-linkr-debuggerctl ota confirm
```

`ota status` 报告当前 OTA 状态（`idle`、`uploading`、`verified`、`pending_test`、
`rebooting`、`failed`）、flash 大小和 MCUboot swap type。`ota upload` 发送
MCUboot 格式的 `.bin` 文件；CLI 计算 SHA256 并将字节大小和哈希作为 header 一起发送。
`ota test` 请求对已验证镜像执行测试启动；板子在短延迟后重启。如果测试启动后
watchdog 报告健康，镜像会在 16 秒门槛后自动确认。`ota confirm` 立即手动确认
当前运行的镜像，清除自动确认计时器。

Agent 或自动化程序推荐优先使用 JSON 输出：

```sh
radxa-linkr-debuggerctl --json ota status
radxa-linkr-debuggerctl --json ota upload /path/to/firmware.bin
radxa-linkr-debuggerctl --json ota test
radxa-linkr-debuggerctl --json ota confirm
```

## OpenOCD / JTAG

Radxa Linkr Debugger 可以和 OpenOCD 配合使用：Linkr Debugger 负责目标板供电和恢复
控制，板载 CH347F 路径负责目标板 JTAG/SWD。CH347F 直连目标调试口，RP2350
不在 JTAG/SWD 数据链路中，也不会把 Linkr Debugger 自己模拟成 CMSIS-DAP 或 JTAG
probe。

安装 OpenOCD 后先确认版本：

```sh
openocd --version
```

先给目标板供电，然后使用本机 OpenOCD 安装里的 CH347F interface 配置和目标板
对应的 target 配置启动 OpenOCD：

```sh
radxa-linkr-debuggerctl power set 5v_out on
openocd -f interface/<ch347-interface>.cfg -f target/<target>.cfg
```

CH347F 支持取决于 OpenOCD 构建版本。如果系统包没有 CH347F interface script，
需要使用 WCH/vendor OpenOCD 构建，或补充匹配的 interface 配置。

OpenOCD 通常会在 TCP `3333` 暴露 GDB server，并在 TCP `4444` 暴露 telnet
控制接口。目标板重启优先使用 OpenOCD 的 `reset halt`、`reset run` 或目标系统
自己的软重启；只有软重启不可行时，才使用电源输出断电再上电作为硬重启 fallback。

完整流程见 [doc/openocd/README.md](doc/openocd/README.md)。

## NCM 网络接口

固件枚举为复合 USB 设备，而 release 发布的 `radxa-linkr-debuggerctl` CLI 会通过
USB NCM 接口上的 HTTP 与调试板通信。默认设备 URL 为 `http://172.29.203.1`。
调试板会在 NCM 链路上运行一个小型 DHCPv4 server，让 host 自动拿到兼容地址；如
果你修改了默认地址规划，可用 `radxa-linkr-debuggerctl --url ...` 显式指定。

普通用户应使用 Web UI。高级用户和 Agent 可以使用 release 发布的 Rust CLI，它对
同一套 HTTP JSON API 做了包装；直接 `curl` 仍用于原始 API 调试和 curl-first 的
Agent skill。MCU USB CDC ACM 串口会继续保留，作为 Zephyr 通用 cmdline 和
BOOTSEL fallback 的辅助通道，但它不是主控制面。只要 CDC ACM shell 可用，本地
`bootloader` shell 命令就会走与 HTTP API 相同的 MCU ROM BOOTSEL 路径。

如果需要在一条长连接上同时做实时遥测和双向控制，应先通过 HTTP 创建 live
session，再连接响应中返回的 `/api/v1/ws/<slot>` 专用 WebSocket URL。当前固件一次
最多支持四个并发 WebSocket 客户端，每个 live session 分配独立的 `/api/v1/ws/<slot>` URL。
WebSocket 客户端可以从状态快照里观察 watchdog 状态，但不能手动喂狗；watchdog
监督完全由固件自主完成。

mDNS 暂时不作为首发必需项。现在 DHCP 已经解决跨平台即插即用的地址获取问题；
mDNS 只是后续可选的"名字更友好"增强，不影响正常使用。

## 强制门户发现

固件实现了多路径强制门户检测辅助功能，在 host 接入 NCM 链路时最大化操作系统
自动打开调试板 Web UI 的概率。

**DHCP**：NCM 接口上运行的 DHCPv4 server 会将路由器和 DNS 地址通告为
`172.29.203.1`，并发送 DHCP option 114（Captive Portal URI），其值为
`http://172.29.203.1/captive-portal/api`。这是一个面向兼容性的 HTTP 端点，
不是可信的 HTTPS 信号。

**DNS**：固件在 NCM 接口上运行一个轻量级 DNS 响应器，绑定 UDP port 53。
对任何传入查询均返回指向 `172.29.203.1` 的泛解析 A 记录；对 AAAA 查询返回
NOERROR 但 answers 为零（NODATA）。不提供递归、转发或缓存；只针对本 NCM
本地服务器查询的名称提供响应。DNS TTL 设为 30 秒，以便主机缓存相对快速过期。

**HTTP port 80**：单一 Zephyr HTTP 服务绑定 NCM 本地地址 `172.29.203.1`，
并在 port 80 上按 URL 路径路由：`/`、`/assets/*`、
`/api/v1/*`、`/api/v1/ws/*`、`/captive-portal/api` 和传统检测探针。当 GET 请求到达
`/captive-portal/api` 时，返回 HTTP 200 和 JSON 正文，`Content-Type` 为
`application/captive+json`，内容为
`{"captive":true,"user-portal-url":"http://172.29.203.1/","venue-info-url":"http://172.29.203.1/"}`。
对其他不匹配路由路径的 GET 路径则返回 HTTP 302，`Location` header 指向
`http://172.29.203.1/`。未知 API 路径返回 JSON 404。其他 HTTP 方法返回 HTTP 405。

当前 pinned Zephyr HTTP/1 server 会在应用 callback 之前处理 dynamic resource 的
`HEAD`，并返回默认的 headers-only HTTP 200。因此强制门户诊断和兼容性承诺以 `GET` 为准。

**自动打开概率**：大多数现代操作系统通过对已知检测名称进行 DNS 查询，
然后对返回地址发起 HTTP 请求来检查强制门户。DHCP option 114、泛解析 DNS A
记录和监听 port 80 的组合，意在最大化 OS 自动打开调试板 Web UI 的概率。
这是一种 best-effort 机制，不做保证。结果因操作系统版本、网络配置和主机是否
有其他活动网络接口而异；如果 host 在其他适配器上有优先默认路由，可能不会触发
检测流程。由于 NCM lease 会通告默认路由和 DNS server，连接调试板也可能改变
host 的路由或 DNS 优先级；多网卡环境必须确认原有互联网出口仍保持优先。需要
Web UI 时，用户始终可以直接在浏览器中打开
`http://172.29.203.1/`，或使用 `curl`、CLI/TUI。

## 硬件映射

### RP2350A 版本

| 功能 | 固件名称 | 原理图信号 | GPIO |
| --- | --- | --- | --- |
| 12 V 输出使能 | `12v_out` | `GP02_12V_EN` | 2 |
| 5 V 输出使能 | `5v_out` | `GP05_5V_EN` | 0 |
| 5 V WS / VDD_5V 常开电源轨 | `5v_ws` | `GP09_5V_WS_EN` | 1 |
| 20 V 输出使能 | `20v_out` | `GP10_20V_EN` | 3 |
| TF/SD 路由切换 | `switch sd` | `GP06_TF_SW` | 4 |
| USB hub mux 切换 | `switch usb` | `GP03_USB3_HUB` | 5 |
| CH347 1.8 V VIN 电源 | `switch vin` 内部控制 | `1V8_EN` | 6 |
| TF 写保护 | — | `TF_WP` | 22 |
| CH347 VIO 电压选择 | `switch vin` | `VIO_SEL` | 23 |
| 测试点 | — | `TP15` | 24 |
| 状态 LED | — | `LED_BLUE` | 25 |
| GPIO 别名 | `CON_MAS` | `CON_MAS` | 7 |
| GPIO 别名 | `CON_REST` | `CON_REST` | 8 |
| GPIO 别名 | `CON_USER` | `CON_USER` | 9 |
| J16 GPIO 范围 | `GP10`-`GP20` | — | 10-20 |
| J16 ADC3 / GPIO | `ADC3` / `GP29` | — | 29 (ADC3) |
| 5 V 电流监测 | `adc read 5v_out` | `S_C_5V` | 26 (ADC0) |
| 12 V 电流监测 | `adc read 12v_out` | `S_C_12V` | 27 (ADC1) |
| 20 V 电流监测 | `adc read 20v_out` | `S_C_20V` | 28 (ADC2) |

VIN 启动默认值为 3.3V。GPIO1 VDD_5V 及其 GPIO6 VDD_1V8 子电源轨在
Device Tree 模型中保持常开。可选 CH347 VIO 电平使用标准 `regulator-gpio`
regulator 建模，包含精确的 1.8V 和 3.3V states，固件通过 Zephyr regulator
API 选择电压。切换前请确认目标板支持所选电压。

电流监测通道使用 INA139、10 mOhm 采样电阻和 50 kOhm 输出负载。
MCU 同时上报原始 ADC 调试值，以及通过 Zephyr
`current-sense-amplifier` 标准接口得到的电流值；主机侧现在直接展示这些值，
不再做 ADC 校准表或零点修正。
传感器传输函数参考公开的
[TI INA139 规格书](https://www.ti.com/product/INA139)。

当前原理图副本放在：
- [doc/radxa-linkr-debugger-schematic-x1.1.pdf](doc/radxa-linkr-debugger-schematic-x1.1.pdf)

旧版 G2 (RP2040) 原理图 `doc/radxa-linkr-debugger-schematic.pdf` 仅作归档参考；
G2 硬件不受本固件支持。

## 开发

运行单元测试：

```sh
./apps/radxa_linkr_debugger/tests/run_unit_tests.sh
```

测试脚本覆盖：

- 共享板级模型的 host C 单元测试。

## 仓库结构

```text
apps/radxa_linkr_debugger/        Zephyr 应用
apps/radxa_linkr_debugger/src/    固件源码和共享板级模型
apps/radxa_linkr_debugger/tests/  单元测试
cmd-ng/                          面向高级用户和 Agent 的 Rust CLI/TUI
web/                             Web UI 和本地设备/串口桥接
doc/                          硬件文档、OpenOCD 配置和宣传素材
skills/radxa-linkr-debugger/      面向 Agent 的 skill 和操作规程
west.yml                      Zephyr workspace manifest
```
