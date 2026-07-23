# CLI 参考

[English](cli.md)

`radxa-linkr-debuggerctl` 通过 USB NCM 与调试板通信。板子运行 DHCPv4 server，主机会自动获取兼容地址，无需额外配置。默认设备 URL 为 `http://172.29.203.1`，仅在需要覆盖时才传 `--url`。

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
