[English](webui.md)

# Web UI

Radxa Linkr Debugger 固件 HTTP 和 WebSocket API 的 React/Vite 控制界面。
生产固件会将 gzip 压缩的 UI 嵌入 flash，通过 USB NCM 在
主机 NCM `/30` 中板卡 `.1` 地址（例如 `http://10.20.30.1/`）提供服务。
页面使用同源 `/api/v1` HTTP 和 WebSocket 端点，正常板级控制不需要主机代理。

## 访问

连接 USB NCM 接口后先读取板卡 URL，再打开：

```sh
ip -brief addr show
# 主机 NCM 是板卡 /30 的 .2，板卡是 .1
# 或在板端 CDC shell：
ip
```

## 强制门户发现

固件通过 DHCP option 114 和 HTTP 检测端点辅助操作系统打开调试板 Web UI，
同时避免把调试板配置成 host 的互联网网关。

- **DHCP**：NCM 接口上的 DHCPv4 server 从板卡持久化的私有 `/30` 分配
  `network + 2`，明确不通告默认网关或 DNS server。它仍发送 DHCP option 114
  （Captive Portal URI），URI 在运行时由当前 server 地址和 `/generate_204`
  路径生成，因此不会抢占以太网、Wi-Fi 或 VPN 的互联网访问路径。
- **HTTP port 80**：单一 Zephyr HTTP 服务监听 80 端口；主机经 NCM 在板卡 `.1` 地址访问。GET 请求到达
  `/captive-portal/api` 或 `/generate_204` 时返回 HTTP 200 和 capport JSON
  正文（使用当前板卡 URL）。其他不匹配路由的 GET 路径返回 HTTP 302，
  `Location` 指向当前板卡 URL。
- **自动打开为 best-effort，不保证。** 结果因操作系统版本、网络配置和
  主机是否有其他活动网络接口而异。需要 Web UI 时，用户始终可以在浏览器中
  直接打开输出的 URL，或使用 `curl`、CLI/TUI。

## 仪表盘概览

主仪表盘提供以下卡片：

- **电源控制** — 开关 `12v_out`、`5v_out`、`20v_out`、`vdd_5v`、
  `5v_usb_hub` 和 `5v_otg`。hub 与 OTG 端口是 `vdd_5v` 的子轨：打开子轨会同时
  打开父轨，仅打开父轨不会打开子轨。
- **ADC 监测** — 四路 INA226 电源轨的实时电流读数；hub 与 OTG 支路没有独立测量。
  ADC0 作为 PD 输入的实测 DC20V 源侧电压显示，ADC1-3 是 native pad 电压卡片。
- **Switch 路由** — TF/SD 在 `target` 和 `usb-reader` 之间切换，并提供 `tf_wp`
  写保护。x1.2 没有 USB route 控件。
- **GPIO** — safe/control GPIO `GP5`-`GP9`、`GP12`-`GP16`，以及仅输入的
  `GP29`/ADC3。GP5/GP6 是普通 J8 GPIO，不是 switch。

电源与测量卡片把 ADC0 合并到 PD 设置面板：显示值是固件按 Device Tree 还原的
DC20V 源侧值，`raw`/`mv` 作为 pad 诊断值提供。ADC1、ADC2、ADC3 作为独立
native 电压卡片显示在 switch 路由上方，使用 0..3.3 V 量程。Web UI 绝不执行
`115/15` 或主机侧校准。ADC0 未校准精度约 2%，界面不声称更高精度。

在 **Configuration > Saved** 标签页内，**已保存配置**卡片始终可见。配置行和操作
显示在标题栏下方；电源和 Switch 分组默认展开，GPIO 分组默认折叠。分组折叠只改变
本地显示，不会触发保存、清除或刷新；展开或折叠分组时，当前选择和进行中的反馈仍会保留。

电源与电流卡片包含触发式功率分析仪。支持手动、电流阈值、GPIO 边沿和
上电触发，保留四次捕获用于叠加对比，可导出 CSV 或 NDJSON。捕获使用
固件设备时间戳；触发前缓冲和触发后持久化均由浏览器完成。

## Terminal 工作区

Terminal 工作区与主仪表盘并列，包含：

- **逻辑分析仪** — RP2350 PIO2+DMA 高速单次捕获。支持安全引脚列表中的
  1-16 个通道（GP7-GP9、GP10-GP20、GP29），触发模式 `none`、`rising`、
  `falling`、`either`，请求采样率 1-125 MHz，最多导出 512 个样本。
  完成的捕获可在波形视图中预览，并导出为 CSV 或 PulseView `.sr` 文件。
  还提供连续流式模式（1-25 MHz）。浏览器内 Rust/WASM 解码器支持
  UART、I2C 和 SPI 协议解码。
- **J4 串口终端** — 使用 xterm.js 显示一个逻辑目标控制台。页面创建固件
  live session 并订阅 `serial.j4`，不经过浏览器设备选择器、主机串口 broker
  或本机 tty。进入工作区时自动连接；手动断开后在当前页面 session 内保持
  sticky disconnected，不自动重试，用户显式点击 reconnect 后才恢复。

## 工作区标签页

右侧工作区提供并排标签页：**Web 串口终端**、**电源分析**、**逻辑分析仪**、
**自动化编排** 与 **配置维护**。GPIO 控件位于 **硬件控制 → I/O**，保持唯一的
硬件控制入口。引脚
圆点的颜色本身实时反映每一个最新电平：黑色为低电平、红色为高电平，输入态
引脚同样如此。外层虚线表示输入，实线表示输出；较小的电平色圆点始终位于该
方向边界内。引脚通过直接手势驱动，无需先选中：短按在 220 ms 双击窗口结束后
请求输出低电平；窗口内再次短按改为请求设为输入；按住 600 ms 请求输出高电平，
按住期间危险色进度直接沿同一条虚/实方向环扫过。移动超过 8 px、指针捕获丢失、
`pointercancel` 或按 Escape 都会取消手势且不写入任何内容。键盘操作在聚焦的
引脚上立即生效：Enter/Space/0 输出低电平，1 输出高电平，I 设为输入；按键
自动重复会被忽略。卡片同一时间只允许一个进行中的请求，等待中的引脚会灰显
并叠加一个实心警示色忙碌环。
挂载或刷新引脚图不会写入 GPIO 状态，界面不做乐观更新，请求期间仍以最新固件
快照作为显示权威。
maskrom/EDL 恢复线 `CON_MAS`（`GP7`）是普通白名单输出，可通过这些手势驱动。
`GP29` 在由 `adc3` 电压监测占用时保持输入态；不支持的输出请求会在固件层失败，
卡片会显示该错误。

硬件控制浮窗会把上次选择的 **电源** 或 **I/O** 分区保存在浏览器缓存中。
点击浮窗外侧背景会自动收起浮窗；点击浮窗内部不会收起。

## Workflow 库、任务与导入导出

### Workflow 库

**Automation** 工作区把多个 workflow 保存在浏览器 IndexedDB 库中。一个 workflow
可以部署到多个板子；deployment 以 board store UUID 加 task ID 为键。
**Save to library** 更新当前 workflow，**Save as new workflow** 创建独立条目，
**Open** 打开已保存条目，删除只移除该库条目。未保存的 editor draft 与库分离，
并通过本地 draft 存储在刷新后保留；打开另一个 workflow 前会提示未保存变更。

该库只在当前浏览器本地。需要可移植副本时必须导出 workflow。Workflow
导入/导出使用 canonical `linkr-test.v1` source，保留 loops、Units 和 assertions。
Workflow 导入会先预览并校验文件或粘贴的 source，再替换 editor draft；不会运行
板子。Task IR 导入/导出是 **Task** 卡片中的独立动作；Task IR 是 deployment
requests，不是可编辑 workflow。

### 内置任务与 task-v2 任务

MASKROM 与 EDL 是 `GET /api/v1/tasks/catalog` 的六条不可变固件目录条目，
覆盖两种模式与 `5v_out`/`12v_out`/`20v_out`。它们不写入 flash，也不占四个
task-v2 槽。**Task** 卡片把它们与 `GET /api/v1/tasks` 的 task-v2 metadata
合并显示。Catalog 与 metadata 拉取相互独立：catalog 失败时 built-in 行隐藏并
显示错误，stored task 仍可用。

Task-v2 store 有四个 slot、独立 UUID/revision，并在板上保存 exact
`linkr-test.v1` source 加 `linkr-task.v1` IR（各最多 4000 字节）。卡片可以：

- 把当前 workflow 保存为 board task；只有 source <=4000 字节时才把 exact
  source 写入板子。
- 打开 stored task 的 workflow source。board-backed source（`present`）可在
  重启后由新浏览器 profile 从板子重新打开。超过 4000 字节的 source 只保存在
  本地 workflow library（`local-only`），会明确标记，并只部署 IR；除非导出再
  导入，新浏览器无法重新打开。IR-only task 或 built-in 没有无损 source，界面
  显示该限制，不提供假的 Open 动作。
- 从 exact `/api/v1/tasks/ir` 运行 stored task，并通过普通 power、GPIO、switch
  端点逐条派发。`wait_ms` 只在客户端；首次失败即停止。
- 导入一个 task IR 而不改兄弟任务，或显式用 full-set IR 替换全部任务。导入
  永不运行硬件。Full IR 导入只对 task-IR digest 未变的任务保留 source。
- 导出一个 task IR 或 full IR set。导出 built-in 只复制其主要请求，不包含
  catalog cleanup，因此不是 built-in 的无损副本。
- 原子删除一个 stored task 或清空全部任务。Clear 保持 store UUID，不改变实时
  硬件。

如果 stored task 使用保留的 built-in ID，对应 built-in 行会标记 stored entry
已被遮蔽；执行该 ID 时选择 built-in。Task 写入只有 JSON/base64 mutation；不存在
blob upload 或 per-ID write/delete 路由。

固件不会在 boot 时自动运行、重放或播种任务。Stored task 保持静默，直到显式
运行。旧 `linkr/task/tasks` blob 不迁移、不读取。

## 固件工具与电源分析

- **固件工具**位于仪表盘侧栏，包含 OTA、正常重启和 ROM BOOTSEL 控件。
  完整 OTA 工作流参见 [OTA 固件更新](ota.zh-CN.md)。
- **电源分析**是工作区标签页，包含普通捕获和启动分析。
- **Automation** 是工作区标签页，包含 workflow 库、测试编辑器，以及用于列出、
  导入、导出、执行、删除和清空 board task 的 task-v2 Task 卡片。

### 重启与 BOOTSEL

**Restart**（正常重启）通过 `POST /api/v1/reboot` 请求正常冷重启。板子在响应
后 250 ms 重启，页面随后重连；该动作绝不请求 ROM BOOTSEL，也不会出现
`RPI-RP2` 设备。如果复位期间一直按住物理 BOOTSEL 按键，则由 ROM 单独决定
进入 BOOTSEL。未被已保存 v1 配置快照恢复的电源轨回到各自未使能的启动默认
状态；`vdd_5v` 是父级电源项，`5v_usb_hub` 与 `5v_otg` 是它的子轨且同样启动
关闭。板子恢复
后必须重新设置并验证电源轨与 PD。

**Enter BOOTSEL** 通过 `POST /api/v1/bootloader` 执行，是独立动作，不是
Restart 的别名。它是 Web UI 中唯一应让板子枚举为 `RPI-RP2` 的动作。ROM
BOOTSEL 只能刷写合成完整 `radxa-linkr-debugger-rp2350.uf2`（MCUboot + 应用）；
仅含应用固件的 `zephyr.uf2` 不包含 MCUboot，绝不能使用。

物理 BOOTSEL 按键不是 Web UI 的第三个动作。固件运行中长按 3 秒后释放才请求
正常重启；短按不动作；一直按住不释放不会复位；复位或上电时按住则由 ROM
采样并进入 ROM BOOTSEL。按键由固件线程处理，无法恢复完全卡死的核心；该
故障模式下硬件 watchdog 仍是兜底路径。

## 连接目标串口

打开板载页面并进入 **Web 串口终端**。xterm.js J4 终端使用与仪表盘相同的
USB-NCM 地址：

1. 页面创建 `/api/v1/live-sessions` slot；
2. 连接返回的 `/api/v1/ws/<slot>`；
3. 订阅 `topic:"serial.j4"` 并显示原始 binary 下行。

每个浏览器 binary frame 是一条原子 J4 写，最大 480 字节。页面区分
`serial_write_accepted` 与之后的 `serial_write_complete`；accepted 不代表 UART
最后一个 stop bit 已上 wire。`serial_state` 上报 line coding、generation、
DTR/RTS、USB、队列、pause/fault 状态，以及自动终端 responder。显式输入仍是
multiwriter。只有一个 client 回复自动终端查询：DTR 有效时为 CDC，否则为编号
最小的已连接 WebSocket。其它页面显示 **Auto-reply observer** 和
**Manual input allowed**，仍可发送显式键盘、IME、粘贴和 Ctrl-C 输入，不是只读；
下行 escape 序列也不会被过滤。overrun 同时包含本次 drop 增量和累计值。
responder 切换保持同一套 xterm modes：promotion 前的输出先经过 parser fence
排空，之后才启用自动回复；generation 变化会在后续字节前重建 renderer。被动页面
的旧半截查询不会在之后被回复。

UART settings 接受 4800 到 115200 的 custom 整数 baud，并提供五个推荐 preset。
高于 115200 的 rate 会被拒绝；早期 high-rate 样本只是诊断证据。Apply 后以
`GET /api/v1/serial/j4` 的 current settings 与 generation 为权威；CDC
SET_LINE_CODING 无法返回稍后的 application-level rejection，因此也必须 GET
确认。低 baud 写 timeout 按有界八槽队列推导，并增加 2 秒余量。

页面显示的 `dtr` 是已发布的 CDC/gate 状态，不是内部 promotion phase。DTR=false
时，USB default line coding 只会 stage，不会覆盖 active tuple 或 generation；
此低态下的 HTTP/WS Apply 立即生效。终端 assert DTR 时，板子先应用 staged USB
settings，之后才 publish `dtr=true` 并开放 ingress。apply/attach/detach 失败会
保持 CDC blocked，直到成功的 LOW/reset。API 不公开 phase 或 block-reason 字段。

逻辑分析仪占用 GP10/GP11 时，终端连接保持存在，J4 进入 PAUSED 并明确拒绝输入；
采集 cleanup 恢复 UART1 和 fresh sink epoch 后，同一终端无需刷新即可继续。手动
disconnect 后不自动重试。

非浏览器访问可连接板卡当前 NCM 地址的 raw TCP 5555，或使用 J4 CDC ACM
interface。raw TCP 最多两个客户端，是纯二进制流，不是 Telnet、SSH 或 noVNC。
主机 tty 编号不稳定，必须按 interface 身份识别 J4，不能假定固定设备号。

## 启动功率分析

启动工作流位于 **电源分析**工作区标签页。需要已连接的 J4 session
以及空闲的功率捕获会话。经一次明确确认后：

1. 清除并录制目标串口控制台。
2. 关闭所选电源轨并等待配置的放电延迟。
3. 在恢复供电前 arm 固件 `power_on` 捕获。
4. 从返回的串口数据中标记首个上电后 UART 字节、U-Boot 或 UEFI、内核
   和登录标记。
5. 报告峰值电流、平均功率、积分能量和最近两条触发对齐的功率曲线。

启动固件选择器默认自动检测，可固定为 U-Boot 或 UEFI。任务可在上电前
取消。完成后可下载串口日志。

## 开发预览

支持的硬件 UI 是烧入固件并由板卡 port 80 提供的 production bundle。port 5173
上的 Vite 服务只用于开发预览，不是设备传输或 J4 broker。GitHub Pages 可用于
静态外观评审；当前 J4 产品验收使用板载页面及其直连固件 HTTP/WebSocket 路径。

## 相关文档

- [OTA 固件更新](ota.zh-CN.md)
- [OpenOCD / JTAG](openocd.zh-CN.md)
