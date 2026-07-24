[English](troubleshooting.md)

# 故障排查

常见问题和解决方法。

## CLI 连不上板子

**症状：** `doctor` 失败或 `status` 返回连接错误。

1. 检查 USB 线。必须是数据线——纯充电线不行。
2. 确认板子出现了网络接口：
   - Linux：`ip link show` — 找新的 `enp*` 或 `usb*`
   - macOS：`ifconfig` — 找 `172.29.203.x` 的 `en*`
   - Windows：网络连接里找新的 RNDIS/NCM 适配器
3. Ping 板子：`ping 172.29.203.1`
4. 如果 ping 通但 CLI 不行，显式指定 URL：
   ```sh
   radxa-linkr-debuggerctl --url http://172.29.203.1 doctor
   ```
5. 还是不行？换根线或换个 USB 口试试。

## Web UI 打不开

**症状：** 浏览器访问 `http://172.29.203.1/` 显示"无法访问此网站"。

1. 先确认 CLI 能连上：`radxa-linkr-debuggerctl doctor`
2. 如果 doctor 通过但浏览器不行，检查是否有其他网络接口优先级更高。
3. 用无痕/隐私窗口试试——扩展程序可能干扰。
4. macOS 上如果之前关掉过 captive portal 提示，需要手动输入 URL。

## macOS Gatekeeper 拦截 CLI

**症状：** 弹出"Apple 无法验证此软件"对话框。

```sh
xattr -dr com.apple.quarantine ./radxa-linkr-debuggerctl
```

安装脚本会自动处理。如果是手动解压的，对二进制文件执行上面的命令。

## TUI 显示的数据是旧的

**症状：** 切换后 switch 状态或 GPIO 值没有更新。

TUI 轮询频率 60 Hz，但物理 mux 需要稳定时间。切换路由后等 1–2 秒再看 `actual` 和 `desired`。如果两者不一致，硬件 mux 可能没切过去——再执行一次路由命令。

## Switch 路由没生效

**症状：** `switch route` 后 `switch get` 还是旧路由。

1. 检查状态输出中的 `desired` 和 `actual` 字段。
2. USB 和 VIN 路由需要 `--confirm`：
   ```sh
   radxa-linkr-debuggerctl switch route usb target --confirm
   ```
3. 等几秒再查。硬件 mux 需要稳定时间。
4. 不要快速连续执行相反方向的路由命令。

## ADC 读数为零

**症状：** `adc read` 显示 0.000000A。

电源输出必须是 ON 状态才有电流。检查：
```sh
radxa-linkr-debuggerctl power list
```

如果电源关着，先打开：
```sh
radxa-linkr-debuggerctl power set 5v_out on
```

## OTA 上传失败

**症状：** `ota upload` 返回错误。

- `sha256_mismatch`：传输过程中文件损坏。重新下载再试。
- `size_mismatch`：请求头中的大小与实际文件不符。重新上传。
- `invalid_mcuboot_header`：文件格式不对。用 `radxa-linkr-debugger-rp2350-ota.bin`，不要用 `.uf2` 或 `.elf`。
- `image_too_large`：固件超出 OTA 分区大小。

## Watchdog 不断复位板子

**症状：** 板子反复重启，LED 停止闪烁。

watchdog 在核心服务停止上报健康状态时会复位板子。通常意味着固件崩了。通过 CDC ACM 串口看崩溃日志：

```sh
# Linux / macOS
screen /dev/ttyACM0 115200
```

如果板子陷入启动循环，用 BOOTSEL 恢复：
```sh
# Zephyr shell 可用时
bootloader

# 或者按住 BOOTSEL 上电，然后：
picotool load -v -x radxa-linkr-debugger-rp2350.uf2
```

## CDC ACM 串口没出现

**症状：** 插入后没有 `/dev/ttyACM0`（Linux）或 COM 口（Windows）。

1. 检查 `dmesg`（Linux）或设备管理器（Windows）有没有 USB 枚举错误。
2. 换个 USB 口——有些 hub 不支持 CDC ACM。
3. Linux 上可能需要把用户加到 `dialout` 组：
   ```sh
   sudo usermod -aG dialout $USER
   ```
   注销重新登录生效。

## 逻辑分析仪捕获失败

**症状：** `POST /api/v1/logic-analyzer` 返回 `invalid_config`。

- 检查引脚是否在安全列表中：GP7–GP9、GP10–GP20、GP29。
- `pre_samples > 0` 只在 ≤25 MHz 的边沿触发下有效。
- 总样本数（pre + post）不能超过 512。
- 采样率必须在 100,000 到 125,000,000 Hz 之间。

## 多个 TUI 实例互相干扰

**症状：** 一个 TUI 的修改在另一个中看不到。

正常情况下不会——TUI 用 HTTP 轮询，多实例应该共存。如果出问题，确认每个实例用的是同一个板子 URL。混用 `--url` 参数会造成混乱。

## 还是解决不了？

- [CLI 参考](cli.zh-CN.md) — 命令详情
- [HTTP API 参考](api.zh-CN.md) — 原始 API 调试
- [GitHub Issues](https://github.com/xzl01/agent-debugboard/issues) — 提交问题
