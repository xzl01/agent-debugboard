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
- TUI 使用无常态边框的高密度布局：顶部两行状态与按需出现的七行三通道电流示波器，其下固定显示 Controls、Saved Config、Status 三页页签和当前页表头；示波器按各通道可见峰值自适应量程，无数据时高度为零，底部是不会截断半个操作的 htop 式键位块
- Tab/Shift+Tab 前后切页；上下键按可见行移动 Controls/Saved Config 焦点，GPIO 双列行会保持左右位置，左右键只在同一物理行的兄弟 GPIO cell 间切换；Status 页方向键直接滚动。Power/Switch 的整行和每个 GPIO 半行 cell 都有独立鼠标命中区域
- TUI 的 Power 行完全使用最新固件状态中的 `power_outputs` 目录并保持固件顺序；host 不维护 rail 常量，新增/移除/重排输出都会随下一次状态刷新反映，`5V_FIN` 作为输入电源不会成为可控行
- TUI GPIO 排列只使用固件返回的 `layoutGroup/layoutLabel/layoutRow/layoutColumn`，不会在 host 端硬编码连接器或 pin map，也不会按 `note` 隐藏任何 GPIO；包括 `CON_MAS` 在内的所有固件返回项都会显示。正常宽度下同一固件物理行最多显示两个独立 cell，窄于 48 列时每针单行，缺失 metadata 的引脚按固件快照顺序回退显示
- TUI GPIO 主操作与 Web UI 一致：输入切到输出 HIGH，输出在 HIGH/LOW 间切换；状态使用 `◌ IN LOW`、`◌ IN HIGH`、`○ OUT LOW`、`● OUT HIGH`，LOW 深灰、HIGH 红色；电源和需要确认的 switch（包括 VIO）第一次操作只打开红框确认，三秒内再次确认才执行；确认框是常态界面之外唯一带边框的区域
- 本次 TUI 显示重设计不改变 HTTP 端点、两秒轮询周期、确认/动作语义、硬件默认值或固件行为
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

## Released 与 nightly 通道

- 正式 release 由 GitHub `Release` workflow 在推送 `v*` tag 后产出，资产、签名策略与现有 release 一致。
- 一条独立的滚动 nightly workflow `.github/workflows/nightly.yml` 仅在每次推送到 `dev` 分支时把可变 `nightly` Git tag 与标记为 `not latest` 的 prerelease 一同发布，覆盖固定 9 个资产（4 个固件产物、Linux AMD64 / macOS ARM64 / Windows AMD64 三份 CLI 归档、skill bundle 与 `SHA256SUMS.txt`），并在每次 run 中清理先前多余资产。nightly 是滚动且仅用于测试，不会替换正式 `v*` release；正式 release 仍多出 Linux ARM64 CLI 归档这一份、共 10 个资产，是生产交付的唯一权威来源。
