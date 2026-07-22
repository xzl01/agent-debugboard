# CLI 参考

[English](cli.md)

## NCM 网络接口

固件枚举为复合 USB 设备。CLI 通过 USB NCM 接口上的 HTTP 与调试板通信，默认设备 URL 为 `http://172.29.203.1`。调试板在 NCM 链路上运行 DHCPv4 server，host 自动获取兼容地址。仅在需要覆盖默认地址时才使用 `--url <URL>`。

```sh
radxa-linkr-debuggerctl --url http://192.168.1.100 status
```

## 基本命令

查询调试板状态和运行诊断：

```sh
radxa-linkr-debuggerctl status
radxa-linkr-debuggerctl doctor
```

## JSON 输出

Agent 或自动化程序推荐优先使用 JSON 输出。JSON 响应使用标准信封格式：

- `schema`：`"radxa-linkr-debugger.v1"`
- `ok`：布尔值，表示成功或失败
- `command`：执行的命令
- `error`：失败时为 `{code, message}`

```sh
radxa-linkr-debuggerctl --json doctor
radxa-linkr-debuggerctl --json status
radxa-linkr-debuggerctl --json power list
radxa-linkr-debuggerctl --json adc read
radxa-linkr-debuggerctl --json gpio list
radxa-linkr-debuggerctl --json watchdog status
```

## 电源控制

控制三个电源输出：

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

读取电流监测 ADC 通道：

```sh
radxa-linkr-debuggerctl adc read
radxa-linkr-debuggerctl adc read 5v_out
radxa-linkr-debuggerctl adc read 12v_out
radxa-linkr-debuggerctl adc read 20v_out
```

人类可读输出默认保持简洁（如 `5v_out=0.540000A`）。需要调试字段时使用 `-v` / `--verbose`，会额外输出 `signal`、`mv` 等信息。

### 录制

`adc record` 创建 live websocket session 并将遥测数据录制到文件：

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

### 列表和查询

```sh
radxa-linkr-debuggerctl switch list
radxa-linkr-debuggerctl switch get sd
radxa-linkr-debuggerctl switch get usb
radxa-linkr-debuggerctl switch get vin
```

### SD switch

在目标板和 USB 读卡器之间路由 TF/SD 卡：

```sh
radxa-linkr-debuggerctl switch route sd target
radxa-linkr-debuggerctl switch route sd usb-reader
```

### USB switch

在 PC 和目标板之间路由 USB：

```sh
radxa-linkr-debuggerctl switch route usb pc --confirm
radxa-linkr-debuggerctl switch route usb target --confirm
```

### VIN 控制

选择 CH347 VIO 电压电平：

```sh
radxa-linkr-debuggerctl switch route vin 3.3v --confirm
radxa-linkr-debuggerctl switch route vin 1.8v --confirm
```

VIN 启动默认值为 3.3V。切换到 1.8V 属于专家操作：必须先确认外接目标支持 1.8V 信号电平，连接 VIO 物理测量设备，并明确接受硬件副作用。

## GPIO

### 列出 GPIO

```sh
radxa-linkr-debuggerctl gpio list
```

### 设置 GPIO 输出

```sh
radxa-linkr-debuggerctl gpio set GP13 1
radxa-linkr-debuggerctl gpio set GP13 0
radxa-linkr-debuggerctl gpio set CON_MAS 1
```

### 读取 GPIO 输入

```sh
radxa-linkr-debuggerctl gpio input GP13
radxa-linkr-debuggerctl gpio input J16_PIN1
```

### GPIO 命名

GPIO 名称支持三种格式：
- `GPxx` — 标准 MCU 引脚名（如 `GP13`）
- 数字引脚 — 原始 MCU 引脚号（如 `4`）
- 板级 note — 原理图标签（如 `CON_MAS`、`J16_PIN1`）

CLI/TUI 同时显示 `GPxx` 和板级 `note`。

## Watchdog

```sh
radxa-linkr-debuggerctl watchdog status
```

watchdog 由固件自身管理，不由主机喂狗。固件自动 arm MCU 硬件 watchdog，仅在核心固件循环、HTTP/API 服务和 CDC ACM cmdline fallback 都持续上报健康存活时才继续喂狗。如果任何一项停止响应，固件停止喂狗，MCU 复位，下次启动通过 retained recovery marker 进入 ROM BOOTSEL。

## OTA 固件更新

```sh
radxa-linkr-debuggerctl ota status
radxa-linkr-debuggerctl ota upload /path/to/firmware.bin
radxa-linkr-debuggerctl ota test
radxa-linkr-debuggerctl ota confirm
```

- `ota status` 报告当前 OTA 状态（`idle`、`uploading`、`verified`、`pending_test`、`rebooting`、`failed`）、flash 大小和 MCUboot swap type
- `ota upload` 发送 MCUboot 格式 `.bin` 文件，含 SHA256 完整性校验
- `ota test` 请求对已验证镜像执行测试启动
- `ota confirm` 立即手动确认当前运行镜像

OTA 接收 MCUboot 格式应用二进制文件。不要上传 `.uf2` 或 `.elf` 文件。使用 release 产物 `radxa-linkr-debugger-rp2350-ota.bin`。

## 板级自监控

状态 JSON 包含 `board_monitoring`，含以下类别：

| 类别 | 字段 |
| --- | --- |
| `temperature` | CPU die 温度传感器读数 |
| `heap` | 系统堆运行时统计 |
| `memory` | 加法式压力对象（见下方） |
| `runtime` | 板端 uptime（`uptime_ms` / `uptime_seconds`） |
| `cpu` | CPU 利用率 delta |

每个类别携带 `available`（布尔值）和 `reason`（字符串）。固件仅在 Zephyr 设备或 runtime stats API 已启用且可读取时报告真实数值。

### 内存压力字段

`memory` 类别包含三个压力报告字段：

- `pressure_pct_x100`（遗留）— `max(current system heap %, highest thread stack high-water %)`
- `current_pressure` — 加法式对象：`max(current heap %, RX packet slab %, TX packet slab %, RX data buffer pool %, TX data buffer pool %)`。可动态升降。
- `peak_pressure` — 启动生命周期加法式对象，覆盖范围与 `current_pressure` 相同，额外包含 thread stack high-water 和 `since: "boot"`。

`current_pressure` 和 `peak_pressure` 均包含：
- `pressure_pct_x100`，范围 0..10000
- `limiting_component` — 驱动最大值的组件
- `limiting_name` — 实例名称
- `tie_count` — 多个组件共享最大值时的数量

## 状态 LED

RP2350A 板上 GPIO25 的蓝色状态 LED 作为 watchdog 心跳指示灯。它约每秒闪烁一次，仅在硬件 watchdog 喂狗成功后推进。跳过或失败的喂狗会将 LED 重置为非活动状态。
