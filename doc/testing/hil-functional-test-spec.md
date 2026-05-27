# HIL 功能测试规范

本文档定义 `radxa-linkr-debugger` 固件与主机控制逻辑改动后的板级 HIL（Hardware-in-the-Loop）功能测试规范。

## 适用范围

以下改动不能只依赖编译/静态检查/unit test，必须完成 HIL 实机功能验证：

- RP2040 固件逻辑改动
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
```

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

将 `/dev/sdX1` 替换为实际 RP2040 BOOTSEL 块设备路径。

## 标准 HIL 验证 checklist

默认优先使用 release 发布的 `radxa-linkr-debuggerctl` CLI 执行以下主机侧验收命令。
只有在你正在验证尚未发布的 `cmd-ng` 改动时，才将同一组命令替换为等价的
`cargo run --manifest-path cmd-ng/Cargo.toml -- ...` 源码运行方式。

### 1. 主机诊断与状态

```sh
radxa-linkr-debuggerctl --json doctor
radxa-linkr-debuggerctl --json status
```

验证：

- `schema`
- `ok`
- `command`
- `error.code`（异常时）

### 2. HTTP API 连通性

```sh
curl -fsS http://172.29.203.1:8080/api/v1/status
curl -fsS http://172.29.203.1:8080/api/v1/watchdog
radxa-linkr-debuggerctl --json power list
```

### 3. 电源输出 get/set

```sh
radxa-linkr-debuggerctl --json power set 12v_out on
radxa-linkr-debuggerctl --json power set 12v_out off

radxa-linkr-debuggerctl --json power set 5v_out on
radxa-linkr-debuggerctl --json power set 5v_out off

radxa-linkr-debuggerctl --json power set 5v_ws on
radxa-linkr-debuggerctl --json power set 5v_ws off

radxa-linkr-debuggerctl --json power set 20v_out on
radxa-linkr-debuggerctl --json power set 20v_out off
```

### 4. ADC 电流监测

```sh
radxa-linkr-debuggerctl --json adc read
radxa-linkr-debuggerctl --json adc read 5v_out
radxa-linkr-debuggerctl --json adc read 12v_out
radxa-linkr-debuggerctl --json adc read 20v_out
```

验证：

- 返回字段合理
- `raw / mv / current_ua / sensor_value` 可解析

### 5. switch 路由控制

```sh
radxa-linkr-debuggerctl --json switch get sd
radxa-linkr-debuggerctl --json switch route sd target
radxa-linkr-debuggerctl --json switch get sd

radxa-linkr-debuggerctl --json switch get usb
radxa-linkr-debuggerctl --json switch route usb target --confirm
radxa-linkr-debuggerctl --json switch get usb
```

注意：

- `set` 后等待硬件稳定再 `get`
- 优先顺序验证，避免并行反向测试干扰

### 6. safe GPIO

```sh
radxa-linkr-debuggerctl --json gpio list
radxa-linkr-debuggerctl --json gpio set GP13 1
radxa-linkr-debuggerctl --json gpio input GP13
radxa-linkr-debuggerctl --json gpio set GP13 0
radxa-linkr-debuggerctl --json gpio input GP13
```

### 7. watchdog 状态

```sh
radxa-linkr-debuggerctl --json watchdog status
```

### 8. BOOTSEL 进入

```sh
radxa-linkr-debuggerctl bootloader
```

若 HTTP 不可用，使用串口 fallback：

```text
linkr-debugger:~$ bootloader
```

随后重新烧录：

```sh
picotool load -v -x build/radxa_linkr_debugger/zephyr/zephyr.uf2
```

### 9. USB CDC ACM fallback

验证：

- HTTP/WS 不可用时，CDC ACM 串口仍可访问
- fallback shell 仍可执行基本命令
- fallback 路径仍可进入 BOOTSEL

## 最短 HIL smoke test

时间紧张时，至少验证以下 6 项：

1. `--json doctor`
2. `--json status`
3. `power set/get`
4. `adc read`
5. `switch route/get`
6. `bootloader`

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
