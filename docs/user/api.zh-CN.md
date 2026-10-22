[English](api.md)

# HTTP API 参考

基础 URL：主机 NCM `/30` 中的板卡 `.1` 地址（CDC shell `ip` 命令会打印）；示例使用 `$BOARD_URL`（USB NCM 接口，端口 80）。板卡首次启动会派生并持久化唯一私有 `/30`，见[网络身份](../reference/network.md)。

```sh
export BOARD_URL=http://<板卡 IP>
```

所有 JSON 响应包含 `"schema": "radxa-linkr-debugger.v1"` 和 `Cache-Control: no-store`。

## 响应信封

成功响应：

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": true,
  "command": "<command>"
}
```

失败响应（仅当 `ok` 为 `false` 时才包含 `error`）：

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": false,
  "command": "<command>",
  "error": { "code": "<code>", "message": "<message>" }
}
```

## 网络身份

### `GET /api/v1/net`

返回持久化的 MAC、来源（`derived` 或 `manual`）、私有 `/30` 网络、板卡地址、
DHCP 基址和板卡 URL。

```sh
curl -fsS "$BOARD_URL/api/v1/net"
```

### `PUT /api/v1/net`

更新 `mac`、`network` 或两者；省略的字段保持当前值。`mac` 必须是本地管理的
单播地址；`network` 必须是私有 `/30` 基址，可带 `/30` 后缀。手动值写入同一个
原子身份 blob 并优先于派生值。

```sh
curl -fsS -X PUT -H 'Content-Type: application/json' \
  -d '{"network":"10.20.30.0"}' "$BOARD_URL/api/v1/net"
curl -fsS -X PUT -H 'Content-Type: application/json' \
  -d '{"mac":"02:11:22:33:44:55"}' "$BOARD_URL/api/v1/net"
```

## 状态

### `GET /api/v1/status`

完整板级状态快照，包含电源输出、switch、ADC 通道、watchdog、板级监控和 GPIO。

```sh
curl $BOARD_URL/api/v1/status
```

响应字段：`project`、`build`（包含 `profile` 与 `image_version`）、`mcu`、`usb`、`power_inputs`、`power_outputs`、`switches`、`adc_channels`、`watchdog`、`board_monitoring`、`gpio_layout_groups`、`gpios`。

编码后的 status body 必须严格小于固件专用的 6656 字节缓冲区。新增状态字段时
必须通过这一协议大小门槛；客户端不能因此假定响应具有固定长度。

`power_outputs` 由固件动态公布。x1.2 上包含六路可控输出：`12v_out`、`5v_out`、
`20v_out`、`vdd_5v`、`5v_usb_hub` 和 `5v_otg`。`5v_usb_hub` 与 `5v_otg` 是
`vdd_5v` 的子轨，没有 INA226/ADC descriptor，也不会在既有 power-output 形状之外
新增状态字段。

## J4 目标串口

产品只公开一个逻辑目标串口 `j4`，即 GP10/GP11 上的专用 RP2350 UART1 路径。
固件把同一个 4096 字节 UART RX ring 独立 fan-out 到
USB CDC ACM4、四个 WebSocket sink 和两个 raw-TCP sink；慢 sink 只丢自己的
最旧数据。

### `GET /api/v1/serial/j4`

返回 available、运行状态、当前 line coding、配置 generation、DTR/RTS、USB
configured、sink 数、队列状态、传输计数、各 sink drop 计数，以及
`config_request` / `config_counters` 诊断。后者区分 CDC/Web request、
invalid/conflict/no-change、configure/apply 结果和 rollback 结果。`state` 为
`ready`、`reconfig`、`paused`、`faulted` 或 `unavailable`。

同一响应以 `terminal_responder` 报告全局自动终端 responder：对象如
`{"transport":"cdc","slot":0}` 或 `{"transport":"websocket","slot":2}`；
没有选中时为 `null`。DTR 有效时 CDC 优先，否则选择编号最小的已连接
WebSocket；raw TCP 永远不会被选中。

`dtr` 是已发布的 CDC/gate 状态，只有 promotion 完成后才为 true；它不是原始物理
DTR 采样。响应不公开内部 CDC phase 或 block reason 字段。

```sh
curl -fsS $BOARD_URL/api/v1/serial/j4
```

### `PUT /api/v1/serial/j4/config`

提交一次异步 line-coding compare-and-swap。先读取当前 `generation`，再提交完整
tuple 和该 generation：

```sh
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"baud":115200,"data_bits":8,"parity":"none","stop_bits":1,"flow_control":"none","expected_generation":1}' \
  $BOARD_URL/api/v1/serial/j4/config
```

成功返回 HTTP 202、`accepted:true` 和 `request_id`；generation 过期或转换进行中
返回 HTTP 409。baud 接受 4800 到 115200 的任意整数；五个推荐 preset 为
9600、19200、38400、57600、115200，5200 等 custom 值同样有效。超过 115200
的请求（包括 115201）返回 HTTP 400，当前 tuple 保持不变。data bits 为 5–8，
parity 为 `none`/`odd`/`even`，
stop bits 为 1 或 2，flow control 当前仅支持 `none`。

高于 115200 的 rate 不是产品支持范围。HIL 记录中的 230400/921600/1500000
样本是促成收紧上限的诊断证据，不是可接受的 client 配置。

应用是异步的，必须轮询 `GET /api/v1/serial/j4`；其中 current tuple、generation、
完成结果和诊断计数才是权威。USB CDC host 的 SET_LINE_CODING transfer 无法把稍后
发生的 application-level rejection 返回给 host，因此 CDC 工具也必须读取该状态，
不能假定 SET 已应用。

CDC DTR=false 时，USB default line-coding notification 只 stage 候选 tuple，不改变
active line coding 或 generation。此低态下的 HTTP/WS PUT 会立即应用；CDC session
已拥有端口（DTR high/promoting）时，HTTP/WS PUT 返回冲突而不是被 stage。LOW→HIGH
时板子先应用 staged USB tuple，之后才 publish `dtr=true` 并开放 ingress；apply/
attach/detach 失败会保持 blocked，直到成功的 LOW/reset。

### 流式传输

- 内嵌 Web 终端创建普通 live session，再订阅
  `{"type":"subscribe","topic":"serial.j4"}`。下行是 binary WS；每个上行
  binary message 是一条原子串口写，最大 480 字节。
- 显式 J4 输入是 multiwriter。只有自动终端回复使用唯一的 responder。WS
  `serial_state` 帧带有逐 client 的 `terminal_responder` 布尔值和 `responder`
  descriptor；只有显式 `true` 才授权该页面回复终端查询。其它 Web client 仍可
  发送键盘、IME、粘贴和 Ctrl-C 输入，它们是自动回复的 observer，不是只读。
  ANSI 序列不会被过滤。
- raw TCP 监听 `0.0.0.0:5555`，最多两个客户端。它是纯二进制流，没有 banner、
  echo、CR/LF 转换、Telnet/IAC、SSH 或 noVNC 语义，例如
  `nc <board-ip> 5555`。
- USB CDC ACM4 是同一个 J4 逻辑流的 USB fallback。主机 tty 编号不稳定，必须按
  interface 身份识别，不能假定固定 `/dev/ttyACM*`。

旧桌面本地串口路径已经退役，不属于产品传输。5173 仅用于 Vite 开发预览；
已验收的内嵌 UI 由板卡 port 80 提供。

## 电源控制

### `GET /api/v1/power`

列出所有电源输出。

### `GET /api/v1/power/{name}`

获取单个电源输出状态。`{name}` 为 `12v_out`、`5v_out`、`20v_out`、`vdd_5v`、
`5v_usb_hub` 或 `5v_otg`。

`5v_otg` 是 J3 双向 VBUS 开关（`OTG_USB_EN`，GPIO23），父轨为 `vdd_5v`。当前
`state`/`value` 表示请求/期望的使能状态；实际导通还需要 `vdd_5v`，且端口可经
Q10 被外部回灌。当前 API 尚未暴露 `effective`/`backfeed`，不得把 requested-on
当成已测得 5 V 输出。

`5v_usb_hub` 是 GPIO24 上 `vdd_5v` 的开关子轨，启动时关闭。其 setter 具有
父子语义：打开 hub 时按需先打开 `vdd_5v`；关闭 hub 不改变 `vdd_5v`；关闭
`vdd_5v` 会先关闭 hub；仅打开 `vdd_5v` 不会打开 hub。`5v_usb_hub` 出现在
status、`/api/v1/power` 和持久化配置目录中，但没有 INA226/ADC 通道。

### `PUT /api/v1/power/{name}`

设置电源输出状态。

```sh
curl -X PUT $BOARD_URL/api/v1/power/12v_out -d '{"state":"on"}'
```

请求体：`{"state": "on"}` 或 `{"state": "off"}`

错误：400（`missing_power_output`、`invalid_state`）、403（`power_output_locked`）、500（`set_failed`）

## ADC（电流与电压监测）

### `GET /api/v1/adc/read`

读取 ADC 电流与电压通道。

```sh
curl $BOARD_URL/api/v1/adc/read
curl $BOARD_URL/api/v1/adc/read?channel=5v_out
```

查询参数：`channel=<name>`（可选：`5v_out`、`12v_out`、`20v_out`、`vdd_5v`、
`adc0`、`adc1`、`adc2`、`adc3`）。

响应按固定顺序包含八路：先是 `5v_out`、`12v_out`、`20v_out`、`vdd_5v`
四路 INA226 电流通道，再是 `adc0`、`adc1`、`adc2`、`adc3`。其余电源输出
`5v_usb_hub` 与 `5v_otg` 没有 INA226，也没有 ADC descriptor；`vdd_5v` 是包含
hub 支路的父轨总量，客户端不得伪造 hub 或 OTG 电流读数。INA226 reading 携带 `name`、`signal`、
`power_enabled`、`raw`、`mv`、`current_ua`、`voltage_valid`、`voltage_uv`、
`sensor_value`、`unit`。

ADC reading 携带 `name`、`signal`、`role`、`raw`、`mv`、`sensor_channel`、
`unit`、`sensor_value`。`adc0` 的 `role` 为 `pd_input`，canonical
`sensor_value` 由固件按 Device Tree 的 R46/R47 分压以 64-bit 算术还原为
源侧 DC20V（115/15）；`raw` 和 `mv` 仍是 pad 侧诊断值。`adc1` 到 `adc3`
的 `role` 为 `native`，canonical 值就是 pad 电压。客户端不得自行执行
`115/15` 换算，也不得把 `raw`/`mv` 当作已还原的源侧测量。ADC0 精度未校准，
约为 2%；没有逐板校准时不得声称更高精度。

## Switch 路由

### `GET /api/v1/switch`

列出所有 switch 路由。

### `GET /api/v1/switch/{name}`

获取单个 switch 路由。`{name}` 为 `sd` 或 `tf_wp`。

### `PUT /api/v1/switch/{name}`

设置 switch 路由。

```sh
curl -X PUT $BOARD_URL/api/v1/switch/sd -d '{"route":"usb-reader"}'
curl -X PUT $BOARD_URL/api/v1/switch/tf_wp -d '{"route":"protected"}'
```

有效路由：
- `sd`：`target`、`usb-reader`
- `tf_wp`：`writable`、`protected`

x1.2 没有 `switch/usb`；GET/PUT `/api/v1/switch/usb` 返回 HTTP 404。
`vdd_5v` 与 switch 路由无关，`5v_usb_hub` 是它的电源子轨。

## GPIO

### `GET /api/v1/gpio`

列出所有安全 GPIO。响应包含 `reserved` 字段，列出不可用引脚。

### `GET /api/v1/gpio/{identifier}`

获取单个 GPIO 状态。`{identifier}` 接受 `GPxx`、原始引脚号或板级 note（如 `CON_MAS`）。

### `PUT /api/v1/gpio/{identifier}`

配置 GPIO。

```sh
# 设置输出高电平
curl -X PUT $BOARD_URL/api/v1/gpio/GP13 -d '{"direction":"output","value":1}'

# 切回输入模式
curl -X PUT $BOARD_URL/api/v1/gpio/GP13 -d '{"direction":"input"}'
```

`GET /api/v1/status`、`GET /api/v1/gpio` 列表响应和 WebSocket status snapshot
都携带以下顶层连接器元数据：

```json
{
  "gpio_layout_groups": [
    {"name": "J8", "edge": "right", "order": 0},
    {"name": "J13", "edge": "right", "order": 1}
  ]
}
```

`name` 与各 GPIO 的 `layoutGroup` 对应；`edge` 是板边位置提示，`order`
独立于 GPIO 数组顺序定义连接器顺序。对于已映射引脚，`layoutLabel`、
`layoutRow`、`layoutColumn` 描述元件面俯视坐标，顶部为 `row=0`、左列为
`column=0`。已确认坐标为：J8 GP12 `(9,0)`、GP13 `(8,0)`、GP14 `(7,0)`、
GP15 `(6,0)`、GP16 `(5,0)`、GP29/ADC3 `(4,0)`、GP5 `(4,1)`、GP6 `(5,1)`；
J13 GP9 `(0,0)`、GP8 `(0,1)`、GP7 `(1,0)`。GP5/GP6 是普通 RP2354 直连
GPIO，默认 input、无 pull，不是 switch 控件；GP17/GP18 不存在。未来固件若描述
未知 group，客户端仍不得丢弃；遇到未知 `edge` 时应按“无板边对齐提示”通用
显示。这里的 11 项 safe-GPIO 允许列表不是完整 pinout；其中 10 项可输出，
GP29 仅输入。

## 重启

### `POST /api/v1/reboot`

请求正常冷重启。MCU 在响应后 250 ms 执行，这条路径绝不请求 ROM BOOTSEL。
固件在发送响应前已准备好计划重启状态，因此成功响应保证：只要复位期间没有
按住物理 BOOTSEL 按键，下一次启动就回到正常固件，而不是 ROM USB boot 路径。

```sh
curl -X POST $BOARD_URL/api/v1/reboot
```

`5v_out`、`12v_out`、`20v_out`、`vdd_5v`、`5v_usb_hub`、`5v_otg` 中的每一条都回到
自己的未使能启动状态，除非已保存的 v1 配置快照恢复了该电源轨；`vdd_5v` 是父轨，
`5v_usb_hub` 是它的子轨且同样启动关闭。板子恢复后必须重新设置并验证电源轨与
PD。

受支持的变更方法是 `POST`。`GET`、`PUT`、`DELETE` 返回 HTTP 405，
`error.code` 为 `method_not_allowed`，且不会调度复位。重启已处于 pending 时
重复 `POST` 返回 HTTP 409、`error.code=reboot_pending`，不会调度第二次或
更早的复位。

## Bootloader

### `POST /api/v1/bootloader`

进入 ROM BOOTSEL 模式。这不是正常重启，也不是 `POST /api/v1/reboot` 的
别名。MCU 在响应后 250 ms 复位。

```sh
curl -X POST $BOARD_URL/api/v1/bootloader
```

ROM BOOTSEL 只能刷写合成完整 UF2 `radxa-linkr-debugger-rp2350.uf2`
（MCUboot + 应用）。绝不能使用仅含应用固件的 `zephyr.uf2`，它不包含
MCUboot，可能导致板子无法正常启动。

## 任务（固件目录与 task-v2 存储）

固件拥有两个独立任务来源。`GET /api/v1/tasks/catalog` 的不可变内置目录包含
MASKROM/EDL 配方，永不占用四个 task 槽。task-v2 存储最多保存四个操作员任务，
每个任务是 canonical `linkr-test.v1` source 加 `linkr-task.v1` deployment IR。
固件只校验和保存，不执行、重放或在开机时自动播种任务；CLI/Web 取回 IR 后按
普通 power、GPIO、switch 请求逐条派发。

### 内置目录

`GET /api/v1/tasks/catalog` 返回固件拥有的不可变目录。当前包含六条
MASKROM/EDL 条目，覆盖两种模式与 `5v_out`/`12v_out`/`20v_out`。目录条目没有
slot、store revision 或 source 文件，也不占 task-v2 槽。请求 `builtin/...` 而
目录不可用时必须 fail closed，不能回退到同 ID 的 stored task。

### Task-v2 存储模型

- 一个 Settings NVS 分区：**128 KiB**，`0x1B0000..0x1D0000`，保存 config
  snapshot 以及所有 task-v2 manifest/IR/source key；仍低于 2 MiB 内部 flash
  边界。分区大小受限，使首次 NVS 格式化和后续 GC 保持在 5 s 看门狗窗口内。
  预发布阶段的 320 KiB 布局不做迁移；从该布局升级需整片擦除/重刷。
- 四个 task slot。每个 board-backed task 的 exact canonical source 最多
  **4000 字节**；full-set IR 和 one-task IR section 也最多 **4000 字节**；
  单次 mutation body 最多 **12288 字节**。
- Task ID 为 1..63 个 UTF-8 字节，不能包含空白、控制字节、DEL 或 `#`。ID 是
  不透明标识，客户端不得从文本推断硬件行为。
- 每个 slot 有两个 source bank 和两个 full-set IR bank。manifest 保存活动
  bank、`task_store_id` UUID、uint64 revision、catalog fingerprint、full-IR
  digest 和每 slot 的 source/IR digest。
- mutation 先写 inactive bank、readback 并校验 digest，最后提交 manifest。
  manifest 写入是唯一 linearization point，掉电会保留旧 manifest。revision
  不回绕。
- `task_store_id` 在首次 manifest 时创建，clear 和普通重启都不改变；只有
  factory erase 才改变。
- 不读取或迁移已移除的 `linkr/task/tasks` Settings blob。Legacy task blob
  write 和 per-ID delete 路由不属于本契约，也不提供 alias。

Exact source bytes 是权威。`source_state:"present"` 的 board task 可由板子无损
重新打开。IR-only task、IR 已改变的 import task 和 built-in 没有无损 workflow
source。Web 可把超过 4000 字节的 source 只保存在浏览器 workflow library 并只
部署 IR；该 source 不在板上，新浏览器不可用。

### `GET /api/v1/tasks`

只返回 metadata，不返回 blob。字段为 `schema`、`ok`、`command:"tasks"`、
`action:"list"`、UUID 文本 `task_store_id`、十进制字符串 `revision`、64 个
小写 hex 的 `catalog_fingerprint`、`storage`、`full_ir_bytes`、
`full_ir_digest` 和 `tasks` 数组：

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": true,
  "command": "tasks",
  "action": "list",
  "task_store_id": "12345678-1234-4678-9abc-123456789abc",
  "revision": "7",
  "catalog_fingerprint": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "storage": {
    "total_bytes": 131072,
    "used_bytes": 5532,
    "free_bytes": 125540,
    "task_bytes": 680,
    "reserve_bytes": 8192,
    "measured": true,
    "save_allowed": true
  },
  "full_ir_bytes": 320,
  "full_ir_digest": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "tasks": [
    {
      "slot": 0,
      "id": "power-cycle",
      "request_count": 3,
      "source_state": "present",
      "source_bytes": 180,
      "source_digest": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
      "ir_bytes": 240,
      "task_ir_digest": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    }
  ]
}
```

`storage` 来自共享 Settings NVS。`used_bytes + free_bytes == total_bytes`；
`used_bytes` 包含 NVS 元数据和 GC 预留，`task_bytes` 是 manifest/source/IR 的
逻辑负载。`measured` 为 false 时表示固件无法读取 NVS 统计，`save_allowed` 仅作
提示。正常的 task save/import 还要求
`free_bytes >= reserve_bytes + 本次新增字节`，否则在写入前返回 HTTP 507
`storage_full`；`delete`/`clear` 不受门禁限制，以便释放空间。

`tasks[]` 只包含已占用 slot。present source 的 `source_bytes` 非零且带 SHA-256
`source_digest`；IR-only task 为 `source_state:"absent"`、`source_bytes:0`、
`source_digest:null`。不得为 built-in 或 IR-only task 伪造 source digest。
`task_ir_digest` 是 standalone blob 的 SHA-256：exact ASCII
`# linkr-task.v1\n` 加上 active full IR 中该任务的 exact section bytes，从
`# task <id>\n` 到该任务最后一条 request 的 LF。

### `GET /api/v1/tasks/ir`

返回 exact active full-set `linkr-task.v1` bytes，Content-Type 为
`application/vnd.radxa.linkr-task-ir.v1`。body 长度和 SHA-256 必须匹配
`GET /api/v1/tasks` 的 `full_ir_bytes`/`full_ir_digest`；空 store 返回空 body。
raw response 由 HTTP transaction 持有到完成。

### `GET /api/v1/tasks/slots/{0..3}/source`

返回该已占用 slot 的 exact canonical source，Content-Type 为
`application/vnd.radxa.linkr-test.v1+ndjson`；SHA-256 必须匹配
`source_digest`。没有无损 source 或 slot 不存在时返回 HTTP 404
`task_source_absent`。

客户端应先读 metadata、取 raw bytes、校验 digest；若 revision/digest 变化，
从 metadata 读起重试一次。

### `POST /api/v1/tasks/mutations`

所有 task-v2 写入使用同一个 JSON mutation 端点，必须发送
`Content-Type: application/json`。body 是 UTF-8 JSON 对象，包含字符串字段
`schema`（`radxa-linkr-debugger.v1`）、`action`
（`upsert`/`delete`/`clear`/`import`）、`expected_revision`、
`catalog_fingerprint`，以及 action 可选的 `id`、`source_base64`、
`ir_base64`。字段顺序固定。

`source_base64` 和 `ir_base64` 是严格 canonical RFC 4648 base64（标准字母表、
精确零 pad bit、无 JSON 转义或空白），携带 exact source/IR bytes；其他编码
一律拒绝。整个 body 最多 **12288 字节**，足以容纳两个 4000 字节 payload 的
base64（各 5336 字符）和固定 envelope。

- **Upsert**：要求合法 ID 和 ID 匹配的 standalone one-task `linkr-task.v1` IR。
  source 可选；省略 source 会把该 slot 变成 IR-only 并清除旧 source link，不会
  保留旧 source。Web 只在 source <=4000 字节时发送 source。固件校验 source
  UTF-8/length/control-byte、IR syntax 和 ID，计算并保存两个 digest；不编译
  workflow。
- **Delete**：只带 ID，删除该 slot。
- **Clear**：无 payload，删除所有 slot；不改变 `task_store_id`。
- **Full-IR import**：只带 full-set IR，替换所有 slot。只有同 ID 且 task-IR
  digest 未变时保留 source；changed/removed task 丢失 source。空 import 非法，
  应使用 clear。

每个 mutation 在 store/flash-owner mutex 下校验 expected revision 和 catalog
fingerprint；冲突返回 HTTP 409 且零写入。inactive source/IR bank 先写入并
readback 校验，之后才提交 manifest。

成功响应使用标准 envelope，`action` 为 `upsert`/`delete`/`clear`/`import`，
并带新的 `task_store_id`、十进制 `revision`、`catalog_fingerprint`、`storage`
和受影响 slot 的 `task` 对象（clear/delete 可为 `null`）。task 对象字段与
`tasks[]` 条目相同。

稳定错误状态：

- `400`：`invalid_mutation`、`invalid_task`、`body_too_large`
- `404`：`task_source_absent`
- `409`：`revision_conflict`、`catalog_conflict`、`busy`
- `507`：`storage_full`（NVS 写入失败，或四个 task slot 已满）
- `500`：`readback_failed`、`storage_error`

manifest 或数据 key 读回校验失败会把 task backend 标记为不可用，固件不会自动
修复，且该状态下 `clear` 也会被拒绝。只能整片擦除/重刷恢复；这个未发布布局没有
板端修复路径。

`revision_conflict` 和 `catalog_conflict` 的错误响应带当前 revision 与 catalog
fingerprint。

### Task 写入表面

Task 写入只有 `POST /api/v1/tasks/mutations`。没有 `/api/v1/tasks` blob upload
或 per-ID write/delete 路由；unsupported method 返回 HTTP 405。唯一的清空操作
是 mutation opcode `3`（clear），唯一的单任务删除是 opcode `2`（delete）。
Read 路由只接受 GET，mutation 路由只接受 POST。

### 客户端执行

CLI/Web 读取 metadata 加 `/api/v1/tasks/ir`，按 ID 选一个任务，再通过公共 HTTP
API 顺序派发每条记录。每条记录都是 `/api/v1/power/`、`/api/v1/gpio/` 或
`/api/v1/switch/` 下的 `PUT`。`wait_ms` 在成功请求后由客户端 sleep，永不发送
给板子。首次失败即停止，后续记录不再派发。built-in 使用 catalog 提供的 cleanup；
stored task 不推断 cleanup。

CLI `task store` 会编译 workflow source 或接受 one-task IR，但不发送 source
payload，因此创建的是 IR-only board task。需要 board-backed 无损 source 时，
使用 Web workflow editor/API 的 source-bearing upsert。IR-only task 的
`GET /tasks/slots/{slot}/source` 和 CLI `task source` 返回
`task_source_absent`。

## Watchdog

### `GET /api/v1/watchdog`

Watchdog 状态。字段：`supported`、`automatic`、`healthy`、`armed`、`timeout_ms`、`bootloader_on_timeout`、`fault_injection_available`、`fault_injection_armed`、`failing_service`。故障注入标志总是返回，但下面 HIL endpoint 只在 fault overlay 构建中存在。

### `GET /api/v1/watchdog/fault`

仅 HIL 构建。返回 `action=fault_injection`、`available` 和 `armed`。

### `POST /api/v1/watchdog/fault`

仅 HIL 构建。启用有界的 watchdog 故障注入。在 `POST /api/v1/ota/test` 设置 retained marker 之前，固件返回 HTTP 409 且 `error.code=fault_injection_rejected`；如果已经 arm，则返回 HTTP 409 且 `error.code=already_armed`；若 OTA confirm 正持有 marker 清除事务，则返回 HTTP 409 且 `error.code=confirm_in_progress`。arm 后停止喂狗，并会在 10 秒后自动 disarmed。

### `DELETE /api/v1/watchdog/fault`

仅 HIL 构建。在 deadline 前 disarmed 故障注入。

## OTA 固件更新

### `GET /api/v1/ota`

OTA 状态。字段：`state`（`idle`/`uploading`/`verified`/`pending_test`/`rebooting`/`failed`）、`expected_size`、`written_size`、`max_size`、`swap_type`、`current_image_confirmed`、`test_marker_present`。在受控测试中，手动确认或看门狗门控自动确认后标记会清除。MCUboot 回滚后标记会短暂保留，直到固件观察到当前运行镜像已确认，然后被清除，板子回到干净的 idle 状态。

### `POST /api/v1/ota/upload`

上传 MCUboot 格式固件二进制文件。

必需请求头：
- `Content-Type: application/octet-stream`
- `X-Linkr-Ota-Size: <byte_size>`
- `X-Linkr-Ota-Sha256: <hex_sha256>`（64 个十六进制字符）

```sh
FIRMWARE=path/to/firmware.bin
SIZE=$(stat -c%s "$FIRMWARE" 2>/dev/null || stat -f%z "$FIRMWARE")
SHA256=$(sha256sum "$FIRMWARE" 2>/dev/null | cut -d' ' -f1 || shasum -a 256 "$FIRMWARE" | cut -d' ' -f1)
curl -X POST $BOARD_URL/api/v1/ota/upload \
  -H "Content-Type: application/octet-stream" \
  -H "X-Linkr-Ota-Size: $SIZE" \
  -H "X-Linkr-Ota-Sha256: $SHA256" \
  --data-binary "@$FIRMWARE"
```

### `POST /api/v1/ota/test`

请求对已验证镜像执行测试启动。板子在约 750ms 延迟后重启。

### `POST /api/v1/ota/confirm`

立即确认当前运行镜像。

## Live Sessions（WebSocket）

### `POST /api/v1/live-sessions`

创建 WebSocket session。返回分配的 slot `ws_url`。

```sh
curl -X POST $BOARD_URL/api/v1/live-sessions
```

响应：`session_id`、`ws_url`（`ws://<board-ip>/api/v1/ws/<slot>`）、`connected`。

### `GET /api/v1/live-sessions/{id}`

按 ID 查询 session。

### `DELETE /api/v1/live-sessions/{id}`

删除 session。仍连接时返回 409。

## 逻辑分析仪

### `GET /api/v1/logic-analyzer`

当前状态：`idle`、`armed`、`capturing`、`done`、`error`。

### `POST /api/v1/logic-analyzer`

Arm 捕获。

```json
{
  "selected_pins": [13, 15],
  "sample_rate_hz": 1000000,
  "pre_samples": 0,
  "post_samples": 512,
  "trigger": "none",
  "trigger_pin": 0
}
```

约束：100,000–125,000,000 Hz 采样率；最多 512 总样本；最多 16 通道（安全引脚列表 GP7–GP9、GP10–GP20、GP29）；`pre_samples > 0` 需要边沿触发且 ≤25 MHz。

响应：`requestedSampleRateHz`、`actualSampleRateHz`、`samplePeriodPs`、`backend`。

### `GET /api/v1/logic-analyzer/capture`

状态为 `done` 后获取捕获数据。返回 `sampleCount`、`triggerIndex`、`config` 和 `samples` 数组。

### `DELETE /api/v1/logic-analyzer`

释放捕获资源。

## WebSocket 协议

连接 `ws://<board-ip>/api/v1/ws/{0|1|2|3}`（live-sessions 返回的 slot URL）。

最多支持 4 个并发客户端。

### 客户端 → 服务端消息

**订阅**（开始接收遥测）：
```json
{"type": "subscribe", "topic": "live", "rate_hz": 60, "batch_size": 1}
```
- `rate_hz`：1–250（默认 10）；大于 250 返回错误码 `invalid_rate`
- `batch_size`：1–18（>1 启用批处理模式）

**订阅 J4 串口**：
```json
{"type":"subscribe","topic":"serial.j4","id":"terminal-j4"}
```

同一个 WS slot 内，`serial.j4`、live ADC 和 sigrok 三种模式互斥。页面同时需要
终端和遥测时应分别创建 live session。串口模式中的 binary payload 只在 J4 ready
时准入。服务端先发 `serial_write_accepted`，仅在 UART 物理 wire complete 后发送
匹配的 `serial_write_complete`；runtime 取消时 disposition 为 `canceled`。队列满、
paused、reconfig、faulted 或超过 480 字节都会得到明确 JSON error，不会被误报为
accepted。

`serial_state` 同时标识自动终端 responder：`terminal_responder` 是接收 slot 的
布尔值，`responder` 是全局选中 descriptor。只有该 slot 回复自动 DSR/DA/CPR
查询；其它 slot 是可显式输入的 observer，不是只读 client。

**取消订阅**：
```json
{"type": "unsubscribe"}
```

**命令**：
```json
{"type": "command", "command": "power_set", "id": "1", "output": "12v_out", "state": "on"}
```

可用命令：`power_set`、`switch_route`、`gpio_set`、`reboot`、`bootloader`、`capture_arm`、`capture_trigger`、`capture_stop`、`capture_cancel`。保存的任务序列仅通过 HTTP 管理。

`reboot` 请求[重启](#重启)一节描述的正常冷重启，不是 `bootloader` 别名。
固件先发送 `reboot`/`ok` 结果帧，再等待 250 ms 执行重启。`bootloader` 仍是
显式的 ROM BOOTSEL 命令。

功耗捕获必须显式指定上位机流式协议模式：

```json
{"type":"command","command":"capture_arm","id":"capture-1","mode":"host-stream-v1","trigger":"current","output":"5v_out","threshold_ua":500000,"rate_hz":250}
```

布防前应将 `mode` 与状态响应中的 `power_capture_protocol` 对比，避免固件与 Web
版本不一致时无限等待。`rate_hz` 接受 1–250；省略时默认 100，大于 250 返回
错误码 `invalid_rate`。

GPIO 命令（包括直接控制恢复线）：

```json
{"type":"command","command":"gpio_set","id":"2","gpio":"CON_MAS","direction":"output","value":1}
```

对 ADC3 所有的 `GP29` 等仅输入 GPIO 发起输出请求时，`/api/v1/gpio/<id>`
返回 HTTP 403，`gpio_set` 返回错误帧。两种传输都使用 `error.code`
`input_only`，且 `error.message` 包含 `input-only`。其他 GPIO 配置失败使用
`configure_failed`。

WebSocket 不提供独立的 MASKROM/EDL 命令，也不提供通用 task-run 命令。
built-in 来自固件目录 `GET /api/v1/tasks/catalog`，客户端通过通用任务
runner 组合已有的普通 `gpio_set` 与 `power_set` 命令执行。

### 服务端 → 客户端消息

**状态快照**（状态变化时推送）：
```json
{"type": "snapshot", "topic": "status", "power_capture_protocol": "host-stream-v1", "sequence": 1, "power_outputs": [], "switches": {}, "watchdog": {}, "gpio_layout_groups": [{"name":"J8","edge":"right","order":0},{"name":"J13","edge":"right","order":1}], "gpios": [], "board_monitoring": {}}
```

**ADC 遥测**（按订阅速率推送）：
```json
{"type": "telemetry", "topic": "adc", "sequence": 1, "uptime_us": 12345, "readings": []}
```

**批处理遥测**（`batch_size > 1` 时）：
```json
{"type": "telemetry-batch", "topic": "adc", "channels": [], "samples": []}
```

每个 WebSocket 客户端由 256 点遥测环形缓冲支持。批处理消息包含
`dropped_samples`；非零表示客户端消费速度不足，长时上位机归档必须标记为
不完整。长时记录应持续保存这些遥测批次，而不是把完整数据累积在调试板捕获
RAM 中。

**命令结果**：
```json
{"type": "result", "command": "power_set", "id": "1", "ok": true}
```

J4 串口模式还会发送 `serial_state`、`serial_write_accepted`、
`serial_write_complete` 和 `serial_overrun`。`serial_overrun.dropped_bytes` 是相对
上一次通知的增量，`total_dropped_bytes` 是该 sink 的累计值；binary 下行帧就是
原始 UART 字节。

**捕获事件**：
```json
{"type":"capture_triggered","capture_id":1,"device_t_mono_us":123456,"sample_sequence":42,"dropped_samples":0}
```

固件只保存触发状态和元数据。原始样本持续通过 ADC 遥测发送，由上位机完成
缓冲和持久化。`dropped_samples` 是所属遥测游标从本次订阅开始累计的溢出数量。
`capture_stop` 释放已触发捕获；`capture_cancel` 解除触发器。

## 错误码

| HTTP 状态码 | Code | 含义 |
|---|---|---|
| 400 | `missing_power_output`、`invalid_state`、`invalid_config`、`invalid_route` | 请求错误 |
| 403 | `power_output_locked`、`not_allowed`、`input_only` | 禁止；`input_only` 表示 ADC3/GP29 由固件占用且仅允许输入 |
| 404 | `unknown_power_output`、`not_found`、`unknown_channel` | 未找到 |
| 405 | `method_not_allowed` | 方法不允许 |
| 409 | `already_armed`、`no_verified_image`、`upload_in_progress` | 冲突 |
| 413 | `body_too_large`、`image_too_large` | 负载过大 |
| 500 | `set_failed`、`read_failed`、`arm_failed`、`confirm_failed` | 内部错误 |
| 503 | `no_slots_available` | 无可用 WebSocket slot |

## 相关文档

- [CLI 参考](cli.zh-CN.md)
- [WebUI 指南](webui.zh-CN.md)
- [逻辑分析仪](logic-analyzer.zh-CN.md)
- [电源分析仪](power-analyzer.zh-CN.md)
- [OTA 固件更新](ota.zh-CN.md)
