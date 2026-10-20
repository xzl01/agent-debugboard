[English](webui.md)

# Web UI

Radxa Linkr Debugger 固件 HTTP 和 WebSocket API 的 React/Vite 控制界面。
生产固件会将 gzip 压缩的 UI 嵌入 flash，通过 USB NCM 在
`http://172.29.203.1/` 提供服务。页面使用同源 `/api/v1` HTTP 和
WebSocket 端点，正常板级控制不需要主机代理。

## 访问

连接 USB NCM 接口后打开：

```text
http://172.29.203.1/
```

## 强制门户发现

固件通过 DHCP option 114 和 HTTP 检测端点辅助操作系统打开调试板 Web UI，
同时避免把调试板配置成 host 的互联网网关。

- **DHCP**：NCM 接口上的 DHCPv4 server 只分配本地地址和子网路由，明确不
  通告默认网关或 DNS server。它仍发送 DHCP option 114（Captive Portal URI），
  值为 `http://172.29.203.1/captive-portal/api`，因此不会抢占以太网、Wi-Fi
  或 VPN 的互联网访问路径。
- **HTTP port 80**：绑定在 `172.29.203.1:80` 的单一 Zephyr HTTP 服务按
  URL 路径路由。GET 请求到达 `/captive-portal/api` 时返回 HTTP 200 和
  JSON 正文：
  `{"captive":true,"user-portal-url":"http://172.29.203.1/","venue-info-url":"http://172.29.203.1/"}`。
  其他不匹配路由的 GET 路径返回 HTTP 302，`Location` 指向
  `http://172.29.203.1/`。
- **自动打开为 best-effort，不保证。** 结果因操作系统版本、网络配置和
  主机是否有其他活动网络接口而异。需要 Web UI 时，用户始终可以在浏览器中
  直接打开 `http://172.29.203.1/`，或使用 `curl`、CLI/TUI。

## 仪表盘概览

主仪表盘提供以下卡片：

- **电源控制** — 开关 `12v_out`、`5v_out`、`20v_out`。
- **ADC 监测** — 各电源轨的实时电流读数。
- **Switch 路由** — TF/SD 在 `target` 和 `usb-reader` 之间切换；J12 下层插入的
  USB 设备可在主机（`pc`）和 J12 上层所接目标板（`target`）之间切换。
- **GPIO** — safe/control GPIO `GP7`-`GP9`、`GP12`-`GP16`，以及仅输入的
  `GP29`/ADC3。

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

## 内置任务与已保存任务

在 **Automation** 工作区中，MASKROM 与 EDL 以六个固件内置任务行出现，
来自 `GET /api/v1/tasks/catalog`，覆盖两种模式与 `5v_out`、`12v_out`、
`20v_out` 的所有组合。Web UI 自身不保存任何恢复配方：rail、GPIO 电平
与等待时间全部来自经过严格校验的固件 catalog。built-in 与 stored task
共用通用 runner，只派发普通 power/GPIO 请求，不写入固件 flash，也不占用
四个 stored task 槽。**Task** 卡片可以：

- 把 catalog 中的 built-in 与 `GET /api/v1/tasks` 返回的 stored task 合并显示。
  两个请求相互独立：catalog 不可用或校验失败时，built-in 行消失并显示
  错误提示，而 stored task 仍可正常使用。
- 直接执行 built-in，不读取或修改 `/api/v1/tasks`。
- 把当前编辑器工作流展开并校验为仅含固件可执行原语步骤后再保存。
- 执行已存储任务：页面下载当前的 `linkr-task.v1` blob，按 id 选中任务，
  再按顺序通过普通的 power、GPIO、switch 端点派发每条记录。`wait_ms`
  在每次成功请求之后由**客户端** sleep；首次失败即停止，并把失败
  记录展示在界面上。发给板子的 wire `body` 永远不会携带 `wait_ms`。
- 清除全部已存储任务。当前硬件状态保持不变。

如果 stored task 使用保留的 built-in ID，对应 built-in 行会标记该 stored
entry 已被遮蔽；执行该 ID 时选择 built-in。清空 stored task 会删除碰撞条目，
但 catalog 中的 built-in 仍保留显示。

固件**不会**在开机时自动运行、重放或预演已存储任务。重启之后仍存在
于 flash 中的任务保持静默，直到 **Task 卡片**显式运行它。旧开发期
任务数据由新标记 `# linkr-task.v1` 与新 key `linkr/task/tasks`
定向失效；既没有迁移路径，也不提供读取别名。

## 固件工具与电源分析

- **固件工具**位于仪表盘侧栏，包含 OTA、正常重启和 ROM BOOTSEL 控件。
  完整 OTA 工作流参见 [OTA 固件更新](ota.zh-CN.md)。
- **电源分析**是工作区标签页，包含普通捕获和启动分析。
- **Automation** 是工作区标签页，包含测试编辑器以及用于列出、保存、
  执行和清除已存请求序列的 flash 持久化 Task 卡片。

### 重启与 BOOTSEL

**Restart**（正常重启）通过 `POST /api/v1/reboot` 请求正常冷重启。板子在响应
后 250 ms 重启，页面随后重连；该动作绝不请求 ROM BOOTSEL，也不会出现
`RPI-RP2` 设备。如果复位期间一直按住物理 BOOTSEL 按键，则由 ROM 单独决定
进入 BOOTSEL。未被已保存 v1 配置快照恢复的电源轨回到各自的 boot-off 默认
状态，`vdd_5v` 跟随 USB 路由默认值；板子恢复后必须重新设置并验证电源轨与
PD。

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
DTR/RTS、USB、队列和 pause/fault 状态；overrun 同时包含本次 drop 增量和累计值。

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
