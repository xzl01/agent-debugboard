# cmd-ng

`cmd-ng/` 是当前主力开发的 Rust 主机侧 CLI/TUI 实现。

普通用户应优先使用 Web UI。已发布的 `radxa-linkr-debuggerctl` 面向高级用户、Agent、
自动化和 HIL 验证；本文件只描述如何开发、构建和直接运行 `cmd-ng` 源码树。

主机侧 CLI/TUI 统一由 Rust 实现，并保留“无参数进入 TUI、有参数进入传统 CLI”的入口契约。
release 归档、skill 安装器和 Nix 包额外提供 `rdb` 链接；它指向同一个
`radxa-linkr-debuggerctl` 可执行文件，不是第二套 Cargo binary target。

## 当前范围

- CLI：`status`、`doctor`、`power list|get|set`、`switch list|get|route`、`adc read`、`adc record`、`gpio list|set|input`、`watchdog status`、`bootloader`、`ota status|upload|test|confirm`
- 输出：支持 `--json`，并校验 `radxa-linkr-debugger.v1` envelope
- TUI：无参数启动，为高级用户提供终端交互入口
- OTA（仅 RP2350）：`ota status` 查看状态，`ota upload PATH` 上传 MCUboot 格式应用 bin，`ota test` 请求测试启动，`ota confirm` 立即确认运行镜像；SHA256 仅校验完整性，无签名/认证/安全启动/防回滚；固件设计为在未确认镜像 watchdog 复位后请求 MCUboot 回滚，但该恢复路径的故障注入 HIL 尚未完成
- VIN：`switch get vin` 和带确认保护的 route 控制（RP2350）

## 构建与运行

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- --help
cargo run --manifest-path cmd-ng/Cargo.toml -- --json status
cargo run --manifest-path cmd-ng/Cargo.toml --
```

## 说明

- 默认设备 URL 仍是 `http://172.29.203.1`
- `--json` 仍要求固件返回 `schema/ok/command`
- TUI 现在以 HTTP 轮询作为主数据通道，因此可以稳定多开；实时高频采集改由 `adc record` 走 websocket
- TUI 使用无常态边框的高密度布局：顶部两行状态与按需出现的七行三通道电流示波器，其下固定显示 Controls、Saved Config、Status 三页页签和当前页表头；示波器按各通道可见峰值自适应量程，无数据时高度为零，底部是不会截断半个操作的 htop 式键位块。示波器相邻活动通道之间固定放一个未着色空格 cell（gutter），四种规范宽度 W=47/48/80/120 的内容列宽与零号 gutter 列在 [cmd-ng/DESIGN.md](DESIGN.md) 第 7 节定义；终端太窄时 gutter 收起、通道平铺
- Tab/Shift+Tab 前后切页，也可左键点击 Controls、Saved Config、Status 页签的可见标签范围切页；活动页签、页签间的两个空格、非左键 Down 以及确认框/错误状态下的页签输入均为惰性。Saved Config 页每个可见条目行独占一个铺满整行的命中矩形，左键 Down 落在条目上会按权威 items 重新解析 ID、切焦点、切光标、切换本地选择恰好一次；其他鼠标事件以及落在已不在权威 items 中的旧 ID 行都为惰性。Saved Config 确认框打开时，只有它自己的 `[ Confirm ]` 与 `[ Cancel ]` 接受左键 Down，模态下的其它命中全部惰性。上下键按可见行移动 Controls/Saved Config 焦点，GPIO 双列行会保持左右位置，左右键只在同一物理行的兄弟 GPIO cell 间切换；Status 页方向键直接滚动。Power/Switch 的整行和每个 GPIO 半行 cell 都有独立鼠标命中区域
- TUI 的 Power 行完全使用最新固件状态中的 `power_outputs` 目录并保持固件顺序；host 不维护 rail 常量，新增/移除/重排输出都会随下一次状态刷新反映，`5V_FIN` 作为输入电源不会成为可控行
- TUI GPIO 排列只使用固件返回的 `layoutGroup/layoutLabel/layoutRow/layoutColumn`，不会在 host 端硬编码连接器或 pin map，也不会按 `note` 隐藏任何 GPIO；包括 `CON_MAS` 在内的所有固件返回项都会显示。正常宽度下同一固件物理行最多显示两个独立 cell，窄于 48 列时每针单行，缺失 metadata 的引脚按固件快照顺序回退显示
- TUI GPIO 操作是直发语义：`l`/`L` 驱动 LOW，`o`/`O` 驱动 HIGH，`i`/`I` 恢复输入；Enter、Space、`0`、`1` 对 GPIO 均为惰性。直接键解码只接受小写 `l`/`o`/`i` 且无任何修饰键，或大写 `L`/`O`/`I` 且修饰键恰好为 `Shift`；其它修饰键组合（Ctrl、Alt、Super、或小写配 Shift、大写无 Shift 等失配形式）返回 `None`，不会取消既有手势、也不会派发任何硬件请求。鼠标左键在 600ms 前释放后进入 220ms 双击等待窗，窗口到期且没有第二次按下时才驱动 LOW；按住到 600ms 一次性驱动 HIGH，同一引脚在等待窗到期前第二次按下/释放恢复输入且不会产生瞬态 LOW；中键、右键始终惰性。终端 `Resize` 是 TUI 强制重绘边界：Resize 到来时取消任何进行中的 GPIO 手势（包括初始左键 Down 与 AwaitSecond 状态），整体清空 `hit_map` 使旧几何失效并立即重绘，ready-event 排空也在 Resize 处停下，Resize 之后排队的鼠标事件按新几何处理、不会落在旧 cell 上。cell 始终显示最近一次权威快照的 `◌ IN LOW`、`◌ IN HIGH`、`○ OUT LOW`、`● OUT HIGH`（LOW 深灰、HIGH 红色加粗），不做乐观更新；在途动作在状态后缀后追加黄色加粗的 `[LOW…]`/`[HIGH…]`/`[INPUT…]` 标记，按住中的引脚追加 `[HOLD…]`（await-second 无标记；同一 cell 至多一个标记，在途动作标记优先），选中态下标记在白底选区中保持可见并加粗，宽度不足时先整体丢弃标记；当 cell 宽度足够时状态后缀完整，更窄的 cell 允许对后缀做整体 hard-clip，在途动作同时由状态行 `gpio <name>=…` 兜底；选中 GPIO cell 时 keybar 切换为 GPIO 专用键位（`l LOW`、`o HIGH`、`i INPUT`、`Mouse click/hold/2x`，47 列内完整放下这四段），其余键位按剩余宽度整段取舍；所有行按 `unicode-width` 显示列裁剪，CJK 字形不会被截半或折行；电源和固件广告的每个 switch 第一次操作都只打开红框确认，Controls 的 MODE 因而统一显示 `confirm`。TUI 把这套三秒确认门控无差别套到所有固件广告的 switch 上，不看固件 `requires_confirm`；策略本身不硬编码任何生产 switch 名称、路由或 pin。点击 switch 时其整行焦点会先更新并保留在确认框下，未被模态遮住的 cell 仍保留合成 `accent.select` 背景，且此时不会发送 PUT；硬件确认事件仅在严格早于 3 秒（`now < started + CONFIRM_TIMEOUT`）按下才执行一次。达到或超过 3 秒的确认按键按超时处理，发出该命令的 `timeout_message`，不依赖渲染/poll tick 是否已观察到超时；取消或超时均不发请求。固件 `requires_confirm` 数据仍供 Saved Config、API 与 CLI 使用；确认框是常态界面之外唯一带边框的区域
- GPIO 鼠标手势以终端 cell 为容差单位：同一 cell 内的 Moved 或左键 Drag 报告保持手势有效；只有 pin、终端列或终端行变化而跨入另一 cell 才会取消。到达 600ms 的 Up 立即按原始 pin 派发 HIGH；await-second 到达 220ms 的第二次 Down 先派发原始 pin 的 LOW 并消费该次 Down，下一次新的 Down 才开始新手势。
- 除 TUI 对所有 switch 统一增加现有三秒确认门控外，本次改动不改变 HTTP 端点、两秒轮询周期、硬件默认值或固件行为
- MASKROM/EDL 只作为固件目录提供的普通自动化任务出现；使用 `task list` 查看并通过 `task run builtin/...` 执行，TUI 不提供专用入口；host 不持有任何恢复配方
- GPIO 在 CLI/TUI 中会同时显示 `GPxx` 和 `note`；控制时可使用 `GPxx`、数字引脚（如 `13`）或精确 note（如 `CON_MAS`）。`GP29` 仍在持久化/安全目录中，但由 ADC3 占用时是 input-only；输出请求会被固件拒绝。参见[固件说明](../apps/radxa_linkr_debugger/README.md)与[权威 ADC telemetry contract](../docs/reference/adc-telemetry.md#gp29-ownership)。
- `adc read` 默认返回四个 descriptor：`5v_out`、`12v_out`、`20v_out`（电流）和 `adc3`（GP29 电压）。HTTP rich path 中三路电流的 `current_ua` 是 signed integer µA，ADC3 从 `sensor_value` 得到 signed integer µV；`adc read -v` 还显示 `signal`、`raw`、`mv`、`sensor_value` 等诊断字段，host 不做 ADC 校准或零点修正。
- 实时 WebSocket 使用紧凑形状：单样本 reading 包含 `name`、`signal`、`kind`、`unit`、`value`，其中 `value` 是由 `unit` 指定的 signed integer（电流 `uA`、电压 `uV`），且只有电流通道携带 `power_enabled`。Batch 先声明 `channels[]`，每个 `values: i32[]` 按位置与 `channels` 对齐；`power_enabled_mask` 是 unsigned 8-bit 掩码，仅对 `kind="current"` 的通道有意义。WS 不携带 HTTP 的 `raw`、`mv`、`current_ua` 或 `sensor_value`；完整 wire shape 见[权威 ADC telemetry contract](../docs/reference/adc-telemetry.md)。
- `adc record OUTPUT_PATH [MAX_SAMPLES] [--rate-hz HZ]` 会创建 live websocket session；`.ndjson` 输出完整 telemetry 记录，`.csv` 输出设备时间戳和三路电流列；默认请求 1000Hz，`--rate-hz` 可指定 1..1000Hz，高于 100Hz 时 CLI 请求 batch JSON 并逐样本展开；正常完成或连接、解析、写盘失败后都会关闭并删除自身 session
- 固件最多支持四个并发 websocket 客户端，多个 `adc record` 可以同时运行；触发式功耗采集使用全局硬件缓冲区，同一时间只能有一个 capture owner
- recorder 会写入主机接收时间和 `metadata.requested_rate_hz`，并把设备 `sample_sequence`、`uptime_us` 和 `device_t_mono_us` 放入 `metadata.device_timing`；紧凑 batch 仅提供 `sequence` 与 `uptime_us` 时，Rust 会将其归一化为对应别名，也会接受固件显式提供的别名；CSV 时间列优先使用 `device_t_mono_us`，否则回退到 `uptime_us`，再否则为 0；采样环覆盖通过 `metadata.dropped_samples` 显式报告，分析采样间隔时应优先使用设备时间
- `raw` 模式在 HTTP 路径下不支持
- `watchdog` 仍只暴露 `status`，不提供 host 侧 feed/控制
- 板内 `vdd_5v`（VDD_5V）电源轨随 `switch usb` 路由联动：切 `pc` 开、切 `target` 关；路由不变时仍可手动 `power set vdd_5v`，下次路由切换会重新强制。开机默认路由 `target` 下保持关闭；关闭它会切断 CH347 1.8V VIN 的 VDD_1V8 子电源轨
- VIN 切换需要 `--confirm`（TUI 中为 Space/Enter 确认），因为电压切换有副作用；RP2350 的 GPIO1 VDD_5V 和 GPIO6 VDD_1V8 由固件 Device Tree 建模为常开，可选 CH347 VIO 电平由固件标准 `regulator-gpio` 节点建模并通过 Zephyr regulator API 切换。执行 1.8V 切换前必须确认目标支持该电平、连接 VIO 物理测量设备，并明确接受硬件副作用；默认验证只读取或保持 3.3V
- 安装统一桌面栈后，`-d` / `--desktop` 会启动单个 `linkr-tray` 后台守护并立即退出；Tray 与 Host 服务运行在同一进程，普通 CLI/TUI 启动也会自动确保它已运行。无图形桌面或 GTK 已加载但 AppIndicator 实现不可用时，同一二进制会以 `--headless` 模式常驻；之后恢复完整图形环境再运行 CLI 或 `-d`，headless 守护会优雅交接为单个图形 Tray 守护。Linux 的 `linkr-tray` 本身仍链接 GTK；完全未安装 GTK 的最小系统应直接运行 `linkr-host serve`。普通 CLI/TUI 在桌面组件缺失时会继续工作，只有显式 `-d` 会把启动失败作为错误。仅在明确不需要尝试任何后台 Host 时设置 `LINKR_SKIP_DESKTOP=1`。

## Released 与 nightly 通道

- 正式 release 由 GitHub `Release` workflow 在推送 `v*` tag 后产出，资产、签名策略与现有 release 一致。
- 一条独立的滚动 nightly workflow `.github/workflows/nightly.yml` 仅在每次推送到 `dev` 分支时把可变 `nightly` Git tag 与标记为 `not latest` 的 prerelease 一同发布，覆盖固定 9 个资产（4 个固件产物、Linux AMD64 / macOS ARM64 / Windows AMD64 三份 CLI 归档、skill bundle 与 `SHA256SUMS.txt`），并在每次 run 中清理先前多余资产。nightly 是滚动且仅用于测试，不会替换正式 `v*` release；正式 release 仍多出 Linux ARM64 CLI 归档这一份、共 10 个资产，是生产交付的唯一权威来源。
