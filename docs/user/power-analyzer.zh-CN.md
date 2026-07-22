[English](power-analyzer.md)

# 功率分析仪

基于固件环形缓冲区和设备单调时间戳的功率分析。功率分析仪捕获电源轨的
电流波形，支持多种触发类型，并提供最多四次捕获的叠加对比。

功率分析仪在 Web UI 仪表盘的 **电源与电流** 卡片中可用，启动功率分析
位于 **高级与恢复** 工具箱中。

## 捕获容量

| 板卡 | 容量 |
| --- | --- |
| RP2350 | 2048 个样本 |

约束条件：`pre_samples + post_samples + 1` 不得超过板卡容量。

捕获存储使用固件拥有的全局环形缓冲区，因为同一时间只能有一个硬件 ADC
捕获处于活动状态。关闭拥有该捕获的 WebSocket 会取消捕获。

## 触发类型

| 触发 | 说明 | 额外参数 |
| --- | --- | --- |
| `manual` | 发送 `capture_trigger` 命令触发 | — |
| `current` | 电流超过阈值时触发 | `threshold_ua`（微安） |
| `power_on` | 电源轨上电事件触发 | — |
| `gpio` | GPIO 边沿触发 | `gpio`（来自安全列表）+ `edge`（`rising`/`falling`/`either`） |

## 启动工作流

捕获生命周期通过现有 live WebSocket 会话的命令完成：

1. **启动**：发送带触发配置的 `capture_arm` 命令
2. **开始**：固件回复 `capture_begin`
3. **样本**：固件按顺序为每个缓冲样本发送 `capture_sample`
4. **完成**：固件发送 `capture_complete`

启动命令示例：

```json
{
  "type": "command",
  "command": "capture_arm",
  "id": "capture-1",
  "trigger": "current",
  "output": "5v_out",
  "threshold_ua": 500000,
  "rate_hz": 100,
  "pre_samples": 100,
  "post_samples": 300
}
```

手动捕获在启动后发送 `capture_trigger`。使用 `capture_cancel` 取消。

### 时间戳归一化

根据捕获响应中的 `trigger_offset` 归一化 X 轴。主机接收时间不是可靠的
采样时钟——请使用每个样本附带的设备单调时间戳。

## Web UI

### 电源与电流卡片

仪表盘 **电源与电流** 卡片提供：

- 各电源轨的实时电流读数
- 支持手动、电流阈值、GPIO 边沿和上电触发的触发式功率分析
- 最近 4 次捕获的叠加对比
- 导出带设备时间戳的 CSV 或 NDJSON
- 最新捕获的持续时间、mAh 和 Wh 报告（基于设备单调时间戳的梯形积分）

### 导出格式

| 格式 | 说明 |
| --- | --- |
| CSV | 带设备时间戳的电流/电压样本 |
| NDJSON | 每行一个 JSON 对象，包含设备时间戳 |

两种格式均保留触发配置、源电源轨、边沿/阈值设置、采样率和预/后窗口大小。

## CLI：`adc record`

命令行连续 ADC 录制：

```sh
radxa-linkr-debuggerctl adc record OUTPUT_PATH [MAX_SAMPLES] [--rate-hz HZ]
```

示例：

```sh
# 录制为 NDJSON（默认）
radxa-linkr-debuggerctl adc record /tmp/adc.ndjson 1000 --rate-hz 250

# 录制为 CSV
radxa-linkr-debuggerctl adc record /tmp/adc.csv 1000 --rate-hz 250
```

### 输出详情

- **格式**：默认 NDJSON；输出路径以 `.csv` 结尾时为 CSV
- **设备时间**：保存在 `metadata.device_timing` 下，包含
  `sample_sequence`、`uptime_us` 和 `device_t_mono_us` 字段
- **CSV 时间列**：优先使用 `device_t_mono_us`，回退到 `uptime_us`，最后
  使用 `0`
- **环形溢出**：在受影响的首行报告为 `metadata.dropped_samples`
- **速率**：默认 1000 Hz WebSocket 订阅；`--rate-hz` 接受更低的速率。
  100 Hz 以上的请求使用批量 JSON 传输，录制器将每个设备样本展开为独立行。

## 启动功率分析

位于 Web UI 的 **高级与恢复** 中。同时记录上电电流波形和串口启动里程碑。

### 前置条件

- 已连接的 UART0 或 UART1 串口会话
- 空闲的功率捕获会话

### 工作流

1. 清除并记录目标串口控制台
2. 关闭选定电源轨，等待配置的放电延迟
3. 在恢复电源轨之前启动固件 `power_on` 捕获
4. 上电后独立记录串口接收数据
5. 为上电后的首个 UART 字节、U-Boot/UEFI 标记、内核标记和登录标记
   打时间戳
6. 报告峰值电流、平均功率和积分能量

### 启动固件检测

自动检测从串口输出识别启动固件类型。当目标平台已知时（如 Radxa O6N、
Q6A），可手动指定为 **U-Boot** 或 **UEFI**。

### 结果

- **峰值电流**：使能电源轨上的最大样本值
- **平均功率**：能量除以捕获持续时间
- **积分能量**：基于设备单调时间戳的梯形积分
- **功率曲线叠加**：同一电源轨最近两次完成的运行触发对齐后叠加显示

能量仅覆盖固件捕获窗口，不包含窗口外的串口活动。

### 限制

- 仅保留上电请求后收到的字节；电源轨关闭期间排空的输入视为过期数据
- 无上电后 UART 数据、串口连接丢失或无 Login 签名的捕获报告为部分完成
- `power_on` 捕获将全部捕获容量用于后触发样本；当前固件不返回预触发
  环形缓冲区
- 串口里程碑使用浏览器单调时钟（主机观测时间），功率曲线使用固件
  单调时钟

## 延伸阅读

- [协议细节](../../doc/power-analyzer.md) — 捕获协议、WebSocket 消息格式
  和固件端实现
