[English](ota.md)

# MCUboot OTA 固件更新

RP2350 固件支持无签名 MCUboot 空中固件更新。

## 安全说明

- **无签名验证。** OTA 路径不验证加密签名。
- **无认证。** 任何持有板子 USB NCM 访问权限的主机都可以提交固件镜像。
- **无安全启动。** 引导加载程序不强制执行信任链。
- **无防回滚。** 没有版本单调性检查来阻止降级。
- **SHA256 仅用于完整性校验。** CLI 计算上传负载的 SHA256 并作为 header
  发送。固件在设备端重新计算哈希并拒绝不匹配的。SHA256 验证完整性，
  不验证真实性或签名。

## 初次安装

MCUboot 固件的初次安装需要使用 ROM BOOTSEL 刷写合并型可启动镜像
（`radxa-linkr-debugger-rp2350.uf2`）。初次安装完成后，后续更新可通过
OTA 使用 MCUboot 格式的应用二进制文件交付。

## 接受的产物

- **使用**：MCUboot 格式的应用 `.bin` — 即
  `radxa-linkr-debugger-rp2350-ota.bin`，来自 sysbuild 应用输出
  `zephyr.signed.bin`。
- **不要通过 OTA 上传** `.uf2` 或 `.elf`。
- 虽然构建文件名包含 `signed.bin`，但本项目配置下是无签名 MCUboot 格式。

## OTA 工作流程

工作流程分三步：**上传** → **测试** → **确认**。

1. **上传** MCUboot 格式的应用二进制文件到板子。
2. **测试** — 请求对新上传的镜像执行测试启动。板子在短延迟后重启。
3. **确认** — 验证测试启动成功后，手动确认镜像。或者等待约 16 秒
   watchdog 健康门槛自动确认。

如果测试镜像未经确认且在未确认窗口期间发生 watchdog 复位，专用 retained
marker 会让 MCUboot 执行回滚到上一个已确认镜像，而不会强制进入 ROM
BOOTSEL。显式 `bootloader` 命令和普通非 OTA watchdog 复位仍正常进入
ROM BOOTSEL。

## CLI 命令

```sh
radxa-linkr-debuggerctl ota status
radxa-linkr-debuggerctl ota upload /path/to/firmware.bin
radxa-linkr-debuggerctl ota test
radxa-linkr-debuggerctl ota confirm
```

### `ota status`

报告当前 OTA 状态、flash 大小和 MCUboot swap type。

状态：`idle`、`uploading`、`verified`、`pending_test`、`rebooting`、`failed`。

### `ota upload PATH`

发送 MCUboot 格式的 `.bin` 文件。CLI 计算 SHA256 并将字节大小和哈希
作为 header 一起发送。

### `ota test`

请求对已验证镜像执行测试启动。板子在短延迟后重启。如果测试启动后
watchdog 报告健康，镜像会在约 16 秒门槛后自动确认。

### `ota confirm`

立即手动确认当前运行的镜像，清除自动确认计时器。

## JSON 输出用于自动化

Agent 或自动化程序推荐优先使用 JSON 输出：

```sh
radxa-linkr-debuggerctl --json ota status
radxa-linkr-debuggerctl --json ota upload /path/to/firmware.bin
radxa-linkr-debuggerctl --json ota test
radxa-linkr-debuggerctl --json ota confirm
```

## Web OTA 卡片

Web UI 在 **高级与恢复** 下也提供 OTA 卡片。它通过同一 USB NCM HTTP API
交付 RP2350 固件更新，不需要额外的主机工具。当 UI 通过 HTTPS 从 GitHub
Pages 提供服务时，需要设备桥接网关（`npm run device-bridge`）。详见
[webui.md](webui.md)。

## MCUboot 回滚

如果测试镜像未经确认且 watchdog 复位，retained marker 会驱动 MCUboot
回滚而非 ROM BOOTSEL。这意味着有问题的镜像不会导致板子无法恢复 —
上一个已确认镜像会自动恢复。

## 相关文档

- [Web UI](webui.zh-CN.md)
- [OpenOCD / JTAG](openocd.zh-CN.md)
