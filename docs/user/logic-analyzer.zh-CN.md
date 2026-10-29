[English](logic-analyzer.md)

# 逻辑分析仪

RP2350 PIO2+DMA 高速单次 GPIO 捕获，用于 PIO 速率下的短突发诊断。
不适用于高采样率下的持续流式传输。

逻辑分析仪位于 Web UI 的独立 **Logic Analyzer 工作区**，与串口终端分开。

## 捕获 GPIO 引脚

正式目标采集集合为 **GP12–GP16、GP27–GP29，共 8 路数字逻辑输入**。
ADC1–ADC3 是后三个触点的名称，在这里指数字高/低电平采集，不是模拟电压采样。

Web UI **暂时仅可选 GP12–GP16**（共 5 个通道），默认选择 GP13。GP27–GP29
采集能力**尚未实现**，需要先 review 固件通道映射、引脚 ownership 和协议契约。
目前只读显示是临时安全状态，不能视为已完成八通道要求。J8/J13 图仍作为正式
物理布局参考：

| 引脚 / 触点 | Web UI 行为 |
| --- | --- |
| GP12–GP16 | 可选择的数字采集引脚，J8 pin 1–5 |
| GP7–GP9 | 禁用的 J13 恢复控制（`CON_MAS`、`CON_REST`、`CON_USER`） |
| ADC1–ADC3（GP27/GP28/GP29） | 保留触点，分别对应 J8 pin 8/7/6；采集能力待实现，目前只读且不由 Web 采集功能采样 |
| GP5、GP6、GP20 及其他非采集触点 | 保持无交互的布局参考，不响应采集或触发操作 |

底层原生 sigrok/CLI 的硬件允许列表仍为 **GP10–GP20**，但原有
GP10/GP11/GP17–GP20 补充采集入口不再出现在 Web 选择器中。这一界面限制不改变
sigrok 协议或 WIDE11 通道映射。GP10/GP11 属于 J4 UART；GP19 是内部 PD CFG1
strap，绝不能驱动；GP20 是 J8 pin 9 / PIO I2C SCL，绝不能驱动或拉低。
GP21（J8 pin 10 / SDA）不属于 WIDE11 通道。连接探针前应以固件 GPIO metadata
和 x1.2 板图为准。

**J4 仍会受到影响：**当前 FAST8 后端接管 GP10–GP17 的物理范围，其中包含
GP10/GP11。因此，即使 Web 只选择 GP12–GP16，采集仍会暂停 J4 UART；在选择器
隐藏 UART 引脚并不等于采集时与这些引脚隔离。

## 配置参数

| 参数 | 范围 | 说明 |
| --- | --- | --- |
| Web UI 通道 | 暂为 1–5 | 仅 GP12–GP16；ADC1–ADC3 采集能力待实现 |
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

在 Web UI 中打开独立的 **Logic Analyzer 工作区**。
宽屏下左侧为采样设置、右侧为引脚选择，小屏时顺序堆叠。两组控件下方是独立的
全宽波形区域，随后是同页的协议配置和结果，不再通过“采集/协议解码”模式切换。

### 波形预览

捕获的样本以 SVG 波形可视化形式直接在浏览器中显示。每个配置的引脚作为
独立通道显示。有界采集和连续采样使用同一个波形区域；协议注释与数字波形共用
同一时间坐标，不再另建第二份波形预览。
设置预览显示请求采样率；波形旁的实际采样率来自采集确认。修改下一次采集的
配置不会改变已有捕获的实际采样率或时间轴。

连续采样期间，取消勾选“跟随最新”会固定当前的绝对样本窗口，采样仍继续。
滑块可选择其他尚在缓存内的窗口，重新勾选后返回最新样本。浏览器保留最近
1,000,000 个样本；若冻结窗口被淘汰，会显示警告并移到最早可用窗口，不会把
较新的数据悄悄标为旧历史。

连续波形预览按显示宽度分桶，保留每桶的首末边沿，避免把快速翻转或单点脉冲
画成恒定电平。密集活动仍会压缩显示；要仔细查看，应减小窗口，不能把总览
中的边沿数量当作全部实际跳变。

### 协议解码

基于浏览器的 Rust/WASM 解码器可将捕获的波形解码为协议注释。支持的协议：

- UART
- I2C
- SPI

解码器通过以下稳定 URL 提供服务：

- `/assets/decoder/logic-decoder.js`
- `/assets/decoder/logic-decoder_bg.wasm`（`application/wasm`，gzip 压缩）

可在采集前准备协议配置。有界采集完成（`done`）后，在同一页面运行浏览器内
解码器；更改协议配置会使旧结果失效，需要重新解码。连续采样期间会根据同页
协议配置自动更新注释。通道配置无效或解码失败时，协议面板会显示错误；修正配置
后可恢复解码。注释直接绘制在现有波形上；导出、取消采集和停止连续
采样仍是独立的采集操作。

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
位于 J4；GP12–GP16、GP20、GP29 分别位于 J8 pin 1–5、9、6；GP17/GP18 尚无已确认的
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

## Stream 模式

Web UI 的 Stream 使用 sigrok live session 及其二进制 WebSocket 协议，与上文
可选的 Rigol SCPI 兼容接口相互独立。

### 传输

UI 先创建 `/api/v1/live-sessions`，连接返回的 `/api/v1/ws/<slot>`，再发送
`post=0` 的 sigrok `CONFIG`。25 MHz 及以下使用滚动 Stream 路径；浏览器最多
保留最近一百万个样本，这个有界历史不代表无限期无损记录。

在受支持的高率组合上，100/125 MHz Stream 是一次无损 packed burst，而非无限
连续监控。它只要求协商出 `GENERIC_PACKED_BURST`；`post=0` sentinel 仍使用
CONFIG v1，不要求 `CONFIG_V2`。设备的 100000 样本缓冲区写满后自动停止，
并以只读最终 Stream 快照保留。WIDE11 的 125 MHz 组合仍不支持；触发后深度
超过 65535 的 bounded capture 仍要求 `CONFIG_V2`。

### 浏览器 UI

在独立的逻辑分析仪工作区点击 **Stream**。状态行显示请求/实际速率、总接收
样本数，以及保留快照是自动停止、手动停止还是不完整。滚动波形为每个选定
引脚渲染缓冲历史；操作前后都会显示高率能力和自动停止说明。

流式缓冲区最多保留 100 万个样本，支持窗口大小选择（1K–256K）、跟随最新
数据切换和用于浏览历史数据的平移滑块。

解码器支持单次捕获的完整事后分析，也会从有界的流式数据窗口更新实时注释。

## 快速开始

1. 打开 Web UI：`http://<board-ip>/`
2. 打开独立的 **Logic Analyzer** 工作区
3. 配置引脚、采样率和触发模式
4. 点击 **Arm capture**
5. 查看波形，或导出 CSV / PulseView `.sr`
6. 25 MHz 及以下的滚动监控请点击 **Stream**；受支持的 100/125 MHz 组合会
   执行一次 100000 样本 packed burst，并保留只读结果
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
