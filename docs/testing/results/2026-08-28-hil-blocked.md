# 2026-08-28 Linkr HIL 记录（BLOCKED）

## Scope

本轮 HIL 用于验证 2026-08-28 工作区中的 Web、task-blob 解析和 task HTTP
资源注册改动。目标是先烧录当前 canonical combined UF2，再验证 HTTP/WS、
任务端点、CDC fallback 与 BOOTSEL 恢复路径。

## Artifacts

- Combined UF2（用于 ROM BOOTSEL）：
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`
- SHA256：`f0ff97b0b5396568f7a5aca65560000c8e33239709ee2b0c6639daf05889947d`
- OTA bin（未使用）：
  `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350-ota.bin`
- SHA256：`0431dd964fad0ac7a178bdaa657fcd68832cce32a43b95859dd6a1a4a0c2a1da`

## Pre-flight（PASS）

- `radxa-linkr-debuggerctl --json doctor`: `ok=true`，CLI 0.3.0。
- `GET /api/v1/status`: `ok=true`，RP2350，USB `ncm-http`，watchdog healthy。
- `GET /api/v1/adc/read`: `ok=true`，返回三路电流与 ADC3。
- `GET /api/v1/tasks/catalog`: `ok=true`，返回 6 个 built-in 任务。
- `GET /api/v1/tasks`: `ok=true`，`task_count=0`，stored blob 为空。

## Attempted HIL

1. `POST /api/v1/bootloader`：PASS。固件返回
   `ok=true, command=bootloader, entering rp2350 BOOTSEL in 250 ms`。
2. RPI-RP2 枚举：PASS（通过 `lsblk` 观察到 `sdb RPI RP2350 usb 128M` 和
   `sdb1`；`usb-device 2e8a:000f Raspberry Pi RP2350 Boot`）。
3. Combined UF2 烧录：**BLOCKED**。
   - 沙箱内没有 `/dev/sdb` / `/dev/sdb1` 设备节点。
   - `picotool load -v -x ...` 返回：RP2350 device in BOOTSEL but unable to
     connect, maybe try `sudo` or check permissions。
   - `udevadm trigger` 被 `Permission denied`。
   - 在 `/tmp` 尝试 `mknod` 被 `Operation not permitted`。

## Remaining State

- 板子当前仍停留在 ROM BOOTSEL，`http://172.29.203.1` 不可用。
- CDC ACM fallback：`/sys/class/tty/ttyACM2` 可见
  （`cdc_acm`，product `2fe3:db01`），但沙箱没有 `/dev/ttyACM2` 节点，
  MCP `http://127.0.0.1:8787/mcp` 未运行，因此无法执行 CDC 检验。
- HIL 结论：**BLOCKED**；本次未烧录新固件，也未完成 HTTP/WS、任务、
  CDC 或 BOOTSEL 恢复验证。

## Required Next Action

需要在宿主环境物理复位/重新上电板子，并用有权限的 `picotool` 或
`udisksctl` 将上述 combined UF2 写入 RPI-RP2，然后恢复 HTTP 后重新执行 HIL。
