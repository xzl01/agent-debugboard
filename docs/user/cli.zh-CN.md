# CLI 参考

[English](cli.md)

`radxa-linkr-debuggerctl` 通过 USB NCM 与调试板通信。release 安装包还会提供
指向同一可执行文件的 `rdb` 链接，因此两个名称接受完全相同的命令。板子运行
DHCPv4 server，主机会自动获取兼容地址。默认设备 URL 为
`http://172.29.203.1`，仅在需要覆盖时才传 `--url`。

安装统一桌面栈后，运行 CLI/TUI 只会在后台启动一个 `linkr-tray` 守护；
Tray 与 Host 服务位于同一进程。使用 `-d` / `--desktop` 可只启动桌面启动器
和后台守护后退出，CLI 退出后守护仍常驻。没有 `DISPLAY` /
`WAYLAND_DISPLAY`，或 GTK 已加载但 Tray 找不到 AppIndicator 实现时，CLI
会把同一二进制改以 `--headless` 模式启动。Linux 的 `linkr-tray` 可执行文件
本身仍依赖 GTK loader；完全未安装 GTK 的最小系统应直接运行 `linkr-host serve`。
桌面组件缺失时普通 CLI/TUI 仍继续工作，只有显式 `-d` 把启动失败视为错误。
仅在明确不需要尝试任何后台 Host 时设置 `LINKR_SKIP_DESKTOP=1`。之后恢复图形环境时，再运行 CLI/TUI 或 `-d`
会让 headless 守护优雅退出并由单个图形 Tray 守护接管，不会同时保留两套进程。
桌面栈启动失败时，CLI 会把守护与 Host 的
启动输出写入并指向
`$XDG_DATA_HOME/radxa-linkr-debugger/host.log`；Linux 默认路径为
`~/.local/share/radxa-linkr-debugger/host.log`。在 NixOS 源码树中直接运行
图形 debug 二进制时，应从 `nix-shell` 内启动，以提供 Tray 所需的
GTK 与 `libayatana-appindicator3`；headless 模式不会构造 AppIndicator UI，
但 Linux 二进制仍需 GTK loader。

该 daemon 仍包含 legacy CH347 `uart0`/`uart1` Serial Broker 与 MCP serial
client；这些是等待单独移除的非 J4 兼容/测试夹具路径，不是生产目标终端。J4 应使用
板载 xterm.js、MCU `serial.j4`、J4 CDC ACM 或 raw TCP 5555。

```sh
# 检查连接和板子健康状态
radxa-linkr-debuggerctl doctor
radxa-linkr-debuggerctl status
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

四路 INA226 电流监测覆盖 `5v_out`、`12v_out`、`20v_out`、`vdd_5v`；
`adc0` 到 `adc3` 是四路仅监测 pad 电压通道。默认输出简洁
（`5v_out=0.540000A`），加 `-v` 可看到 `signal`、`mv` 等原始 ADC 字段。

```sh
radxa-linkr-debuggerctl adc read
radxa-linkr-debuggerctl adc read 5v_out
radxa-linkr-debuggerctl adc read vdd_5v
radxa-linkr-debuggerctl adc read adc0
radxa-linkr-debuggerctl adc read adc3
```

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
  `adc0_voltage_uv` 到 `adc3_voltage_uv`，未知或不可用字段写为空单元格

### JSON ADC 输出

`--json adc read` 返回固定八通道顺序的完整诊断链路：四路电流 rail 后接
ADC0..ADC3 pad 电压。主机 CLI 直接展示固件值，不做主机侧校准。PD 控制使用
`pd status`、`pd fixed MV` 和
`pd pps MV`（整板上限 20 V）。

## Switch 路由

x1.2 有两个物理信号路由切换器：

| Switch | 切换内容 | 可选值 |
|--------|---------|--------|
| `sd` | TF/SD 卡路径 | `target`、`usb-reader` |
| `usb` | J12 下层 USB 设备在主机与目标板之间切换 | `pc`、`target` |

```sh
radxa-linkr-debuggerctl switch list
radxa-linkr-debuggerctl switch get sd
radxa-linkr-debuggerctl switch route sd usb-reader
radxa-linkr-debuggerctl switch route usb target --confirm
radxa-linkr-debuggerctl switch route usb pc --confirm
```

USB 路由变更需要 `--confirm`，因为它有可见硬件副作用。x1.2 没有 VIN/VIO
路由，旧 `switch vin` 示例不适用。

对于 `switch usb`，J12 上层连接目标板，下层插入待切换 USB 设备。`pc` 路由把
该设备连接到 J15 所接主机；`target` 路由把该设备连接到 J12 上层所接目标板。

## GPIO

安全 GPIO 支持三种命名：`GP13`（标准）、`13`（原始引脚号）、`CON_MAS`（板级 note）。CLI 同时显示 `GPxx` 和 note。

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
电源轨回到各自的 boot-off 默认状态；`vdd_5v` 跟随 USB 路由默认值。板子恢复
后必须重新设置并验证电源轨与 PD。

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

## 任务（固件目录与保存的请求序列）

内置自动化任务全部来自固件拥有的不可变目录 `GET /api/v1/tasks/catalog`。
CLI 自身不保存任何恢复配方：每条 rail、GPIO、电平、等待与 cleanup 步骤都
由固件目录提供。当前目录固定包含六个 MASKROM/EDL 配方，分别覆盖模式与
`5v_out`、`12v_out`、`20v_out` 的组合：

```text
builtin/maskrom/5v_out     builtin/edl/5v_out
builtin/maskrom/12v_out    builtin/edl/12v_out
builtin/maskrom/20v_out    builtin/edl/20v_out
```

built-in 不占用固件任务槽，也不会通过 `/api/v1/tasks` 读取、写入或删除；
它们由通用任务 runner 派发，每条记录就是普通的 `gpio` 或 `power` PUT。

同一个合并列表还包括显式保存在调试板 flash `linkr/task/tasks` 键下的
任务 blob。固件只校验和保存 blob，不负责执行；CLI 通过
`GET /api/v1/tasks` 取回已存任务，在本地解析并按顺序派发。`wait_ms` 只在
成功请求后由客户端 sleep，不会进入控制端点。built-in 与 stored task 都
在首次失败时停止，失败或部分取消的 built-in 随后执行目录声明的 cleanup
序列；CLI 不推断也不硬编码任何 cleanup。stored task 不推断 cleanup。
built-in 成功结束时所选电源轨保持开启。该流程与执行正常冷重启的 `reboot`
无关，也与进入调试板 RP2350 ROM BOOTSEL 的 `bootloader` 无关。

固件目录接口不可用时，`task list` 仍然展示 stored task，同时返回结构化的
`catalog_error` 与 `catalog_available:false`；任何 `task run builtin/...`
调用都会返回 `catalog_unavailable`，不会回退到被 shadow 的 stored task。
stored task 在目录不可用期间仍可正常工作。

```sh
# 列出合并后的固件目录（built-in 与 stored）
radxa-linkr-debuggerctl task list

# 直接运行内置任务，不写入固件 flash
radxa-linkr-debuggerctl task run builtin/maskrom/12v_out --confirm

# 把本地文件里的 linkr-task.v1 blob 写入板子
radxa-linkr-debuggerctl task store my-task.ndjson my-task-id

# 执行已存储任务：CLI 取回 blob 并按顺序派发记录
radxa-linkr-debuggerctl task run my-task-id --confirm

# 清除全部已存储任务
radxa-linkr-debuggerctl task clear
```

保存记录规则——每条记录都是对 `/api/v1/power/`、`/api/v1/gpio/` 或
`/api/v1/switch/` 白名单下某路径的单条 `PUT`，附 JSON 字符串形式的
`body` 以及可选的整数 `wait_ms`（范围 `0..60000`）。blob 始终以
`# linkr-task.v1` 开头，每个任务用 `# task <id>` 标记，blob 上限为
4096 字节，最多 4 个任务、每个任务最多 32 条记录。任何越界、不在白名单
内的路径、JSON body 非法或方法不是 `PUT` 的记录，都会在写入前被
`invalid_blob` 拒绝，从而保证下一次 `GET /api/v1/tasks` 返回的内容
与存储内容逐字节相等。

执行行为——`task run <task-id> --confirm` 采用 fail-closed 契约：缺少
`--confirm` 时在任何板卡请求前返回 `confirmation_required`。确认后首先
通过 `GET /api/v1/tasks/catalog` 取回固件目录，并先于任何 stored task
解析 built-in；若目录不可用且目标 id 以 `builtin/` 开头，CLI 直接返回
`catalog_unavailable`，不会回退到被 shadow 的 stored task。命中 built-in
时直接执行且不访问 `/api/v1/tasks`，否则通过 `GET /api/v1/tasks` 下载
当前 blob，按 id 选中 stored task，再通过普通 HTTP API 按顺序派发每条
`PUT` 记录。记录之间，客户端**仅在响应成功之后**按记录的 `wait_ms` sleep。
失败（例如引脚未知或返回 4xx）会立即停止：失败之后的记录**不会**再派发，
CLI 以非零状态退出。JSON 失败字段使用 `error.record_index`（1-based）
和 `error.path`（`error` 对象下 snake_case）；非 JSON 失败把
`task record <n> path "<path>" failed: <message>` 写到 stderr。每条
请求的 wire `body` 都是存储记录的 `body` 字段原文；`wait_ms` 永远不会到达
固件。若已存任务使用固件目录声明的 built-in ID，`task list` 会把它标记
为被遮蔽，`task run` 选择不可变的 built-in；`task clear` 仍会删除该
stored entry，但不会影响目录条目。JSON 列表的 `task_count` 是合并后的
可见数量，`stored_task_count` 单独报告固件条目数，每行带 `source`，
并附 `catalog_available` 与 `catalog_error` 报告目录获取结果。
Ctrl+C 会在请求之间或 `wait_ms` 期间协作取消。built-in cleanup 来自目录
声明，作为主失败/取消之外的独立结果报告，cleanup 失败不会覆盖主错误；
首条请求前取消不执行 cleanup。

开机行为——固件**不再**存储或执行任何开机任务。已存任务保持静默，
直到 CLI 或 Web UI 显式运行它们。旧开发期数据（旧任务存储标记或
settings key 下写入的任何内容）由新版契约定向失效：新版本固定为
`# linkr-task.v1` 标记与 `linkr/task/tasks` 键。没有迁移、没有读取
别名、也没有针对陈旧条目的复位路径；仍持有旧数据的板子只会显示空
任务契约。

```text
GET    /api/v1/tasks
PUT    /api/v1/tasks
DELETE /api/v1/tasks
```

固件目录中的 built-in 属于固件镜像，因此执行 `task clear` 后仍然存在。
显式保存的自定义任务通过 `linkr/task/tasks` 键跨普通重启与 combined-UF2
恢复保留，直到 `task clear` 删除；清除 stored task 不改变当前硬件状态。

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

`task show` 输出一行，包含 `available`（固件 Settings+NVS 后端是否
可达）和 `task_count`。`task clear` 先获取 capture 仲裁器，再获取
flash 仲裁器；如有任一处于忙状态（live capture 或 OTA 正在进行），
它返回 `task clear error=busy` 并以 `-EBUSY` 退出，**不会**删除
已存 blob。清除成功时输出 `task clear ok` 并以 0 退出。后端错误
（忙路径之后）则输出 `task clear error=storage_error` 并以 `-EIO`
退出。CDC shell 使用与 HTTP 层相同的固件枚举 ID，**不**维护主机
侧目录。这是 HTTP 或 WS 卡死时的 fallback，不改变 HTTP/CLI 表面。

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
