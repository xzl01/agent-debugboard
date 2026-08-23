[English](openocd.md)

# OpenOCD / JTAG

Radxa Linkr Debugger 可以和 OpenOCD 配合使用：Linkr Debugger 负责目标板
供电和恢复控制，板载 CH347F 负责目标板 JTAG/SWD。

## 架构

- **Linkr Debugger** — 提供电源控制、启动模式选择和目标板恢复功能。
- **CH347F** — 直连目标调试口。RP2350 不在 JTAG/SWD 数据链路中，
  也不会将自身模拟为 CMSIS-DAP 或 JTAG probe。

## 安装与验证

安装 OpenOCD 后确认版本：

```sh
openocd --version
```

## 给目标板供电

启动 OpenOCD 前先给目标板供电：

```sh
radxa-linkr-debuggerctl power set 5v_out on
```

## 启动 OpenOCD

使用本机 OpenOCD 安装中的 CH347F interface 配置和目标板对应的 target
配置启动 OpenOCD：

```sh
openocd -f interface/<ch347-interface>.cfg -f target/<target>.cfg
```

将 `<ch347-interface>` 替换为 CH347F interface 脚本名称（取决于 OpenOCD
构建版本），`<target>` 替换为目标板配置。

## CH347F 支持

CH347F 支持取决于 OpenOCD 构建版本。如果系统包没有 CH347F interface
script，需要使用 WCH/vendor OpenOCD 构建，或补充匹配的 interface 配置。

## GDB 和 Telnet

OpenOCD 通常暴露：

- **GDB server** — TCP 端口 `3333`
- **Telnet 控制** — TCP 端口 `4444`

连接 GDB：

```sh
target extended-remote :3333
```

## 复位策略

优先使用 OpenOCD 复位命令或目标系统自身的重启路径：

- `reset halt` — 复位目标并立即暂停
- `reset run` — 复位目标并让其运行

当前 CH347 驱动在 SWD 模式下不提供物理 SRST。RP2040 target 配置改用
`SYSRESETREQ`，因此 `reset halt` 和 `reset run` 作用在目标 core 上，而非
板级 reset 线。不要假设 CH347F 提供物理复位。

仅在软重启不可行时，才使用电源输出断电再上电（`power set 5v_out off`
再 `on`）作为硬重启 fallback。

## 完整流程

完整 OpenOCD 工作流和配置详情见
[docs/reference/openocd/README.md](../reference/openocd/README.md)。

## 相关文档

- [Web UI](webui.zh-CN.md)
- [OTA 固件更新](ota.zh-CN.md)
