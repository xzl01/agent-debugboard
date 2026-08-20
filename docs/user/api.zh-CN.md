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

响应字段：`project`、`mcu`、`usb`、`power_inputs`、`power_outputs`、`switches`、`adc_channels`、`watchdog`、`board_monitoring`、`gpios`。

## 电源控制

### `GET /api/v1/power`

列出所有电源输出。

### `GET /api/v1/power/{name}`

获取单个电源输出状态。`{name}` 为 `12v_out`、`5v_out` 或 `20v_out`。

### `PUT /api/v1/power/{name}`

设置电源输出状态。

```sh
curl -X PUT http://172.29.203.1/api/v1/power/12v_out -d '{"state":"on"}'
```

请求体：`{"state": "on"}` 或 `{"state": "off"}`

错误：400（`missing_power_output`、`invalid_state`）、403（`power_output_locked`）、500（`set_failed`）

## ADC（电流监测）

### `GET /api/v1/adc/read`

读取电流监测 ADC 通道。

```sh
curl http://172.29.203.1/api/v1/adc/read
curl http://172.29.203.1/api/v1/adc/read?channel=5v_out
```

查询参数：`channel=<name>`（可选，如 `5v_out`、`12v_out`、`20v_out`）

响应 `readings` 数组字段：`name`、`signal`、`power_enabled`、`raw`、`mv`、`current_ua`、`sensor_value`、`unit`。

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

## Bootloader

### `POST /api/v1/bootloader`

进入 ROM BOOTSEL 模式。MCU 在响应后 250ms 复位。

```sh
curl -X POST http://172.29.203.1/api/v1/bootloader
```

## 目标设备恢复模式

### `GET /api/v1/target-recovery`

返回支持的恢复模式、电源轨、固定时序以及安全释放方向。

### `POST /api/v1/target-recovery`

在断电重启目标电源轨时使 `CON_MAS` 保持有效，然后将引脚释放为输入。Qualcomm EDL 为高电平有效，Rockchip MASKROM 为低电平有效。

```sh
curl -X POST http://172.29.203.1/api/v1/target-recovery \
  -d '{"mode":"rockchip-maskrom","rail":"5v_out"}'
```

有效模式：`qualcomm-edl`、`rockchip-maskrom`。有效电源轨：`5v_out`、`12v_out`、`20v_out`。固件会保持断电 1000 ms，上电前预置恢复信号 20 ms，上电后继续保持 500 ms，即使时序失败也会将 `CON_MAS` 释放为高阻输入。

## Watchdog

### `GET /api/v1/watchdog`

Watchdog 状态。字段：`supported`、`automatic`、`healthy`、`armed`、`timeout_ms`、`bootloader_on_timeout`、`failing_service`。

## OTA 固件更新

### `GET /api/v1/ota`

OTA 状态。字段：`state`（`idle`/`uploading`/`verified`/`pending_test`/`rebooting`/`failed`）、`expected_size`、`written_size`、`max_size`、`swap_type`、`current_image_confirmed`、`test_marker_present`。在受控测试中，手动确认或看门狗门控自动确认后标记会清除；若 MCUboot 回滚测试镜像，标记会保留。

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
- `rate_hz`：1–1000（默认 10）
- `batch_size`：1–20（>1 启用批处理模式）

**取消订阅**：
```json
{"type": "unsubscribe"}
```

**命令**：
```json
{"type": "command", "command": "power_set", "id": "1", "output": "12v_out", "state": "on"}
```

可用命令：`power_set`、`switch_route`、`gpio_set`、`target_recovery`、`bootloader`、`capture_arm`、`capture_trigger`、`capture_stop`、`capture_cancel`。

功耗捕获必须显式指定上位机流式协议模式：

```json
{"type":"command","command":"capture_arm","id":"capture-1","mode":"host-stream-v1","trigger":"current","output":"5v_out","threshold_ua":500000,"rate_hz":100}
```

布防前应将 `mode` 与状态响应中的 `power_capture_protocol` 对比，避免固件与 Web
版本不一致时无限等待。

目标设备恢复命令：

```json
{"type":"command","command":"target_recovery","id":"2","mode":"qualcomm-edl","output":"5v_out"}
```

### 服务端 → 客户端消息

**状态快照**（状态变化时推送）：
```json
{"type": "snapshot", "topic": "status", "power_capture_protocol": "host-stream-v1", "sequence": 1, "power_outputs": [...], "switches": {...}, "watchdog": {...}, "gpios": [...], "board_monitoring": {...}}
```

**ADC 遥测**（按订阅速率推送）：
```json
{"type": "telemetry", "topic": "adc", "sequence": 1, "uptime_us": 12345, "readings": [...]}
```

**批处理遥测**（`batch_size > 1` 时）：
```json
{"type": "telemetry-batch", "topic": "adc", "channels": [...], "samples": [...]}
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
| 403 | `power_output_locked`、`not_allowed` | 禁止 |
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
