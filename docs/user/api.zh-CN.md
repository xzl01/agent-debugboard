[English](api.md)

# HTTP API 参考

基础 URL：`http://172.29.203.1`（USB NCM 接口，端口 80）

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

## 状态

### `GET /api/v1/status`

完整板级状态快照，包含电源输出、switch、ADC 通道、watchdog、板级监控和 GPIO。

```sh
curl http://172.29.203.1/api/v1/status
```

响应字段：`project`、`build`（包含 `profile` 与 `image_version`）、`mcu`、`usb`、`power_inputs`、`power_outputs`、`switches`、`adc_channels`、`watchdog`、`board_monitoring`、`gpio_layout_groups`、`gpios`。

编码后的 status body 必须严格小于固件专用的 6144 字节缓冲区。新增状态字段时
必须通过这一协议大小门槛；客户端不能因此假定响应具有固定长度。

## 电源控制

### `GET /api/v1/power`

列出所有电源输出。

### `GET /api/v1/power/{name}`

获取单个电源输出状态。`{name}` 为 `12v_out`、`5v_out`、`20v_out` 或 `vdd_5v`。

### `PUT /api/v1/power/{name}`

设置电源输出状态。

```sh
curl -X PUT http://172.29.203.1/api/v1/power/12v_out -d '{"state":"on"}'
```

请求体：`{"state": "on"}` 或 `{"state": "off"}`

错误：400（`missing_power_output`、`invalid_state`）、403（`power_output_locked`）、500（`set_failed`）

## ADC（电流与电压监测）

### `GET /api/v1/adc/read`

读取 ADC 电流与电压通道。

```sh
curl http://172.29.203.1/api/v1/adc/read
curl http://172.29.203.1/api/v1/adc/read?channel=5v_out
```

查询参数：`channel=<name>`（可选，如 `5v_out`、`12v_out`、`20v_out`、`vdd_5v`、`adc3`）

四路 INA226 电流通道为 `5v_out`、`12v_out`、`20v_out`、`vdd_5v`；`adc3`
是独立的仅监测 ADC 电压通道。INA226 响应 `readings` 数组字段：`name`、
`signal`、`power_enabled`、`raw`、`mv`、`current_ua`、`voltage_valid`、
`voltage_uv`、`sensor_value`、`unit`。`adc3` 携带 `name`、`signal`、`raw`、
`mv`、`sensor_channel`、`unit`、`sensor_value`。

## Switch 路由

### `GET /api/v1/switch`

列出所有 switch 路由。

### `GET /api/v1/switch/{name}`

获取单个 switch 路由。`{name}` 为 `sd`、`usb` 或 `vin`。

### `PUT /api/v1/switch/{name}`

设置 switch 路由。

```sh
curl -X PUT http://172.29.203.1/api/v1/switch/sd -d '{"route":"usb-reader"}'
curl -X PUT http://172.29.203.1/api/v1/switch/vin -d '{"route":"1.8v"}'
```

有效路由：
- `sd`：`target`、`usb-reader`
- `usb`：`pc`、`target`
- `vin`：`1.8v`、`3.3v`

`usb` switch 对应 J12：上层连接目标板，下层插入 USB 设备；`pc` / `target`
用于选择由主机还是目标板控制该设备。

## GPIO

### `GET /api/v1/gpio`

列出所有安全 GPIO。响应包含 `reserved` 字段，列出不可用引脚。

### `GET /api/v1/gpio/{identifier}`

获取单个 GPIO 状态。`{identifier}` 接受 `GPxx`、原始引脚号或板级 note（如 `CON_MAS`）。

### `PUT /api/v1/gpio/{identifier}`

配置 GPIO。

```sh
# 设置输出高电平
curl -X PUT http://172.29.203.1/api/v1/gpio/GP13 -d '{"direction":"output","value":1}'

# 切回输入模式
curl -X PUT http://172.29.203.1/api/v1/gpio/GP13 -d '{"direction":"input"}'
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
GP15 `(6,0)`、GP16 `(5,0)`、GP29/ADC3 `(4,0)`；J13 GP9 `(0,0)`、
GP8 `(0,1)`、GP7 `(1,0)`。布局字段是可选的：safe GPIO GP17、GP18 尚无已确认
的 J8/J13 位置，因此固件不输出它们的 `layoutGroup`、`layoutRow`、
`layoutColumn`；客户端必须通过通用 fallback 保留显示。客户端也不得丢弃元数据中
未描述的 group；遇到未知 `edge` 时应按“无板边对齐提示”通用显示。这里的 GPIO
数组只是固件 safe-GPIO 允许列表，不是连接器完整 pinout，也不是配合面视图。

## 重启

### `POST /api/v1/reboot`

请求正常冷重启。MCU 在响应后 250 ms 执行，这条路径绝不请求 ROM BOOTSEL。
固件在发送响应前已准备好计划重启状态，因此成功响应保证：只要复位期间没有
按住物理 BOOTSEL 按键，下一次启动就回到正常固件，而不是 ROM USB boot 路径。

```sh
curl -X POST http://172.29.203.1/api/v1/reboot
```

`5v_out`、`12v_out`、`20v_out` 中的每一条都回到自己的
`regulator-boot-off` 默认状态，除非已保存的 v1 配置快照恢复了该电源轨；
`vdd_5v` 跟随 USB 路由默认值。板子恢复后必须重新设置并验证电源轨与 PD。

受支持的变更方法是 `POST`。`GET`、`PUT`、`DELETE` 返回 HTTP 405，
`error.code` 为 `method_not_allowed`，且不会调度复位。重启已处于 pending 时
重复 `POST` 返回 HTTP 409、`error.code=reboot_pending`，不会调度第二次或
更早的复位。

## Bootloader

### `POST /api/v1/bootloader`

进入 ROM BOOTSEL 模式。这不是正常重启，也不是 `POST /api/v1/reboot` 的
别名。MCU 在响应后 250 ms 复位。

```sh
curl -X POST http://172.29.203.1/api/v1/bootloader
```

ROM BOOTSEL 只能刷写合成完整 UF2 `radxa-linkr-debugger-rp2350.uf2`
（MCUboot + 应用）。绝不能使用仅含应用固件的 `zephyr.uf2`，它不包含
MCUboot，可能导致板子无法正常启动。

## 任务（固件目录与保存的请求序列）

固件 API 只负责把通用任务 blob 存到 flash，不执行、重放或自动植入任务。
固件同时在 `GET /api/v1/tasks/catalog` 上提供不可变的内置任务目录；
CLI 与 Web UI 严格消费该目录，并通过已有 power、GPIO、switch API 派发所
选任务。host 自身不持有任何恢复配方。当前目录提供六个 MASKROM/EDL 条目，
stored entry 走独立路径，永远不会出现在 `GET /api/v1/tasks/catalog` 中。

### `GET /api/v1/tasks/catalog`

返回固件拥有的不可变内置目录。host 校验响应 envelope（`schema`、`ok`、
`command: "task"`、`action: "catalog"`、`version: 1`）并拒绝未知字段。
每个目录条目声明 id、显示名、要派发到白名单路径
`/api/v1/power`、`/api/v1/gpio`、`/api/v1/switch` 的 `PUT` 记录列表，
以及一个强制的 cleanup 序列。host 使用与 stored task 相同的通用任务
runner 派发目录条目。

```sh
curl -fsS http://172.29.203.1/api/v1/tasks/catalog
```

### 冻结的边界

| 边界 | 值 |
|---|---|
| 每个 blob 的任务数 | 4 |
| 每个任务的请求数 | 32 |
| blob 字节数 | 4096 |
| 任务 id 字节数 | 31 |
| 任务名称字节数 | 63 |
| 请求行字节数 | 256 |
| 路径字节数 | 96 |
| body 字节数 | 192 |
| `wait_ms` 范围 | 0 到 60000 |

### 存储布局

任务保存在 `Settings+NVS` 的 `linkr/task/tasks` 键下，blob 始终以
字面量 `# linkr-task.v1` 开头。每个任务使用 `# task <id>` 标记，后接一条
或多条 NDJSON 请求记录。典型记录：

```json
{"method":"PUT","path":"/api/v1/power/12v_out","body":"{\"state\":\"off\"}","wait_ms":1000}
```

同一个任务的第二条记录会写在另一行 NDJSON：

```json
{"method":"PUT","path":"/api/v1/gpio/CON_MAS","body":"{\"direction\":\"input\"}","wait_ms":0}
```

完整文件形如：

```text
# linkr-task.v1
# task power-cycle
{"method":"PUT","path":"/api/v1/gpio/GP13","body":"{\"direction\":\"input\"}","wait_ms":0}
{"method":"PUT","path":"/api/v1/power/12v_out","body":"{\"state\":\"off\"}","wait_ms":1000}
```

每条记录必须使用 `method:"PUT"`、JSON 字符串形式的 `body`，且 `path`
只能位于 `/api/v1/power/`、`/api/v1/gpio/` 或 `/api/v1/switch/` 之下。
`wait_ms` 缺省值为 0，必须为 0 到 60000 的整数。blob、版本标记、路径白
名单以及上表中的边界都在解析时校验；任何违规记录都会返回
`invalid_blob`。小于 `0x20` 且不是 tab/LF/CR 的字节在写入前会被拒绝，
保证 GET 返回的 JSON 始终能够逐字节还原已存 blob。

任务 ID 必须是非空 UTF-8 字符串，最长 31 字节。空格、tab、CR、LF、`#`、
保留 ID `catalog` 以及精确的 `.`、`..` 均非法；`PUT` 遇到含有这些非法 ID
的 blob 时返回 HTTP 400 `invalid_blob`。`.hidden`、`...` 等其他含点 ID 仍合法。

旧开发期数据（任何写入先前任务存储标记或 settings key 的内容）由新版
任务存储契约定向失效：新版本固定为 `# linkr-task.v1` 标记与
`linkr/task/tasks` 键。既没有迁移路径，也不提供读取别名；仍持有旧数据
的板子在升级后只会显示空任务契约。

### `GET /api/v1/tasks`

返回任务状态和精确存储的 blob。空板返回 `tasks: []` 和 `blob: ""`，
HTTP 状态仍是 200，响应与其他端点使用相同信封：

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": true,
  "command": "task",
  "action": "list",
  "backend": { "available": true },
  "task_count": 1,
  "tasks": [
    { "id": "power-cycle", "name": "power-cycle", "request_count": 3 }
  ],
  "blob": "# linkr-task.v1\n# task power-cycle\n{\"method\":\"PUT\",\"path\":\"/api/v1/gpio/CON_MAS\",\"body\":\"{\\\"direction\\\":\\\"input\\\"}\",\"wait_ms\":0}\n{\"method\":\"PUT\",\"path\":\"/api/v1/power/12v_out\",\"body\":\"{\\\"state\\\":\\\"off\\\"}\",\"wait_ms\":1000}\n{\"method\":\"PUT\",\"path\":\"/api/v1/power/12v_out\",\"body\":\"{\\\"state\\\":\\\"on\\\"}\",\"wait_ms\":0}\n"
}
```

固定字段：`schema`、`ok`、`command:"task"`、`action:"list"`、`backend`
（`{available}`）、`task_count`（数字）、`tasks[]`（`id`、`name`、
`request_count`）、`blob`（字符串，逐字节存储内容）。已存任务未设置
`name` 时，`name` 默认等于 `id`。响应容量按照 `2 * 4096` 加上有限
envelope 与摘要开销计算；超出容量时返回 `response_too_large`，**不会**
截断。GET 处理器不获取采集或 flash 仲裁器。

### `PUT /api/v1/tasks`

替换已存储的 blob。请求体为完整的 `linkr-task.v1` 文本；同时接受
JSON 字符串字面量（`"..."`），服务端会先反转义标准转义再校验。该端点
需要获取共享 mutation helper，若采集或 OTA 正在运行，会返回 `busy`
（HTTP 409），不会写入。blob 超过 4096 字节返回 `body_too_large`
（HTTP 413）。成功响应：

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": true,
  "command": "task",
  "action": "store",
  "stored": true
}
```

### `DELETE /api/v1/tasks`

清空已存储的 blob，不影响当前硬件状态。先获取共享 mutation helper，
采集或 OTA 进行中时返回 `busy`（HTTP 409）。成功响应：

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": true,
  "command": "task",
  "action": "clear",
  "cleared": true
}
```

### `DELETE /api/v1/tasks/{id}`

仅删除指定的已存任务，其他任务段逐字节保留，不改变当前硬件状态。
删除最后一个任务时移除存储键。将 ID 按 UTF-8 百分号编码为单个 URL 路径
组件，例如 `café` 编码为 `caf%C3%A9`、`/` 编码为 `%2F`、字面量 `%`
编码为 `%25`。服务端仅解码一次；解码后的 NUL（`%00`）会被拒绝，
不会截断 ID。解码后精确为 `.`、`..` 的 ID 非法，包括 `%2E`、`%2E%2E`。

| HTTP 状态 | 结果 |
| --- | --- |
| 200 | 任务已删除，响应见下方 |
| 400 | ID 缺失或非法、百分号解码失败，或解码出 NUL；不删除任何任务 |
| 404 | `not_found`：没有与合法 ID 匹配的已存任务 |
| 409 | `busy`：采集或 OTA 占用共享 mutation 资源；不删除任何任务 |

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": true,
  "command": "task",
  "action": "delete",
  "id": "power-cycle",
  "deleted": true
}
```

### 方法处理

`/api/v1/tasks` 仅接受 GET、PUT、DELETE；其他动词返回 `method_not_allowed`
（HTTP 405）。端点本身不会重放记录，固件契约就是“只存不执行”；执行由
客户端负责。

`/api/v1/tasks/{id}` 仅接受 DELETE，其他方法返回 HTTP 405。

### 客户端执行与边界

CLI 与 Web UI 通过 `GET /api/v1/tasks` 取回 blob，按 id 选中一个任务，
再按顺序把每条记录派发到普通 HTTP API。每条记录请求成功后，客户端再根据
记录中的 `wait_ms` 在本地睡眠；`wait_ms` 只是任务元数据，**不会**进入
传给 power、GPIO 或 switch 端点的请求体。首次失败即停止执行。失败之后的
记录绝不再派发，发送给板子的 wire `body` 也不会包含 `wait_ms`。

CLI 与 Web 页面在各自的输出层上报失败信息，**不会**出现在共享 HTTP
信封里。CLI JSON 失败字段使用 `error.record_index`（1-based）和
`error.path`（`error` 对象下 snake_case），并在 `task` action 上以非零
退出码结束。CLI 非 JSON 失败则把
`task record <n> path "<path>" failed: <message>` 写到 stderr。Web 页面
返回内部结果，包含 `failedIndex`（1-based）和 `failedPath`
（camelCase）以及解析/网络错误字符串。

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
curl -X POST http://172.29.203.1/api/v1/ota/upload \
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
curl -X POST http://172.29.203.1/api/v1/live-sessions
```

响应：`session_id`、`ws_url`（`ws://172.29.203.1/api/v1/ws/<slot>`）、`connected`。

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

连接 `ws://172.29.203.1/api/v1/ws/{0|1|2|3}`（live-sessions 返回的 slot URL）。

最多支持 4 个并发客户端。

### 客户端 → 服务端消息

**订阅**（开始接收遥测）：
```json
{"type": "subscribe", "topic": "live", "rate_hz": 60, "batch_size": 1}
```
- `rate_hz`：1–250（默认 10）；大于 250 返回错误码 `invalid_rate`
- `batch_size`：1–20（>1 启用批处理模式）

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
