# HIL 功能测试规范

本文档定义 `radxa-linkr-debugger` 固件与主机控制逻辑改动后的板级 HIL（Hardware-in-the-Loop）功能测试规范。

## 适用范围

以下改动不能只依赖编译/静态检查/unit test，必须完成 HIL 实机功能验证：

- RP2040 / RP2350 固件逻辑改动
- 主机 CLI/TUI 真实控制逻辑改动
- 电源输出、switch 路由、ADC 电流监测、safe GPIO、watchdog、BOOTSEL 相关改动
- 任何新增面向硬件的功能

以下改动通常不需要 HIL：

- 文档、许可证、NOTICE、说明文字
- CI/workflow 解析、脚本语法检查、格式检查
- 不涉及真实硬件交互的 host-side 工具链/依赖更新

## 强制验收要求

固件相关改动必须同时满足：

1. 完整构建固件
2. 实机烧录并确认正常启动
3. 验证 HTTP/WS 控制通路
4. 验证 USB CDC ACM 串口 fallback 路径（USB CDC ACM serial fallback path）
5. 验证 BOOTSEL 进入能力（BOOTSEL entry path）
6. 新增功能补对应 functional test（可行时）

## 标准构建与烧录

### 构建

```sh
west build -p always -b rpi_pico/rp2040 apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
west build -p always -b rpi_pico2/rp2350a/m33 apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
```

按待测硬件选择其中一个完整构建命令。由于两者共用 canonical build 目录，烧录前最后一次
构建必须对应当前连接的 MCU。

### 烧录

```sh
picotool load -v -x build/radxa_linkr_debugger/zephyr/zephyr.uf2
```

或使用 RPI-RP2 拖拽方式：

```text
build/radxa_linkr_debugger/zephyr/zephyr.uf2
```

Linux 下免 root 烧录：

```sh
RPI_RP2=$(udisksctl mount -b /dev/sdX1 | awk -F" at " '{print $2}' | tr -d '[:space:]')
cp build/radxa_linkr_debugger/zephyr/zephyr.uf2 "$RPI_RP2/"
```

将 `/dev/sdX1` 替换为实际 RP2040 / RP2350 BOOTSEL 块设备路径。

## 标准 HIL 验证 checklist

默认优先使用 release 发布的 `radxa-linkr-debuggerctl` CLI 执行以下主机侧验收命令。
只有在你正在验证尚未发布的 `cmd-ng` 改动时，才将同一组命令替换为等价的
`cargo run --manifest-path cmd-ng/Cargo.toml -- ...` 源码运行方式。

### 1. 主机诊断与状态

```sh
timeout 5s radxa-linkr-debuggerctl --json doctor
timeout 5s radxa-linkr-debuggerctl --json status
```

验证：

- `schema`
- `ok`
- `command`
- `error.code`（异常时）

### 2. HTTP API 连通性

```sh
timeout 5s curl -fsS http://172.29.203.1:8080/api/v1/status
timeout 5s curl -fsS http://172.29.203.1:8080/api/v1/watchdog
timeout 5s curl -fsS http://172.29.203.1:8080/api/v1/switch
timeout 5s radxa-linkr-debuggerctl --json power list
timeout 5s radxa-linkr-debuggerctl --json switch list
```

G3 额外执行 `timeout 5s curl -fsS http://172.29.203.1:8080/api/v1/switch/vin`；
RP2040 不暴露该 switch。

WebSocket 订阅使用 Rust CLI 的 `adc record` 流程创建 live session：

```sh
timeout 10s radxa-linkr-debuggerctl adc record /tmp/linkr-hil.ndjson 3 --rate-hz 10
```

验证输出文件包含 3 条 `radxa-linkr-debugger.v1` telemetry record。
当前固件一次只支持一个活动 WebSocket 客户端；测试重复录制时必须等待前一个连接
正常关闭，不把并发 recorder 作为支持能力。

### 3. 电源输出 get/set

```sh
timeout 5s radxa-linkr-debuggerctl --json power set 12v_out on
timeout 5s radxa-linkr-debuggerctl --json power set 12v_out off

timeout 5s radxa-linkr-debuggerctl --json power set 5v_out on
timeout 5s radxa-linkr-debuggerctl --json power set 5v_out off

# 5v_ws is board-internal: list/status must omit it and direct CLI access must fail locally.
timeout 5s radxa-linkr-debuggerctl --json power list > /tmp/linkr-power-list.json
! grep -q '"name":"5v_ws"' /tmp/linkr-power-list.json
! timeout 5s radxa-linkr-debuggerctl --json power get 5v_ws
! timeout 5s radxa-linkr-debuggerctl --json power set 5v_ws off

# The raw API compatibility entry remains available for low-level diagnostics.
timeout 5s curl -fsS http://172.29.203.1:8080/api/v1/power/5v_ws

timeout 5s radxa-linkr-debuggerctl --json power set 20v_out on
timeout 5s radxa-linkr-debuggerctl --json power set 20v_out off
```

### 4. ADC 电流监测

```sh
timeout 5s radxa-linkr-debuggerctl --json adc read
timeout 5s radxa-linkr-debuggerctl --json adc read 5v_out
timeout 5s radxa-linkr-debuggerctl --json adc read 12v_out
timeout 5s radxa-linkr-debuggerctl --json adc read 20v_out
```

验证：

- 返回字段合理
- `raw / mv / current_ua / sensor_value` 可解析

### 5. switch 路由控制

```sh
timeout 5s radxa-linkr-debuggerctl --json switch get sd
timeout 5s radxa-linkr-debuggerctl --json switch route sd target
timeout 5s radxa-linkr-debuggerctl --json switch get sd

timeout 5s radxa-linkr-debuggerctl --json switch get usb
timeout 5s radxa-linkr-debuggerctl --json switch route usb target --confirm
timeout 5s radxa-linkr-debuggerctl --json switch get usb
```

注意：

- `set` 后等待硬件稳定再 `get`
- 优先顺序验证，避免并行反向测试干扰

### 6. VIN 控制（G3 专用）

默认 HIL 只确认 VIN 保持在安全默认值 3.3V，不执行 1.8V 切换：

```sh
timeout 5s radxa-linkr-debuggerctl --json switch get vin
# 物理验证：测量目标板 VIO 引脚电压是否为 ~3.3V
```

验证：

- `route` 字段回读为 `3.3v`
- 物理测量确认 VIO 电压约为 3.3V

只有同时满足以下条件时，才允许扩展执行 1.8V 切换测试：

- 已明确确认外接目标和 CH347 侧器件均支持 1.8V
- 已连接物理测量设备并能在切换后立即确认 VIO 电压
- 操作者已接受切换的硬件副作用

满足上述条件后，严格按顺序执行，并在结束时恢复 3.3V：

```sh
timeout 5s radxa-linkr-debuggerctl --json switch route vin 1.8v --confirm
sleep 5
timeout 5s radxa-linkr-debuggerctl --json switch get vin
# 物理验证：测量目标板 VIO 引脚电压是否为 ~1.8V
timeout 5s radxa-linkr-debuggerctl --json switch route vin 3.3v --confirm
sleep 5
timeout 5s radxa-linkr-debuggerctl --json switch get vin
# 物理验证：测量目标板 VIO 引脚电压是否为 ~3.3V
```

### 7. safe GPIO

```sh
timeout 5s radxa-linkr-debuggerctl --json gpio list
timeout 5s radxa-linkr-debuggerctl --json gpio set GP13 1
timeout 5s radxa-linkr-debuggerctl --json gpio input GP13
timeout 5s radxa-linkr-debuggerctl --json gpio set GP13 0
timeout 5s radxa-linkr-debuggerctl --json gpio input GP13
```

### 8. watchdog 状态

```sh
timeout 5s radxa-linkr-debuggerctl --json watchdog status
```

### 9. BOOTSEL 进入

```sh
timeout 5s radxa-linkr-debuggerctl bootloader
```

若 HTTP 不可用，使用串口 fallback：

```text
linkr-debugger:~$ bootloader
```

随后重新烧录：

```sh
picotool load -v -x build/radxa_linkr_debugger/zephyr/zephyr.uf2
```

### 10. USB CDC ACM fallback

验证：

- HTTP/WS 不可用时，CDC ACM 串口仍可访问
- fallback shell 仍可执行基本命令
- fallback 路径仍可进入 BOOTSEL

CDC ACM shell 验证（G3 固件）：

```text
linkr-debugger:~$ vin get
linkr-debugger:~$ bootloader
```

默认 CDC fallback 验证不切换 VIN。`vin set 1.8v` / `vin set 3.3v` 只允许在满足
第 6 节的目标兼容性、物理测量和副作用确认条件后执行。

RP2040 (G2) 固件仍注册 `vin` shell 命令，但执行时会明确返回 unavailable；
只有 G3 支持实际 VIN 控制。

## 最短 HIL smoke test

时间紧张时，至少验证以下 6 项：

1. `--json doctor`
2. `--json status`
3. `power set/get`
4. `adc read`
5. `switch route/get`
6. `bootloader`

VIN 1.8V 切换不属于默认 smoke test 范围；它是 G3 专用且需要目标板电压兼容确认和物理测量验证，属于条件限定的可选步骤，详见上方第 6 节。

## 验证结果记录建议

建议记录：

- 提交 hash
- 固件版本/tag
- 构建目录与 UF2 路径
- 烧录方式
- 实测命令与返回值摘要
- 是否通过 BOOTSEL 与 CDC ACM fallback
- 失败项与风险说明

## 参考来源

- `README.md`
- `AGENTS.md`
- `skills/radxa-linkr-debugger/SKILL.md`
