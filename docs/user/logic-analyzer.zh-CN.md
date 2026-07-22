[English](logic-analyzer.md)

# 逻辑分析仪

RP2350 PIO2+DMA 高速单次 GPIO 捕获，用于 PIO 速率下的短突发诊断。
不适用于高采样率下的持续流式传输。

逻辑分析仪位于 Web UI 的 **Terminal 工作区**，与串口终端并列。

## 安全 GPIO 引脚

以下引脚可用于逻辑分析仪捕获：

| 引脚 | 标签 |
| --- | --- |
| GP7 | CON_MAS |
| GP8 | CON_REST |
| GP9 | CON_USER |
| GP10–GP20 | J16 排针 |
| GP29 | ADC3 |

不可使用其他 GPIO 引脚。

## 配置参数

| 参数 | 范围 | 说明 |
| --- | --- | --- |
| 通道 | 1–16 | 从上述安全引脚列表中选择 |
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
| `invalid_config` | 400 | 无效 JSON、参数越界、pre_samples 配合无边沿触发、或速率 >25 MHz |
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

## PulseView 原生集成

固件模拟 Rigol DS1102D 混合信号示波器，PulseView 和 sigrok-cli 可直接
连接，无需客户端修改。

### 连接

```
tcp-raw/<板卡IP>/80
```

端口 80 按首字节复用：HTTP 流量转发到 Web 服务器，SCPI 流量由模拟器
处理。同一 SCPI 引擎也可通过 WebSocket 在 `ws://<板卡IP>/api/v1/scpi` 访问，
供浏览器客户端使用。

### 通道映射

| PulseView 通道 | 板卡引脚 |
| --- | --- |
| D0–D11 | J16 PIN1–PIN12（GP10、GP16、GP11、GP17、GP12、GP18、GP13、GP19、GP14、GP20、GP15、GP29） |
| D12–D14 | J13 CON 引脚（GP7、GP8、GP9） |
| CH1（模拟） | GP29（ADC3） |

### 行为

- **≤25 MHz**：真实硬件预触发捕获（300 预触发 + 212 后触发样本）。
- **>25 MHz**：512 样本单次突发，通过软件将触发边沿对齐到样本 300。
- **AUTO 回退**：如果在超时时间内没有检测到边沿，返回无触发帧。

### sigrok-cli 示例

```sh
# 扫描板卡
sigrok-cli -d rigol-ds:conn=tcp-raw/172.29.203.1/80 --scan

# 带硬件预触发的数字捕获
sigrok-cli -d rigol-ds:conn=tcp-raw/172.29.203.1/80 \
  --config timebase='20 us' --config triggersource=D0 \
  --config triggerslope=f --frames 1 --channels D0 -o capture.sr

# GP29 模拟通道
sigrok-cli -d rigol-ds:conn=tcp-raw/172.29.203.1/80 \
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

TCP 端口 **5555** 上的第二 sigrok 人格模拟 BeagleLogic 内核驱动，在
PulseView 中提供无限连续采集。

### 连接

```
beaglelogic:conn=tcp-raw/<板卡IP>/5555
```

### 通道

14 个数字通道，按 J16 连接器顺序：

- ch0–ch11：J16 PIN1–PIN12（GP10、GP16、GP11、GP17、GP12、GP18、GP13、GP19、
  GP14、GP20、GP15、GP29）
- ch12–ch13：J13 CON 引脚（GP7、GP8）

### 速率和采样格式

| 格式 | 持续速率 |
| --- | --- |
| 16-bit | 最高约 150 kHz |
| 8-bit | 最高约 100 kHz |

≥100 kHz 的速率使用硬件 PIO+DMA 路径。较低速率使用定时 GPIO 寄存器循环。

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

1. 打开 Web UI：`http://172.29.203.1/`
2. 在 Terminal 工作区选择 **逻辑分析仪** 标签
3. 配置引脚、采样率和触发模式
4. 点击 **Arm capture**
5. 查看波形，或导出 CSV / PulseView `.sr`
6. 实时监控请点击 **Stream**（1–25 MHz）
7. 直接 PulseView 访问请使用 `rigol-ds` 驱动，连接字符串
   `tcp-raw/172.29.203.1/80`

## 延伸阅读

- [实现细节](../../doc/logic-analyzer.md) — 固件架构、API 参考、速率指导
  和文件级源码索引
- [PulseView](https://sigrok.org/wiki/PulseView)
- [Sigrok 文件格式](https://sigrok.org/wiki/File_format:Sigrok/v2)
