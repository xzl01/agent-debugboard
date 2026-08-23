# CLI 参考

[English](cli.md)

`radxa-linkr-debuggerctl` 通过 USB NCM 与调试板通信。release 安装包还会提供
指向同一可执行文件的 `rdb` 链接，因此两个名称接受完全相同的命令。板子运行
DHCPv4 server，主机会自动获取兼容地址。默认设备 URL 为
`http://172.29.203.1`，仅在需要覆盖时才传 `--url`。

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

三条可控电源轨：`12v_out`、`5v_out`、`20v_out`。板内 VDD_5V 电源轨在 CLI 和 TUI 中不可见。

```sh
radxa-linkr-debuggerctl power set 12v_out on
radxa-linkr-debuggerctl power set 12v_out off
radxa-linkr-debuggerctl power set 5v_out on
radxa-linkr-debuggerctl power set 5v_out off
radxa-linkr-debuggerctl power set 20v_out on
```

列出所有电源输出：

```sh
radxa-linkr-debuggerctl power list
```

板内 VDD_5V 电源轨不会出现在 CLI 状态或电源控制中。

## ADC（电流监测）

每条电源轨都有对应的电流传感通道。默认输出简洁（`5v_out=0.540000A`），加 `-v` 可看到 `signal`、`mv` 等原始 ADC 字段。

```sh
radxa-linkr-debuggerctl adc read
radxa-linkr-debuggerctl adc read 5v_out
radxa-linkr-debuggerctl adc read 12v_out
```

### 录制

`adc record` 打开 websocket session 并将遥测数据写入文件：

```sh
radxa-linkr-debuggerctl adc record /tmp/adc.ndjson 1000 --rate-hz 250
radxa-linkr-debuggerctl adc record /tmp/adc.csv 1000 --rate-hz 250
```

- 输出格式由文件扩展名决定：`.ndjson` 或 `.csv`
- 默认订阅速率为 1000 Hz；使用 `--rate-hz HZ` 指定更低速率
- 请求速率高于 100 Hz 时固件使用 batch JSON；recorder 将每个设备样本展开为独立行
- 每条输出记录包含主机接收时间戳和 `metadata.requested_rate_hz`
- 设备时间保留在 `metadata.device_timing` 中
- 采样环覆盖通过 `metadata.dropped_samples` 在首个受影响行报告

### JSON ADC 输出

`--json adc read` 返回完整诊断链路：`raw`、`mv`、`current_ua`、`sensor_value` 和电源状态。主机 CLI 直接展示固件值，不做主机侧校准。

## Switch 路由

三个物理信号路由切换器：

| Switch | 切换内容 | 可选值 |
|--------|---------|--------|
| `sd` | TF/SD 卡路径 | `target`、`usb-reader` |
| `usb` | J12 下层 USB 设备在主机与目标板之间切换 | `pc`、`target` |
| `vin` | CH347 VIO 电压 | `3.3v`、`1.8v` |

```sh
radxa-linkr-debuggerctl switch list
radxa-linkr-debuggerctl switch get sd
radxa-linkr-debuggerctl switch route sd usb-reader
radxa-linkr-debuggerctl switch route usb target --confirm
radxa-linkr-debuggerctl switch route usb pc --confirm
```

USB 和 VIN 路由需要 `--confirm`，因为它们有可见的硬件副作用。VIN 启动默认 3.3V。切换到 1.8V 属于专家操作——先确认目标板支持 1.8V 信号电平，并连接 VIO 物理测量设备。

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

`test run` 执行 `linkr-test.v1` NDJSON 脚本。`--serial` 保留为 `--serial-uart0` 的兼容别名；脚本包含 UART1 步骤时必须用 `--serial-uart1` 明确指定设备路径，UART1 不会被静默路由到 UART0。`--output` 会将结果写入文件，扩展名决定格式：`.json`、`.csv` 或 `.ndjson`，其他扩展名默认使用 JSON。

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
built-in 成功结束时所选电源轨保持开启。该流程与进入调试板 RP2350
BOOTSEL 的 `bootloader` 无关。

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
