# CLI 参考

[English](cli.md)

`radxa-linkr-debuggerctl` 通过 USB NCM 与调试板通信。release 安装包还会提供
指向同一可执行文件的 `rdb` 链接，因此两个名称接受完全相同的命令。下文示例省略
连接选项。URL 优先级为显式 `--url` / `--addr` / `--port` >
`LINKR_BOARD_URL` > 自动发现 > 报错。未显式指定 URL、也未设置环境变量 URL 时，
仅发现唯一已验证设备才自动选中；没有已验证设备时返回 `device_not_found`，
有多个时返回 `multiple_devices`，必须用 `--url http://<板卡 IP>` 指定设备。

命令参数和输入文件在设备发现或桌面启动前完成本地校验。自动发现的 HTTP 与
ADC Recorder WebSocket 连接均保留选定的本机 IPv4 地址，不使用代理或跟随
重定向。显式指定 URL 的 Recorder WebSocket 同样拒绝重定向；显式 HTTP URL
仍保留原有客户端策略。

板子运行 DHCPv4 server，主机会自动获取兼容地址。每块板首次启动会派生并持久化
唯一的私有 `/30` 与 MAC。主机 NCM 地址是该网络中的 `.2`，板卡是同一 `/30`
的 `.1`。CDC shell 的 `ip` 命令也会打印板卡 URL。

安装统一桌面栈后，CLI/TUI 板命令会先解析出唯一板卡目标，再启动
`linkr-tray`，并通过 `--board-url` 把解析出的同一个 URL 显式传给 Tray；
Tray 与 Host 服务位于同一守护进程。`-d` / `--desktop` 使用相同的
显式参数、环境变量、唯一设备自动发现目标选择顺序，只启动桌面启动器和守护后退出，
守护仍常驻。`--version`、`--help`、`discover` 和无效命令不会触发桌面自启。
普通 CLI/TUI 在桌面启动失败时发出警告并继续板命令，只有显式 `-d` 把失败视为
fatal。仅在明确不需要尝试任何后台 Host 时设置 `LINKR_SKIP_DESKTOP=1`。

复用已有 Host 前会核对 `/host/api/v1/status` 的 `board_url`；只有 URL 相同才复用。
URL 不同时，诊断会列出两个地址；CLI 不会静默切换、关闭已有 Host/Tray，也不会
中断其共享会话。普通板命令仍会在警告后直接访问所选板卡，已有 Tray/Web UI 则
保持原板卡；显式 `-d` 会失败。需要切换桌面服务时，先检查其他客户端，再退出
现有 Radxa Linkr Tray（或停止手动启动的 `linkr-host` 服务），随后运行
`radxa-linkr-debuggerctl --url http://<板卡 IP> --desktop`。

没有 `DISPLAY` / `WAYLAND_DISPLAY`，或 GTK 已加载但 Tray 找不到 AppIndicator
实现时，CLI 会把同一二进制改以 `--headless` 模式启动。之后恢复图形环境时，
再运行 CLI/TUI 或 `-d` 会让 headless 守护优雅退出并由单个图形 Tray 守护接管，
不会同时保留两套进程。Linux 的 `linkr-tray` 即使在 headless 模式也仍依赖
GTK3 loader；完全未安装 GTK 的最小系统可直接运行
`linkr-host serve --board-url http://<板卡 IP>`。standalone Host 不继承 CLI
自动发现：跨进程只传解析后的 URL，不传自动发现 CLI 连接的本地源地址绑定、
no-proxy 或 no-redirect 策略。

桌面栈启动失败时，CLI 会把守护与 Host 的启动输出写入并指向
`$XDG_DATA_HOME/radxa-linkr-debugger/host.log`；Linux 默认路径为
`~/.local/share/radxa-linkr-debugger/host.log`。headless 模式不会构造
AppIndicator UI。源码树开发时，CLI 按 `LINKR_TRAY_BIN`、packaged sibling、
安装路径/PATH、源码树 debug 的顺序查找 Tray；Nix 包会设置 `LINKR_TRAY_BIN`
指向已包装 Tray。在 NixOS 直接运行源码树 debug Tray 仍需提供 GTK3 与
`libayatana-appindicator3` 的开发 shell，也可把 `LINKR_TRAY_BIN` 显式指向
正确包装的可执行文件。

该 daemon 仍包含 legacy CH347 `uart0`/`uart1` Serial Broker 与 MCP serial
client；这些是等待单独移除的非 J4 兼容/测试夹具路径，不是生产目标终端。J4 应使用
板载 xterm.js、MCU `serial.j4`、J4 CDC ACM 或 raw TCP 5555。

`discover` 列出 USB-NCM 候选设备，支持 `--json` 与 `--timeout`。显式执行
`discover` 时始终扫描本机接口，即使设置了 `--url` 或 `LINKR_BOARD_URL`。
发现仅探测本机接口的 IPv4 `/30` 对端，并严格校验 `/api/v1/status` 的 `schema`、`ok`、
`command`、`project` 后，才将候选视为已验证设备。

```sh
# 列出 USB-NCM 候选设备，也可输出 JSON
radxa-linkr-debuggerctl discover
radxa-linkr-debuggerctl --json discover

# 检查连接和板子健康状态
radxa-linkr-debuggerctl doctor
radxa-linkr-debuggerctl status

# 发现多个设备时，显式选择板卡
radxa-linkr-debuggerctl --url 'http://<板卡 IP>' status
```

不带子命令运行会启动[交互式 TUI](tui.zh-CN.md)。以下所有子命令都在 CLI 模式下使用。

## JSON 输出

脚本和自动化场景加 `--json` 获取结构化输出。所有响应格式统一：

成功响应：

```json
{"schema": "radxa-linkr-debugger.v1", "ok": true, "command": "status"}
```

失败响应（仅当 `ok` 为 `false` 时才包含 `error`）：

```json
{"schema": "radxa-linkr-debugger.v1", "ok": false, "command": "status", "error": {"code": "request_failed", "message": "..."}}
```

```sh
radxa-linkr-debuggerctl --json status
radxa-linkr-debuggerctl --json power list
radxa-linkr-debuggerctl --json adc read
radxa-linkr-debuggerctl --json gpio list
```


## 网络身份

```sh
radxa-linkr-debuggerctl --url http://10.20.30.1 ip
radxa-linkr-debuggerctl --url http://10.20.30.1 ip network 10.20.30.0/30
radxa-linkr-debuggerctl --url http://10.20.30.1 ip mac 02:11:22:33:44:55
radxa-linkr-debuggerctl --url http://10.20.30.1 --json ip
```

`ip` 输出持久化的 MAC、来源、私有 `/30`、板卡地址、DHCP 基址和板卡 URL。
`network` 与 `mac` 修改由固件原子持久化。`net` 是 `ip` 的别名。非法 MAC、
非私有或未对齐的 `/30` 会在本地被拒绝。修改网络后可能需要重新连接 USB NCM
接口或重新获取主机 DHCP 租约；后续命令用 `--url` 指向新的板卡 URL。

## 电源

四条可控电源轨：`12v_out`、`5v_out`、`20v_out`、`vdd_5v`。其中 `vdd_5v`
是辅助 5 V 电源轨，与其他输出一样通过 CLI 和 TUI 暴露。

```sh
radxa-linkr-debuggerctl power set 12v_out on
radxa-linkr-debuggerctl power set 12v_out off
radxa-linkr-debuggerctl power set 5v_out on
radxa-linkr-debuggerctl power set 5v_out off
radxa-linkr-debuggerctl power set 20v_out on
radxa-linkr-debuggerctl power set vdd_5v on
```

列出所有电源输出：

```sh
radxa-linkr-debuggerctl power list
```

## ADC（电流与电压监测）

四路 INA226 电流监测覆盖 `5v_out`、`12v_out`、`20v_out`、`vdd_5v`。
`adc0` 是 PD 输入源侧通道：canonical 值是 Device Tree 按 R46/R47 分压还原的
DC20V 源侧电压；`raw` 和 `mv` 仍是 ADC pad 诊断值。`adc1` 到 `adc3` 是 native
pad 电压通道。默认输出简洁（`5v_out=0.540000A`），加 `-v` 可看到 `signal`、
`source_voltage`、`pad_mv`、`raw` 等字段。

```sh
radxa-linkr-debuggerctl adc read
radxa-linkr-debuggerctl adc read 5v_out
radxa-linkr-debuggerctl adc read vdd_5v
radxa-linkr-debuggerctl adc read adc0
radxa-linkr-debuggerctl adc read adc3
```

CLI 直接展示固件 canonical 值，不做 `115/15`、校准表或零点修正。ADC0 未校准
精度约为 2%；没有逐板校准时不得声称更高精度。

### 录制

`adc record` 打开 websocket session 并将遥测数据写入文件：

```sh
radxa-linkr-debuggerctl adc record /tmp/adc.ndjson 500
radxa-linkr-debuggerctl adc record /tmp/adc.csv 500 --rate-hz 250
```

- 输出格式由文件扩展名决定：`.ndjson` 或 `.csv`
- 默认订阅速率为 250 Hz；`--rate-hz HZ` 接受 `1..250`。CLI 和固件都会拒绝
  更高速率，而不是静默截断
- 每条输出记录包含主机接收时间戳和 `metadata.requested_rate_hz`
- 设备时间保留在 `metadata.device_timing` 中
- 采样环覆盖通过 `metadata.dropped_samples` 在首个受影响行报告
- NDJSON 保留全部八个描述符；CSV 先写四路电流 rail，再追加
  `adc0_voltage_uv` 到 `adc3_voltage_uv`。ADC0 是 canonical 源侧值，
  ADC1-3 是 native pad 值；未知或不可用字段写为空单元格。

### JSON ADC 输出

`--json adc read` 返回固定八通道顺序的完整诊断链路：四路电流 rail 后接
ADC0..ADC3。ADC0 的 canonical scalar 是源侧 DC20V，`raw`/`mv` 是 pad 诊断值；
ADC1-3 是 native pad 电压。主机 CLI 直接展示固件值，不做主机侧校准或分压换算。
PD 控制使用 `pd status`、`pd fixed MV` 和
`pd pps MV`（整板上限 20 V）。

## Switch 路由

x1.2 暴露两个固件 switch 控件：

| Switch | 切换内容 | 可选值 |
|--------|---------|--------|
| `sd` | TF/SD 卡路径 | `target`、`usb-reader` |
| `tf_wp` | TF 写保护 | `writable`、`protected` |

```sh
radxa-linkr-debuggerctl switch list
radxa-linkr-debuggerctl switch get sd
radxa-linkr-debuggerctl switch route sd usb-reader
radxa-linkr-debuggerctl switch route tf_wp protected
```

x1.2 没有 USB 或 VIN/VIO 路由。`/switch/usb` 返回 HTTP 404；`vdd_5v` 是独立
power output。

## GPIO

11 个 safe GPIO 包含普通直连 GP5/GP6 和仅输入 GP29。命名支持 `GP13`（标准）、
`13`（原始引脚号）、`CON_MAS`（板级 note）。CLI 同时显示 `GPxx` 和 note。

```sh
radxa-linkr-debuggerctl gpio list
radxa-linkr-debuggerctl gpio set GP13 1
radxa-linkr-debuggerctl gpio set CON_MAS 1
radxa-linkr-debuggerctl gpio input GP13
```

## 自动化测试脚本

`test run` 执行 `linkr-test.v1` NDJSON 脚本。其 `--serial` / `--serial-uart0` /
`--serial-uart1` 是保留的 generic legacy 双 UART/CH347 测试夹具，不选择 MCU J4，
不建议操作员用于 J4。在该 legacy runner 内，UART1 仍必须显式指定设备路径，绝不
静默路由到 UART0。`--output` 按 `.json`、`.csv`、`.ndjson` 扩展名选择格式。

```sh
radxa-linkr-debuggerctl test run startup.ndjson --serial /dev/tty.usbserial-1234
radxa-linkr-debuggerctl test run dual-uart.ndjson \
  --serial-uart0 /dev/tty.usbserial-A \
  --serial-uart1 /dev/tty.usbserial-B
radxa-linkr-debuggerctl test run startup.ndjson --output startup-report.json
```

`gpio_assert` 会直接使用步骤 `params` 中的 `direction` 和 `value` 进行判定；方向只接受 `input`/`output`，值只接受 `0`/`1`。执行被 Ctrl+C 中止时，报告会标记为 `aborted`。使用全局 `--json` 时，stdout 遵循统一的 `radxa-linkr-debugger.v1` envelope，计数信息位于 `summary`。

如需重复执行一组连续命令，可以添加顶层 `loop` 项。`count` 支持
1-1000 轮，`steps` 至少包含一条普通命令。当前不支持嵌套循环，脚本展开后最多
执行 10,000 条命令。Web UI 中可以勾选连续命令，直接将它们放入循环框。

```json
{"id":"boot-loop","type":"loop","params":{"count":3,"steps":[{"id":"cycle-off","type":"power_off","params":{"rail":"5v_out"}},{"id":"settle","type":"delay","params":{"ms":1000}},{"id":"cycle-on","type":"power_on","params":{"rail":"5v_out"}}]}}
```

## 重启与 BOOTSEL

`reboot` 与 `bootloader` 是两个独立操作：

```sh
# 正常冷重启；响应后 250 ms 执行，绝不进入 ROM BOOTSEL
radxa-linkr-debuggerctl reboot

# 显式进入 ROM BOOTSEL；只刷合成完整 UF2
radxa-linkr-debuggerctl bootloader
```

`reboot` 发送 `POST /api/v1/reboot`，通过正常 JSON envelope 返回
`command: "reboot"`，等待 250 ms 后执行正常冷重启。reboot 动作本身绝不请求
ROM BOOTSEL，也不会枚举为 `RPI` 大容量存储设备；如果复位期间一直按住物理
BOOTSEL 按键，则由 ROM 单独决定进入 BOOTSEL。未被已保存 v1 配置快照恢复的
电源轨回到各自的 boot-off 默认状态。板子恢复后必须重新设置并验证电源轨与 PD。

`bootloader` 发送 `POST /api/v1/bootloader`，是唯一请求 ROM USB boot 路径的
CLI 命令。ROM BOOTSEL 只能使用合成完整 `radxa-linkr-debugger-rp2350.uf2`；
仅含应用固件的 `zephyr.uf2` 不包含 MCUboot，绝不能用于 ROM BOOTSEL。

仅通过 CDC ACM shell 操作时，重启与 ROM BOOTSEL 的命令是 `reboot` 和
`bootloader`。`reboot` 打印正常重启提示且不进入 BOOTSEL；`bootloader` 打印
BOOTSEL 提示。

物理 BOOTSEL 按键语义不同：固件运行中长按 3 秒后释放才请求正常重启；
短按不动作；一直按住不释放不会复位；复位或上电时按住则由 RP2350 ROM
采样并进入 ROM BOOTSEL。按键由固件线程轮询，无法恢复完全卡死的核心；
该情况下硬件 watchdog 仍是兜底路径。TUI 中选中 **System / Reboot** 行并确认
即可请求正常重启；TUI 不提供 BOOTSEL 行或直接按键。需要 ROM BOOTSEL 时使用
独立的 CLI `bootloader` 命令或 Web **Enter BOOTSEL** 卡片。详见
[TUI 指南](tui.zh-CN.md) 与 [Web UI 指南](webui.zh-CN.md)。

## 任务（固件目录与 task-v2 存储）

内置自动化任务来自固件拥有的不可变目录 `GET /api/v1/tasks/catalog`。CLI 不保存
恢复配方：rail、GPIO、极性、等待和 cleanup 全部来自目录。当前目录始终提供六条
MASKROM/EDL 配方，覆盖两种模式和 `5v_out`/`12v_out`/`20v_out` 三个 rail。

Stored task 位于独立的四槽 task-v2 store。板子为每个任务保存最多 4000 字节的
exact canonical `linkr-test.v1` source 和最多 4000 字节的 `linkr-task.v1`
deployment IR。Store 有独立 UUID、uint64 revision、catalog fingerprint 和
SHA-256 source/IR digest。Built-in 不占槽，也不出现在 stored metadata 中。

```sh
# 列出合并后的固件目录（built-in 与 stored）
radxa-linkr-debuggerctl task list

# 直接运行内置任务，不写入固件 flash
radxa-linkr-debuggerctl task run builtin/maskrom/12v_out --confirm

# 把 one-task IR 文件写成 IR-only board task
radxa-linkr-debuggerctl task store my-task.ndjson my-task-id

# 导入完整 linkr-task.v1 IR 集，替换全部 board task
radxa-linkr-debuggerctl task import all-tasks.ndjson

# 导出 exact full IR set
radxa-linkr-debuggerctl task export all-tasks.ndjson

# 导出 board-backed exact source（存在时）
radxa-linkr-debuggerctl task source my-task-id my-task.linkr-test.ndjson

# 原子删除单个任务或清空全部任务
radxa-linkr-debuggerctl task delete my-task-id
radxa-linkr-debuggerctl task clear

# 执行 stored task：CLI 取回 IR 并按顺序派发记录
radxa-linkr-debuggerctl task run my-task-id --confirm
```

`task store` 接受 canonical `linkr-test.v1` workflow source 或 one-task
`linkr-task.v1` IR，并在本地编译/规范化。当前 CLI 不发送 source payload，因此
board task 是 **IR-only**。需要 board-backed 无损 source 时使用 Web workflow
editor。`task source` 只对通过 source-bearing Web/API upsert 写入的任务成功；
否则返回 `task_source_absent`。`task import` 只接受 full-set IR，只有同 ID 且
task-IR digest 未变时才保留 source。`task export` 写出 exact full IR set，并报告
full-IR digest。

Task-v2 store 只有四个 slot，source/full-IR 上限均为 4000 字节。Mutation 携带
当前 revision 与 catalog fingerprint；陈旧值返回冲突且零写入。`task clear` 保持
store UUID，不改变实时硬件。Task 写入只有 JSON/base64 mutation；CLI 不会发送 blob
upload 或 per-ID write/delete 请求。已移除的
`linkr/task/tasks` Settings blob 不读取、不迁移。

`task list` 的文本输出还会打印 `storage` 行（NVS 已用/可用、任务字节数、预留和
`save_allowed`）；JSON 输出保留固件的 `storage` 对象。`task list` 合并不可变
built-in 与 task-v2 metadata。每行显示
`source=builtin|stored`、request count 和
`lossless-source=present|absent|unavailable`。Built-in 没有无损 workflow source；
IR-only stored task 明确为 `absent`。catalog 端点失败时 stored task 仍可见，并带
结构化 `catalog_error` 与 `catalog_available:false`；请求 `builtin/...` 返回
`catalog_unavailable`，不回退到 stored task。

执行行为——`task run <task-id> --confirm` 采用 fail-closed 契约：缺少 `--confirm`
时在任何板卡请求前返回 `confirmation_required`。确认后先取固件目录并优先解析
built-in；命中 built-in 时不读取 task-v2 metadata。否则 CLI 下载
`/api/v1/tasks/ir`，按 id 选中 task，再通过公共 HTTP API 顺序派发每条 `PUT`。
记录之间仅在响应成功之后按 `wait_ms` sleep。失败立即停止，后续记录不再派发。
JSON 失败字段为 `error.record_index`（1-based）和 `error.path`；非 JSON 失败写
`task record <n> path "<path>" failed: <message>` 到 stderr。wire `body` 是存储
记录的 `body` 原文，`wait_ms` 永不发送给板子。Ctrl+C 在请求之间或 `wait_ms`
期间协作取消。Built-in cleanup 来自目录，独立于主失败/取消报告；cleanup 失败
不覆盖主错误。首条请求前取消不执行 cleanup。

开机行为——固件永不在 boot 时存储、执行或播种任务。Stored task 保持静默，直到
CLI 或 Web UI 显式运行。旧 `linkr/task/tasks` blob 没有迁移、读取别名或自动
reset。

### CDC 串口 fallback

当 HTTP 或 Web UI 不可用，但 USB CDC ACM shell 仍能工作时，固件在
串口控制台暴露同一套任务接口。shell 命令只有 `task show` 和
`task clear`；CDC 路径上不存在 boot、default 或 replay 命令。

```console
linkr-debugger:~$ task show
task show available=true task_count=1
linkr-debugger:~$ task clear
task clear ok
```

`task show` 输出一行，包含 `available`（task-v2 NVS 后端是否
可达）和 `task_count`。`task clear` 先获取 capture 仲裁器，再获取
flash 仲裁器；如有任一处于忙状态（live capture 或 OTA 正在进行），
它返回 `task clear error=busy` 并以 `-EBUSY` 退出，**不会**删除
已存任务。清除成功时输出 `task clear ok` 并以 0 退出。后端错误
（忙路径之后）则输出 `task clear error=storage_error` 并以 `-EIO`
退出。CDC 表面只提供 show/clear，不运行、导入、导出或删除单个任务，
也**不**维护主机侧目录。这是 HTTP 或 WS 卡死时的 fallback，不改变
HTTP/CLI 表面。

## Watchdog

```sh
radxa-linkr-debuggerctl watchdog status
```

硬件 watchdog 由固件管理，主机无法喂狗或控制。只有以下三个服务同时健康上报时，watchdog 才会被持续喂到：

- 核心固件循环
- HTTP/API 服务
- CDC ACM cmdline fallback

其中任何一个卡住或停止上报，固件就会停止喂狗，MCU 复位，下次启动通过 recovery marker 进入 ROM BOOTSEL。WebSocket 静默、订阅超时、会话过期**不**会触发 watchdog 失败。

## OTA 固件更新

通过 USB 上传、测试和确认固件更新。只接受 MCUboot 格式的 `.bin` 文件——不要上传 `.uf2` 或 `.elf`。

```sh
radxa-linkr-debuggerctl ota status
radxa-linkr-debuggerctl ota upload radxa-linkr-debugger-rp2350-ota.bin
radxa-linkr-debuggerctl ota test
radxa-linkr-debuggerctl ota confirm
```

完整工作流和回滚机制：[OTA 固件更新](ota.zh-CN.md)。

## 板级自监控

`--json status` 的响应包含 `board_monitoring` 对象。每个类别都有 `available` 和 `reason`——固件只报告实际启用的设备或 API 数值。

| 类别 | 内容 |
|------|------|
| `temperature` | CPU die 温度 |
| `heap` | 系统堆使用（free、allocated、total） |
| `memory` | 跨多个池的内存压力 |
| `runtime` | 板端 uptime（`uptime_ms`、`uptime_seconds`） |
| `cpu` | CPU 利用率 delta |

### 内存压力

`memory` 类别跟踪 heap、网络 packet slab 和 data buffer pool 的压力：

- **`current_pressure`** — 实时快照，可升可降。主要关注这个。
- **`peak_pressure`** — 启动以来的高水位，包含线程栈使用。
- `pressure_pct_x100`（遗留）— heap 和 stack 的最大值，保留向后兼容。

两个对象都包含 `limiting_component`（哪个池达到上限）、`limiting_name`（实例名）和 `pressure_pct_x100`（范围 0–10000）。

`physical` 报告 linker 保留的占用——不是实时使用量。`stacks` 报告每个线程的高水位。

## 状态 LED

RP2350A 板上 GPIO25 的蓝色状态 LED 作为 watchdog 心跳指示灯。它约每秒闪烁一次，仅在硬件 watchdog 喂狗成功后推进。跳过或失败的喂狗会将 LED 重置为非活动状态。
