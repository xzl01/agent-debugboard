[English](logic-analyzer.md)

# 逻辑分析仪

RP2350 PIO2+DMA 高速单次 GPIO 捕获，用于 PIO 速率下的短突发诊断。
不适用于高采样率下的持续流式传输。

逻辑分析仪位于 Web UI 的 **Terminal 工作区**，与串口终端并列。

## 捕获 GPIO 引脚

以下引脚可用于逻辑分析仪捕获：

| 引脚 | x1.2 所有权 / 接入位置 |
| --- | --- |
| GP7–GP9 | J13 恢复控制（`CON_MAS`、`CON_REST`、`CON_USER`） |
| GP10–GP11 | J4 目标 UART；捕获期间逻辑分析仪临时接管 UART 引脚 |
| GP12–GP16 | J8 pin 1–5 |
| GP17–GP18 | 仅捕获lane；不属于safe/control，也没有J8/J13映射 |
| GP19 | PD CFG1 strap；仅采样内部信号，绝不能驱动 |
| GP20 | J8 pin 9，PIO I2C SCL；只读 tap，绝不能驱动或拉低 |
| GP29 | J8 pin 6，由 ADC3 所有且仅输入 |

不可使用其他 GPIO 引脚。特别是 GP21 为 J8 pin 10 / PIO I2C SDA，不属于
WIDE11 通道。此表是逻辑分析仪能力列表，不是物理连接器 pinout；连接探针前应以
固件 GPIO metadata 和 x1.2 板图为准。

## 配置参数

| 参数 | 范围 | 说明 |
| --- | --- | --- |
| 通道 | 1–16 | 从上述捕获允许列表中选择 |
| 采样率 | 100,000–125,000,000 Hz | 实际速率可能因 PIO 时钟分频器量化而略有差异 |
| post_samples | 1–512 | 触发后捕获的样本数 |
| pre_samples | 0–512 | 需要边沿触发且速率 ≤25 MHz；总数（pre+post）上限为 512 |

50 MHz 和 125 MHz 是非常短的单次突发。如需更长的捕获，请使用较低速率。

## 触发模式

| 模式 | 行为 |
| --- | --- |
| `none` | 自由运行捕获，无触发边沿 |
| `rising` | 低到高跳变时开始捕获 |
| `falling` | 高到低跳变时开始捕获 |
| `either` | 任何边沿跳变时开始捕获 |

### 预触发采样

设置 `pre_samples > 0` 并使用边沿触发（`rising`、`falling` 或 `either`），可
同时捕获触发边沿前后的样本。返回数据中触发索引位于 `pre_samples` 位置。

- 仅支持采样率 ≤25 MHz。
- `pre_samples > 0` 且 `trigger: "none"` 会被拒绝（HTTP 400）。
- `pre_samples > 0` 且速率 >25 MHz 会被拒绝（HTTP 400）。
- 总捕获量（`pre_samples + post_samples`）上限为 512 个样本。

## 捕获状态

分析仪按以下状态流转：

```
idle → armed → capturing → done
                         → error
```

## HTTP API

### 启动捕获

```
POST /api/v1/logic-analyzer
```

请求体示例：

```json
{
  "selected_pins": [13, 15],
  "sample_rate_hz": 1000000,
  "pre_samples": 0,
  "post_samples": 512,
  "trigger": "rising"
}
```

成功响应：

```json
{
  "schema": "radxa-linkr-debugger.v1",
  "ok": true,
  "command": "logic-analyzer",
  "action": "armed",
  "requestedSampleRateHz": 1000000,
  "actualSampleRateHz": 977600,
  "samplePeriodPs": 1024000,
  "backend": "rp2350-pio2-dma"
}
```

### 查询状态 / 获取捕获

```
GET /api/v1/logic-analyzer
```

返回当前状态（`idle`、`armed`、`capturing`、`done`、`error`）。

```
GET /api/v1/logic-analyzer/capture
```

返回已完成的捕获数据：

```json
{
  "state": "done",
  "sampleCount": 512,
  "triggerIndex": 127,
  "requestedSampleRateHz": 1000000,
  "actualSampleRateHz": 977600,
  "samplePeriodPs": 1024000,
  "backend": "rp2350-pio2-dma",
  "config": { ... },
  "samples": [
    {"timestampUs": 0, "values": 0},
    {"timestampUs": 1, "values": 3}
  ]
}
```

### 取消捕获

```
DELETE /api/v1/logic-analyzer
```

### 响应元数据

| 字段 | 说明 |
| --- | --- |
| `requestedSampleRateHz` | 配置中请求的采样率 |
| `actualSampleRateHz` | PIO 时钟分频器实际达到的采样率 |
| `samplePeriodPs` | 采样周期（皮秒） |
| `backend` | 捕获后端（`rp2350-pio2-dma`） |
| `sampleCount` | 捕获中的样本数 |
| `triggerIndex` | 触发样本的索引 |

### 错误码

| 错误码 | HTTP 状态码 | 原因 |
| --- | --- | --- |
| `already_armed` | 409 | 分析仪处于 `armed` 或 `capturing` 状态时发送 POST |
| `invalid_config` | 400 | 无效 JSON、参数越界、pre_samples 配合无边沿触发、或预触发捕获速率 >25 MHz |
| `arm_failed` | 500 | 固件内部故障 |

## Web UI

逻辑分析仪卡片位于 Web UI 的 **Terminal 工作区**。

### 波形预览

捕获的样本以 SVG 波形可视化形式直接在浏览器中显示。每个配置的引脚作为
独立通道显示。

### 协议解码

基于浏览器的 Rust/WASM 解码器可将捕获的波形解码为协议注释。支持的协议：

- UART
- I2C
- SPI

解码器通过以下稳定 URL 提供服务：

- `/assets/decoder/logic-decoder.js`
- `/assets/decoder/logic-decoder_bg.wasm`（`application/wasm`，gzip 压缩）

使用方法：启动捕获，等待 `done`，然后使用浏览器内解码器。注释直接渲染在
波形视图上。

### 导出格式

| 格式 | 说明 |
| --- | --- |
| CSV | 带时间戳的原始样本数据 |
| PulseView `.sr` | Sigrok 兼容文件，可在 PulseView 中使用配置的采样率打开 |

## 传输与端口分配

| 端点 | 用途 |
| --- | --- |
| raw TCP 5556 | 原生 Sigrok/BeagleLogic 逻辑分析仪协议 |
| raw TCP 5555 | 仅 J4 raw UART；绝不承载逻辑分析仪流量 |
| TCP 80、`rigol-ds` | 独立的 Rigol SCPI 兼容人格 |
| `/api/v1/scpi` WebSocket | 浏览器 SCPI 兼容传输 |

## Rigol SCPI 兼容

除原生 raw TCP 5556 外，固件还在既有 SCPI 兼容路径上模拟 Rigol DS1102D
混合信号示波器，PulseView 和 sigrok-cli 可通过 `rigol-ds` 驱动连接。

### 连接

```
tcp-raw/<板卡IP>/80
```

端口 80 按首字节复用：HTTP 流量转发到 Web 服务器，SCPI 流量由模拟器
处理。同一 SCPI 引擎也可通过 WebSocket 在 `ws://<板卡IP>/api/v1/scpi` 访问，
供浏览器客户端使用。

### 通道映射

| PulseView 通道 | 固件 GPIO 映射 |
| --- | --- |
| D0–D11 | 固定协议顺序：GP10、GP16、GP11、GP17、GP12、GP18、GP13、GP19、GP14、GP20、GP15、GP29 |
| D12–D14 | GP7、GP8、GP9（分别为 J13 pin 1、3、2） |
| CH1（模拟） | GP29 / ADC3（J8 pin 6） |

D0–D11 顺序是兼容性通道顺序，不是连接器顺序或完整物理 pinout。GP10/GP11
位于 J4；GP12–GP16、GP20、GP29 的 J8 位置如上表；GP17/GP18 尚无已确认的
J8/J13 位置；GP19 仍是不可驱动的 PD CFG1 strap。

### 行为

- **≤25 MHz**：真实硬件预触发捕获（300 预触发 + 212 后触发样本）。
- **>25 MHz**：512 样本单次突发，通过软件将触发边沿对齐到样本 300。
- **AUTO 回退**：如果在超时时间内没有检测到边沿，返回无触发帧。

### sigrok-cli 示例

```sh
# 扫描板卡
sigrok-cli -d rigol-ds:conn=tcp-raw/<board-ip>/80 --scan

# 带硬件预触发的数字捕获
sigrok-cli -d rigol-ds:conn=tcp-raw/<board-ip>/80 \
  --config timebase='20 us' --config triggersource=D0 \
  --config triggerslope=f --frames 1 --channels D0 -o capture.sr

# GP29 模拟通道
sigrok-cli -d rigol-ds:conn=tcp-raw/<board-ip>/80 \
  --config timebase='1 ms' --frames 1 --channels CH1 -o analog.sr
```

在 PulseView 中使用 `rigol-ds` 驱动，连接字符串为
`tcp-raw/<板卡IP>/80`。

## 深度捕获

深度捕获将数据录制到 2 MB SPI-flash 存储而非 RAM，最多支持 100 万个样本。

### 限制

| 参数 | 值 |
| --- | --- |
| 数字速率 | ≤25 kHz |
| 模拟速率（GP29） | ≤10 kHz |
| 最大样本数 | 1,000,000 |
| 存储 | 2 MB SPI-flash 分区 |

### Vendor SCPI 命令

| 命令 | 说明 |
| --- | --- |
| `:LINKR:DEEP:START <rate_hz> [duration_s]` | 擦除窗口并开始捕获（默认 2 秒，最大 30 秒） |
| `:LINKR:DEEP:STATUS?` | 查询状态：`IDLE`、`PREPARING`、`CAPTURING` 或 `DONE` |
| `:LINKR:DEEP:DATA? <offset> <count>` | 下载捕获样本（数字 2 字节/样本，模拟 1 字节/样本） |
| `:LINKR:DEEP:STOP` | 中止当前捕获 |

### Web UI

点击逻辑分析仪卡片上的 **Deep** 按钮（默认：25 kHz、2 秒窗口）。UI 显示
PREPARING/CAPTURING 进度，将样本下载到波形视图，并可导出最多 100 万个
样本的 CSV 或 PulseView `.sr` 文件。

## BeagleLogic 模拟

原生 Sigrok/BeagleLogic 协议监听 raw TCP **5556**，模拟 BeagleLogic 内核驱动，
在 PulseView 中提供连续采集。listener 绑定 `0.0.0.0`，连接时使用板卡当前 NCM
地址。

端口 **5555 不是逻辑分析仪端口**；它是 MCU 拥有的 J4 目标串口 binary raw-UART
传输。Sigrok/BeagleLogic client 连接 5555 无法协商或获得捕获帧。

### 连接

```
beaglelogic:conn=tcp-raw/<板卡IP>/5556
```

### 通道

14 个数字通道采用固定协议顺序（不是连接器顺序）：

- ch0–ch11：GP10、GP16、GP11、GP17、GP12、GP18、GP13、GP19、GP14、GP20、
  GP15、GP29
- ch12–ch13：GP7、GP8（J13 pin 1、3）

这些通道号保留 wire protocol，不表示它们位于同一排针。探针位置必须按当前
固件 metadata 和 x1.2 板图确认。

### 速率和采样格式

| 格式 | 持续速率 |
| --- | --- |
| 16-bit | 最高约 150 kHz |
| 8-bit | 最高约 100 kHz |

≥100 kHz 的速率使用硬件 PIO+DMA 路径。较低速率使用定时 GPIO 寄存器循环。

### 连接故障排查

- 原生 Sigrok/BeagleLogic 使用 raw TCP **5556**；raw TCP **5555** 仅用于 J4 UART。
- 5556 拒绝连接时，确认板卡当前 NCM 地址，并确认没有 WebSocket 或 TCP Sigrok
  session 已占有分析仪。
- 如果收到目标控制台字节而不是协议帧，说明误连到了 5555 的 J4；断开后改用 5556。

## 流式模式（1–25 MHz）

用于较低采样率下的实时波形监控，通过同一 SCPI 示波器协议经 WebSocket
传输。

### 传输

```
ws://<板卡IP>/api/v1/scpi
```

Web UI 循环流式传输 600 样本的实时帧。每帧是一个无间隙的数据岛，以数十
毫秒的间隔到达。这不是连续的多 MHz 记录。

### 浏览器 UI

点击逻辑分析仪卡片上的 **Stream** 开始流式传输（25 MHz 以上禁用）。
实时状态行显示流式传输状态、实际速率和总接收样本数。滚动的实时波形为
每个选定引脚渲染缓冲的样本历史。

流式缓冲区最多保留 100 万个样本，支持窗口大小选择（1K–256K）、跟随最新
数据切换和用于浏览历史数据的平移滑块。

解码器仅对单次捕获进行完整的事后分析，不处理流式数据。

## 快速开始

1. 打开 Web UI：`http://<board-ip>/`
2. 在 Terminal 工作区选择 **逻辑分析仪** 标签
3. 配置引脚、采样率和触发模式
4. 点击 **Arm capture**
5. 查看波形，或导出 CSV / PulseView `.sr`
6. 实时监控请点击 **Stream**（1–25 MHz）
7. Rigol SCPI 兼容访问使用 `rigol-ds` 驱动，连接字符串
   `tcp-raw/<board-ip>/80`
8. 原生 Sigrok/BeagleLogic raw TCP 使用
   `beaglelogic:conn=tcp-raw/<board-ip>/5556`
9. raw TCP 5555 保留给 J4 UART 控制台

## 延伸阅读

- [实现细节](../reference/logic-analyzer.md) — 固件架构、API 参考、速率指导
  和文件级源码索引
- [PulseView](https://sigrok.org/wiki/PulseView)
- [Sigrok 文件格式](https://sigrok.org/wiki/File_format:Sigrok/v2)
