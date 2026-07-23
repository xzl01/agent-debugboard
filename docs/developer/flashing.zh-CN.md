# 刷写

[English](flashing.md)

固件更新有两种方式，取决于板子当前状态。

## ROM BOOTSEL 刷写（初次安装或恢复）

如果板子当前已经运行本固件，可以先让它进入 BOOTSEL，再刷写新的 UF2：

```sh
radxa-linkr-debuggerctl bootloader
picotool load -v -x radxa-linkr-debugger-rp2350.uf2
```

每次修改固件后，都应把这条 BOOTSEL 流程和下方 CDC ACM 串口 shell 的
fallback 路径当作必做验收项；确认串口 fallback 仍然可用后，才能结束该改动。

如果 HTTP/WS 控制面不可用，但 MCU 的 CDC ACM shell 还在，也可以直接在本地
Zephyr shell 里进入同一条 BOOTSEL 路径：

```text
linkr-debugger:~$ bootloader
```

如果板子已经以 `RPI-RP2` 磁盘方式挂载，只需要执行：

```sh
picotool load -v -x radxa-linkr-debugger-rp2350.uf2
```

Linux 下也可以先用 `udisksctl` 挂载 `RPI-RP2`，再复制这一个固定 UF2：

```sh
RPI_RP2=$(udisksctl mount -b /dev/sdX1 | awk -F" at " '{print $2}' | tr -d '[:space:]')
cp radxa-linkr-debugger-rp2350.uf2 "$RPI_RP2/"
```

将 `/dev/sdX1` 替换为实际 BOOTSEL 块设备路径。

如果改用 `RPI-RP2` 盘符拖拽复制，而不是 `picotool`，也只能复制这一个固定产物：

```text
radxa-linkr-debugger-rp2350.uf2
```

## OTA 刷写（初次 MCUboot 安装后适用）

MCUboot 固件初次安装完成后，后续 RP2350 固件更新可通过 OTA 交付。准备好
MCUboot 格式的应用二进制文件后，按顺序执行：

```sh
radxa-linkr-debuggerctl ota upload /path/to/firmware.bin
radxa-linkr-debuggerctl ota test
# 验证测试启动成功后：
radxa-linkr-debuggerctl ota confirm
```

测试启动成功后，也可以等待 16 秒 watchdog 健康门槛自动确认镜像。固件设计为在
未确认测试镜像发生 watchdog 复位后，通过 retained marker 请求 MCUboot 回滚。
该恢复路径的故障注入 HIL 尚未完成，因此必须保留物理 ROM BOOTSEL 恢复路径。

不要通过 OTA 上传 `.uf2` 或 `.elf` 文件。OTA 接收的是 MCUboot 格式的应用二进制文件；
release 产物名为 `radxa-linkr-debugger-rp2350-ota.bin`，它来自 sysbuild 应用输出
`build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin`。虽然构建文件名
包含 `signed.bin`，但本项目配置下它是无签名 MCUboot 格式。
