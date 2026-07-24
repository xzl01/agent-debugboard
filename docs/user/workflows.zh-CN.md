[English](workflows.md)

# 常见工作流

典型任务的分步指南。

## 给目标板断电重启

```sh
# 关闭
radxa-linkr-debuggerctl power set 5v_out off

# 等一下让电容放电
sleep 2

# 打开
radxa-linkr-debuggerctl power set 5v_out on

# 确认
radxa-linkr-debuggerctl power list
```

配合串口监控做硬重启：
```sh
# （另一个终端通过 Web Serial 或 screen 连接串口）

radxa-linkr-debuggerctl power set 5v_out off
sleep 2
radxa-linkr-debuggerctl power set 5v_out on
# 在串口终端看启动输出
```

## 读写目标板的 SD 卡

把 SD 卡路由到主机，像 USB 一样访问，然后切回去：

```sh
# SD 卡路由到主机
radxa-linkr-debuggerctl switch route sd usb-reader

# SD 卡现在作为 USB 大容量存储出现在 PC 上
# 挂载、复制文件、卸载

# SD 卡路由回目标板
radxa-linkr-debuggerctl switch route sd target
```

Linux 上卡会出现在 `/dev/sdX`。macOS 自动挂载。切换路由前一定要先卸载/弹出。

## 捕获启动电流波形

用 Web UI 的启动功率分析（在 **高级与恢复** 下），或者手动操作：

```sh
# 1. 开始录制 ADC 数据
radxa-linkr-debuggerctl adc record /tmp/startup.ndjson 2000 --rate-hz 500 &

# 2. 给目标板断电重启
radxa-linkr-debuggerctl power set 5v_out off
sleep 2
radxa-linkr-debuggerctl power set 5v_out on

# 3. 等启动完成后停止录制（Ctrl+C）
```

NDJSON 文件包含整个启动过程的带时间戳电流读数。

## 捕获逻辑分析仪波形

通过 Web UI：

1. 打开 http://172.29.203.1/，进入 **Terminal** 工作区。
2. 选引脚（如 GP13、GP15）和采样率。
3. 选触发模式（none、rising、falling、either）。
4. 点 **Arm**，然后触发要捕获的事件。
5. 预览波形，导出 CSV 或 PulseView `.sr`。

通过 API：

```sh
# Arm：GP13、1 MHz、256 样本、上升沿触发
curl -X POST http://172.29.203.1/api/v1/logic-analyzer \
  -d '{"selected_pins":[13],"sample_rate_hz":1000000,"post_samples":256,"trigger":"rising","trigger_pin":13}'

# 等捕获完成后获取数据
curl http://172.29.203.1/api/v1/logic-analyzer/capture
```

## 通过 OTA 更新固件

```sh
# 查看当前 OTA 状态
radxa-linkr-debuggerctl ota status

# 上传新固件
radxa-linkr-debuggerctl ota upload radxa-linkr-debugger-rp2350-ota.bin

# 请求测试启动
radxa-linkr-debuggerctl ota test

# 等板子回来（watchdog 约 16 秒后自动确认）
# 或手动确认：
radxa-linkr-debuggerctl ota confirm
```

不要上传 `.uf2` 或 `.elf`。用 `-ota.bin` release 产物。

## 救砖

如果板子 HTTP 和串口都没响应：

1. 拔掉 USB。
2. 按住 RP2350 上的 BOOTSEL 按钮。
3. 按住的同时插回 USB。
4. 板子会出现为 `RPI-RP2` USB 大容量存储。
5. 刷入恢复固件：

```sh
picotool load -v -x radxa-linkr-debugger-rp2350.uf2
```

或者把 `.uf2` 文件拖到 `RPI-RP2` 盘符上。

## 边控制电源边监控目标串口

用 Web UI 的串口终端（Terminal 工作区）配合仪表盘电源控制。或者 CLI + screen 并行：

```sh
# 终端 1：通过 CH347F 连接串口
screen /dev/ttyUSB0 115200

# 终端 2：控制电源
radxa-linkr-debuggerctl power set 5v_out off
sleep 2
radxa-linkr-debuggerctl power set 5v_out on
```

CH347F 提供两路独立 UART（UART0 在 `D1`，UART1 在 `D3`）。Web UI 支持 Tab 模式（切换通道）或分屏模式（同时显示）。

## 自动化测试

CI 或脚本场景用 `--json` 输出解析信封格式：

```sh
# 健康检查
radxa-linkr-debuggerctl --json doctor | jq '.ok'

# 断电重启并验证
radxa-linkr-debuggerctl --json power set 5v_out off
sleep 2
radxa-linkr-debuggerctl --json power set 5v_out on
radxa-linkr-debuggerctl --json power list | jq '.power_outputs[] | select(.name=="5v_out") | .state'

# 读电流
radxa-linkr-debuggerctl --json adc read | jq '.readings[] | select(.name=="5v_out") | .current_ua'
```

JSON 信封固定包含 `schema`、`ok`、`command`、`error`（失败时）。判断成功/失败解析 `ok`，不要解析人读的文本。
