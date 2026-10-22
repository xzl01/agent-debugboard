# cmd-ng

`cmd-ng/` 是当前主力开发的 Rust 主机侧 CLI/TUI 实现。

普通用户应优先使用 Web UI。已发布的 `radxa-linkr-debuggerctl` 面向高级用户、Agent、
自动化和 HIL 验证；本文件只描述如何开发、构建和直接运行 `cmd-ng` 源码树。

主机侧 CLI/TUI 统一由 Rust 实现，并保留“无参数进入 TUI、有参数进入传统 CLI”的入口契约。
release 归档、skill 安装器和 Nix 包额外提供 `rdb` 链接；它指向同一个
`radxa-linkr-debuggerctl` 可执行文件，不是第二套 Cargo binary target。

## 当前范围

- CLI：`status`、`doctor`、`power list|get|set`、`switch list|get|route`、`adc read`、`adc record`、`gpio list|set|input`、`task list|store|delete|clear|import|export|source|run`、`watchdog status`、`reboot`、`bootloader`、`ota status|upload|test|confirm`
- 输出：支持 `--json`，并校验 `radxa-linkr-debugger.v1` envelope
- TUI：无参数启动，为高级用户提供终端交互入口
- OTA（仅 RP2350）：`ota status` 查看状态，`ota upload PATH` 上传 MCUboot 格式应用 bin，`ota test` 请求测试启动，`ota confirm` 立即确认运行镜像；SHA256 仅校验完整性，无签名/认证/安全启动/防回滚；固件设计为在未确认镜像 watchdog 复位后请求 MCUboot 回滚，但该恢复路径的故障注入 HIL 尚未完成
- x1.2 switch：固件当前只广告 `sd` 与 `tf_wp`；CLI switch 命令保持目录驱动

## 构建与运行

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- --help
cargo run --manifest-path cmd-ng/Cargo.toml -- --json status
cargo run --manifest-path cmd-ng/Cargo.toml --
```

## 说明

- 默认设备 URL 仍保留 `http://172.29.203.1` 作为旧镜像回退；新固件请用 `--url http://<板卡 IP>` 或 `LINKR_BOARD_URL=http://<板卡 IP>`（主机 NCM /30 的 .2 地址减一）
- `--json` 仍要求固件返回 `schema/ok/command`
- TUI 现在以 HTTP 轮询作为主数据通道，因此可以稳定多开；实时高频采集改由 `adc record` 走 websocket
- TUI 使用无常态边框的高密度布局：顶部两行状态、按需出现的七行电流示波器与一行 ADC 电压 strip（通道由固件快照的 `adc_channels` 目录驱动，电流包含 `vdd_5v`），其下固定显示 Controls、Saved Config、Status 三页页签和当前页表头；示波器按各通道可见峰值自适应量程，无数据时高度为零，底部是不会截断半个操作的 htop 式键位块。示波器相邻活动通道之间固定放一个未着色空格 cell（gutter），内容分栏规则及三通道合成测试的 W=47/48/80/120 示例在 [cmd-ng/DESIGN.md](DESIGN.md) 第 7 节定义；终端太窄时 gutter 收起、通道平铺
- Tab/Shift+Tab 前后切页，也可左键点击 Controls、Saved Config、Status 页签的可见标签范围切页；活动页签、页签间的两个空格、非左键 Down 以及确认框/错误状态下的页签输入均为惰性。Saved Config 页每个可见条目行独占一个铺满整行的命中矩形，左键 Down 落在条目上会按权威 items 重新解析 ID、切焦点、切光标、切换本地选择恰好一次；其他鼠标事件以及落在已不在权威 items 中的旧 ID 行都为惰性。Saved Config 确认框打开时，只有它自己的 `[ Confirm ]` 与 `[ Cancel ]` 接受左键 Down，模态下的其它命中全部惰性。上下键按可见行移动 Controls/Saved Config 焦点，GPIO 双列行会保持左右位置，左右键只在同一物理行的兄弟 GPIO cell 间切换；Status 页方向键直接滚动。Power/Switch/System 的整行和每个 GPIO 半行 cell 都有独立鼠标命中区域。静态 System `Reboot` 行排在所有动态 switch 之后、GPIO 之前，并使用同一套三秒危险确认门控
- TUI 的 Power 行完全使用最新固件状态中的 `power_outputs` 目录并保持固件顺序；host 不维护 rail 常量，新增/移除/重排输出都会随下一次状态刷新反映，`5V_FIN` 作为输入电源不会成为可控行
- TUI GPIO 排列只使用固件返回的 `layoutGroup/layoutLabel/layoutRow/layoutColumn` 与可选顶层 `gpio_layout_groups:[{name,edge,order}]`，不会在 host 端硬编码连接器或 pin map，也不会按 `note` 隐藏任何 GPIO；包括 `CON_MAS` 在内的所有固件返回项都会显示。已描述组按 `order` 升序，未描述组按首次出现顺序置后；缺少组元数据时保持原通用顺序。仅 `edge=right` 的组名追加可丢弃的 `›`（例如 `J8›`、`J13›`），未知 edge 不装饰；顺序和装饰均不按连接器名字特判。宽度不足时先去掉 `›`，再去掉组名，优先保留状态、方向、电平和 pending/hold；装饰不改变命中矩形或选择身份。80/48 列下同一固件物理行最多显示两个独立半行 cell，47/32 列时每针单行，缺失引脚 metadata 时按固件快照顺序回退显示；CLI 原始 JSON 输出不经过此投影
- TUI GPIO 操作是直发语义：`l`/`L` 驱动 LOW，`o`/`O` 驱动 HIGH，`i`/`I` 恢复输入；Enter、Space、`0`、`1` 对 GPIO 均为惰性。直接键解码只接受小写 `l`/`o`/`i` 且无任何修饰键，或大写 `L`/`O`/`I` 且修饰键恰好为 `Shift`；其它修饰键组合（Ctrl、Alt、Super、或小写配 Shift、大写无 Shift 等失配形式）返回 `None`，不会取消既有手势、也不会派发任何硬件请求。鼠标左键在 600ms 前释放后进入 220ms 双击等待窗，窗口到期且没有第二次按下时才驱动 LOW；按住到 600ms 一次性驱动 HIGH，同一引脚在等待窗到期前第二次按下/释放恢复输入且不会产生瞬态 LOW；中键、右键始终惰性。终端 `Resize` 是 TUI 强制重绘边界：Resize 到来时取消任何进行中的 GPIO 手势（包括初始左键 Down 与 AwaitSecond 状态），整体清空 `hit_map` 使旧几何失效并立即重绘，ready-event 排空也在 Resize 处停下，Resize 之后排队的鼠标事件按新几何处理、不会落在旧 cell 上。cell 始终显示最近一次权威快照的 `◌ IN LOW`、`◌ IN HIGH`、`○ OUT LOW`、`● OUT HIGH`（LOW 深灰、HIGH 红色加粗），不做乐观更新；在途动作在状态后缀后追加黄色加粗的 `[LOW…]`/`[HIGH…]`/`[INPUT…]` 标记，按住中的引脚追加 `[HOLD…]`（await-second 无标记；同一 cell 至多一个标记，在途动作标记优先），选中态下标记在白底选区中保持可见并加粗，宽度不足时先整体丢弃标记；当 cell 宽度足够时状态后缀完整，更窄的 cell 允许对后缀做整体 hard-clip，在途动作同时由状态行 `gpio <name>=…` 兜底；选中 GPIO cell 时 keybar 切换为 GPIO 专用键位（`l LOW`、`o HIGH`、`i INPUT`、`Mouse click/hold/2x`，47 列内完整放下这四段），其余键位按剩余宽度整段取舍；所有行按 `unicode-width` 显示列裁剪，CJK 字形不会被截半或折行；电源和固件广告的每个 switch 第一次操作都只打开红框确认，Controls 的 MODE 因而统一显示 `confirm`。TUI 把这套三秒确认门控无差别套到所有固件广告的 switch 上，不看固件 `requires_confirm`；策略本身不硬编码任何生产 switch 名称、路由或 pin。点击 switch 时其整行焦点会先更新并保留在确认框下，未被模态遮住的 cell 仍保留合成 `accent.select` 背景，且此时不会发送 PUT；硬件确认事件仅在严格早于 3 秒（`now < started + CONFIRM_TIMEOUT`）按下才执行一次。达到或超过 3 秒的确认按键按超时处理，发出该命令的 `timeout_message`，不依赖渲染/poll tick 是否已观察到超时；取消或超时均不发请求。固件 `requires_confirm` 数据仍供 Saved Config、API 与 CLI 使用；确认框是常态界面之外唯一带边框的区域
- x1.2 的 safe/status/config 包含十一个动态 GPIO：GP5/6/7/8/9/12/13/14/15/16/29；GP5/GP6 是可输出的直接 GPIO，使用普通 GPIO 命令与 config ID，不作为 switch 路由或专用功能控件；GP29 为 input-only，GP17/GP18 仍不存在。代表性样例按固件 metadata 投影为 J8 的 ADC3、GP5、GP16、GP6、GP15、GP14、GP13、GP12，再到 J13 的 USER、RSET、MASKROM；80/48 列的 J8 row4 配对 ADC3/GP5、row5 配对 GP16/GP6，共八个 GPIO 可视行，47/32 列每针单行，无 fallback 行。完整 24 位置物理图仅供 Web，CLI/TUI 不请求 `/gpio/layout`，也不添加 host 物理 pin map。样例仅用于回归验证；坐标及选择/命中契约见 [DESIGN.md](DESIGN.md#x12-representative-fixture-test-data-only)
- x1.2 switch 目录仅有 `sd`、`tf_wp`，已移除 `switch/usb`；Controls/Status/Saved Config 只呈现固件广告的目录，通用 switch 命令与模型仍支持任意广告名称/路由，不增加 host 板级白名单。config 目录共 19 项（六路电源、两个 switch、十一个 GPIO），固件序列化快照上限为 19 entries / 88 bytes；host 不实现该二进制格式或板级数量限制。`power/5v_usb_hub`、`power/5v_otg`、`gpio/GP5` / `gpio/GP6` 为正常保存项；旧 `switch/usb`、`gpio/GP17` / `gpio/GP18` 保存请求由固件以 `unknown_item` 拒绝，CLI 如实报告，不重映射或静默丢弃。
- `5v_usb_hub` 与 `5v_otg` 是固件广告的电源子轨，没有对应 ADC descriptor/电流采样。Controls 按 `power_outputs` 显示其真实 on/off 状态，缺测显示 `-` 而非 `0A`；Status 以现有只读文本行同样列出所有电源及未知测量，Saved Config 沿用电源确认流程。电流 scope 仍为四路 INA，遥测仍为八个 descriptor，CSV 不新增该输出的伪造测量列；host 不持有其硬件 pin map。
- GPIO 鼠标手势以终端 cell 为容差单位：同一 cell 内的 Moved 或左键 Drag 报告保持手势有效；只有 pin、终端列或终端行变化而跨入另一 cell 才会取消。到达 600ms 的 Up 立即按原始 pin 派发 HIGH；await-second 到达 220ms 的第二次 Down 先派发原始 pin 的 LOW 并消费该次 Down，下一次新的 Down 才开始新手势。
- TUI 对所有 switch 和静态 Reboot 行统一使用现有三秒确认门控；host 只调用固件已有 HTTP 端点，不改变两秒常规轮询周期、硬件默认值或固件行为
- MASKROM/EDL 只作为固件目录提供的普通自动化任务出现；使用 `task list` 查看并通过 `task run builtin/...` 执行，TUI 不提供专用入口；host 不持有任何恢复配方
- Task-v2 使用四槽 store、独立 UUID/revision 和 exact source/IR（各 ≤4000 字节）。CLI 命令为 `task list|store FILE [TASK-ID]|delete ID|clear|import FILE|export FILE|source ID FILE|run ID --confirm`。`task store` 当前发送 one-task IR、不发送 source，因此创建 IR-only task；board-backed 无损 source 由 Web/API source-bearing upsert 写入。`task source` 对 IR-only task 返回 `task_source_absent`。Task 写入只有二进制 mutation；详见[HTTP task 契约](../docs/user/api.md#tasks-firmware-catalog-and-task-v2-storage)与[CLI 任务说明](../docs/user/cli.md#tasks-firmware-catalog-and-task-v2-storage)。
- GPIO 在 CLI/TUI 中会同时显示 `GPxx` 和 `note`；控制时可使用 `GPxx`、数字引脚（如 `13`）或精确 note（如 `CON_MAS`）。`GP29` 仍在持久化/安全目录中，但由 ADC3 占用时是 input-only；输出请求会被固件拒绝。参见[固件说明](../apps/radxa_linkr_debugger/README.md)与[权威 ADC telemetry contract](../docs/reference/adc-telemetry.md#gp29-ownership)。
- `adc read` 返回八个 descriptor，固件顺序为 `5v_out`、`12v_out`、`20v_out`、`vdd_5v`（四路 INA 电流），然后 `adc0`、`adc1`、`adc2`、`adc3`。ADC0 的 `role=pd_input`，canonical value 是固件按 Device Tree R46/R47 分压还原的源侧 DC20V；`raw`/`mv` 仍是 pad 诊断值。ADC1-3 的 `role=native`，canonical value 是 pad 电压。host 不持有 GPIO/J8 映射或分压比例，不做 `115/15`、ADC 校准、零点修正或外部电压推断；ADC0 未校准精度约 2%。`voltage_valid=false` 的电源轨电压保持未知，不会由 `mv=0` 补成零伏。固件提供的 `note`/`label` 泛化透传显示，缺省为空；`adc read -v` 显示 `signal`、`role`、`source_voltage`、`pad_mv`、`raw` 等诊断字段。
- 实时 WebSocket 使用紧凑形状：单样本 reading 包含 `name`、`signal`、`kind`、`unit`、`role`、`value`，其中 `value` 是由 `unit` 指定的 signed integer（电流 `uA`、电压 `uV`），且只有电流通道携带 `power_enabled`。Batch 先声明 `channels[]`，每个 `values: i32[]` 按位置与 `channels` 对齐；`power_enabled_mask` 是 unsigned 8-bit 掩码，仅对 `kind="current"` 的通道有意义。ADC0 的 value 是源侧值，ADC1-3 是 native pad 值。WS 不携带 HTTP 的 `raw`、`mv`、`current_ua` 或 `sensor_value`；完整 wire shape 见[权威 ADC telemetry contract](../docs/reference/adc-telemetry.md)。
- `adc record OUTPUT_PATH [MAX_SAMPLES] [--rate-hz HZ]` 会创建 live websocket session；默认请求 250Hz，`--rate-hz` 只接受 1..250Hz 整数，超限在创建 session 前拒绝（固件 WS 同样拒绝超限请求并返回 `invalid_rate`）。高于 100Hz 时 CLI 请求 18 样本 batch JSON 并逐样本展开；batch 包含 8 个 descriptor、每样本 8 个 scalar values 和 4 个 rail `voltages_uv`，power/voltage-valid masks 只对应 current descriptor 索引的低四位。长度不匹配、mask 高位或超出 18 样本会拒绝，无旧五通道 tuple 专用兼容。`.ndjson` 保留全部八路及 `metadata.requested_rate_hz`；`.csv` 在四个时间列及既有四路电流 rail 的三列后，按固件 descriptor 顺序追加 `adc0_voltage_uv` 至 `adc3_voltage_uv`。ADC0 列是 canonical 源侧值，ADC1-3 是 native pad 值；缺失或无效电压写空单元格，不假造 `0`；真实关断状态写 `false`，真实零电流/零电压写 `0`。正常完成或连接、解析、写盘失败后都会关闭并删除自身 session
- 固件最多支持四个并发 websocket 客户端，多个 `adc record` 可以同时运行；触发式功耗采集使用全局硬件缓冲区，同一时间只能有一个 capture owner
- recorder 会写入主机接收时间和 `metadata.requested_rate_hz`，并把设备 `sample_sequence`、`uptime_us` 和 `device_t_mono_us` 放入 `metadata.device_timing`；紧凑 batch 仅提供 `sequence` 与 `uptime_us` 时，Rust 会将其归一化为对应别名，也会接受固件显式提供的别名；CSV 时间列优先使用 `device_t_mono_us`，否则回退到 `uptime_us`，再否则为 0；采样环覆盖通过 `metadata.dropped_samples` 显式报告，分析采样间隔时应优先使用设备时间
- TUI 仍只为四路 INA 电流绘制 scope，另在 scope 下方用一行只读 ADC 电压 strip 展示固件电压 descriptor，ADC0 显示 canonical 源侧值，ADC1-3 显示 native pad 值，保留可缺省说明和未知 `--`。窄屏复用现有分栏与 CJK 安全裁剪、按空间减少小数位，不加入电流历史或功率计算；终端不足 7 行时 strip 收起，keybar 保持最后一行，GPIO 选择及命中身份不变。
- `raw` 模式在 HTTP 路径下不支持
- `watchdog` 仍只暴露 `status`，不提供 host 侧 feed/控制
- `reboot` 严格不接受额外参数，执行普通冷重启（normal cold restart），不会进入 ROM BOOTSEL；`bootloader` 是独立的 ROM BOOTSEL 操作，二者不可互换。TUI 的 Reboot 行也明确显示 `not ROM BOOTSEL`，取消或三秒确认超时不会发出请求
- `vdd_5v` 作为固件广告的独立电源输出控制；host 不再保留已移除的 `switch/usb` 专用路由分支、联动规则或默认值。启动状态与 GPIO/电源硬件事实均由固件决定。
- 安装统一桌面栈后，`-d` / `--desktop` 会启动单个 `linkr-tray` 后台守护并立即退出；Tray 与 Host 服务运行在同一进程，普通 CLI/TUI 启动也会自动确保它已运行。无图形桌面或 GTK 已加载但 AppIndicator 实现不可用时，同一二进制会以 `--headless` 模式常驻；之后恢复完整图形环境再运行 CLI 或 `-d`，headless 守护会优雅交接为单个图形 Tray 守护。Linux 的 `linkr-tray` 本身仍链接 GTK；完全未安装 GTK 的最小系统应直接运行 `linkr-host serve`。普通 CLI/TUI 在桌面组件缺失时会继续工作，只有显式 `-d` 会把启动失败作为错误。仅在明确不需要尝试任何后台 Host 时设置 `LINKR_SKIP_DESKTOP=1`。

## ADC CLI与记录格式

```sh
radxa-linkr-debuggerctl adc read
radxa-linkr-debuggerctl adc read adc0 -v
radxa-linkr-debuggerctl --json adc read
radxa-linkr-debuggerctl adc record samples.ndjson 250 --rate-hz 250
radxa-linkr-debuggerctl adc record samples.csv 250 --rate-hz 250
```

八路顺序是固件 descriptor 的顺序；客户端不会按 GPIO 编号或连接器重新排序。
电流使用 `uA`。ADC0 的 canonical `value` 是源侧 DC20V（例如 `20000002`
归一化为 `source_voltage=20.000002V`），`pad_mv` 仍是 pad 诊断值；ADC1-3 的
`value` 是 native pad `uV`。host 不乘分压比。这只是字段示例，不是硬件测量。
HTTP rich `sensor_value` 与 WS compact `kind/unit/value/role` 归一化到同一语义；
现有 JSON 命令的输出路径与错误规则不变。

CSV 前 16 列保持原定义：4 个时间列，以及 `5v_out`、`12v_out`、`20v_out`、
`vdd_5v` 各自的 `power_enabled,current_ua,voltage_uv`。随后追加：

```text
adc0_voltage_uv,adc1_voltage_uv,adc2_voltage_uv,adc3_voltage_uv
```

电压列来自第一条 telemetry 的 descriptor 顺序，不从 host 固定引脚列表推断。
后续样本缺少通道/数值或电压无效时保留空单元格；实测零值才写 `0`。
NDJSON 保留完整八路归一化读数和固件提供的可选说明。

## Released 与 nightly 通道

- 正式 release 由 GitHub `Release` workflow 在推送 `v*` tag 后产出，资产、签名策略与现有 release 一致。
- 一条独立的滚动 nightly workflow `.github/workflows/nightly.yml` 仅在每次推送到 `dev` 分支时把可变 `nightly` Git tag 与标记为 `not latest` 的 prerelease 一同发布，覆盖固定 9 个资产（4 个固件产物、Linux AMD64 / macOS ARM64 / Windows AMD64 三份 CLI 归档、skill bundle 与 `SHA256SUMS.txt`），并在每次 run 中清理先前多余资产。nightly 是滚动且仅用于测试，不会替换正式 `v*` release；正式 release 仍多出 Linux ARM64 CLI 归档这一份、共 10 个资产，是生产交付的唯一权威来源。
