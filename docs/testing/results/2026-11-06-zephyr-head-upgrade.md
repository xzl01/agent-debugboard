# 2026-11-06：Zephyr main 升级与限定 HIL

## 基线与范围

Zephyr 从 `4269cd60816f0a0f6dc9099247c9f62b359939d9` 升级到
`f3fc249035296b87c75fa808b3abf78829fe9378`，即 2026-11-06 09:38:54 UTC
查询的官方 main 快照。其 `VERSION` 为 **4.5.99 开发版**，不是稳定版；`west.yml`
固定完整 SHA，不使用浮动分支。六个导入模块随该 manifest 更新，MCUboot 为
`725d63c9d5bc1bb572d802e7a768daf504186daf`（2.5.0）。

依赖使用本仓库忽略的本地 west 工作区，父级共享 Zephyr/modules/MCUboot 的 HEAD、
工作树和 west 配置保持不变。本仓库原有暂存内容与其他未提交改动保留；没有提交或 push。
内存复用、codec7、WASM 构建与连续子集提取优化均保留。

## 必需适配

- RP2350 qualifier `m33` 改为 `m33_0`；Makefile、CI、overlay/conf 文件名、测试引用与当前文档同步，历史结果不改写。
- 两个应用的 CMake 最低版本改为 3.28；实际构建使用 CMake 4.1.6、Python 3.13.17、SDK 1.0.1。
- UART1 使用已修复的 `UART1_TX_P10` / `UART1_RX_P11`，移除旧 AUX 宏 workaround，功能号仍为11。
- OTA 依赖改为新的 board Kconfig 符号，避免升级后静默关闭；显式启用新 loopback 前置 `NET_DRIVERS`。
- 新内核结构使四个 WS client 共增加64B；按编译器/链接 map 实测更新精确大小断言
  `28768B`、pre-capture `0xDA80`，不移除断言、DMA 对齐或资源门禁。
- 新 MCUboot footer 使可用应用 Flash 区减少4096B；Flash 上限相应从 `0xCCFD8`
  **收紧**至 `0xCBFD8`，继续保证8KiB余量。RAM 32KiB余量门禁不变。

本地嵌套 `zephyr/` 会被模块自动发现误判为应用的模块，造成 Kconfig 递归。
本机忽略的 `.west/config` 显式传入 frozen manifest 的六个 `ZEPHYR_MODULES`，排除误识别；
没有修改上游源码，也没有改变 CI 外层工作区的布局或 canonical `make firmware` 入口。

## HIL 发现并修复的有限采集回归

初版升级镜像的连续五路采集通过，但独立有限采集在 WS post1/4/4096、TCP post1
和冷启动首次 post1 均无 DATA。CONFIG/START、RUNNING 事件和 STOP 响应仍正常；
不能以连续采集通过替代有限采集验收。

新 `dma_rpi_pico.c` 将 `complete_callback_en` 映射为 **IRQ_QUIET**，旧驱动没有该映射。
原应用为两个单块 burst lane 设置1，因而 DMA 完成却不产生中断，也不会运行唯一的
burst 后处理 work。临时 STOP 只读寄存器快照实测：

```text
done_mask=0 remaining=0 ctrl=0xa80049 intr=0x0
```

RP2350 `IRQ_QUIET` mask 为 `0x00800000`，BUSY mask 为 `0x04000000`，即 quiet=1、
busy=0。不是 PIO 不采样，也不是 WS 队列吞帧。仅在本仓库将两 lane 的该字段改为0：

| 同板、相同临时观测 | post1 / fresh restart |
| --- | --- |
| 原配置，`ceed5771…` | DATA0，失败 |
| 仅关闭 quiet，`f9ae1ddc…` | DATA1，通过 |
| 切回同一冻结原镜像 | 再次 DATA0，失败 |

最终移除了临时寄存器日志，只保留两处配置与解释。没有修改 Zephyr/HAL、增加轮询或
恢复延时、放宽判据或改变缓冲。现有 canonical HIL 的 finite/fresh-restart 用例提供修改前
失败、修改后通过的回归证据。

## 最终产物与离线验证

合成完整 UF2 SHA-256：
`7b7b5815c1378934d2cfbe251309d1b756d4a325b5e9dd08194da801baef9809`。

| 应用资源 | 升级前 | 升级后 |
| --- | ---: | ---: |
| Flash 使用 | 822508 B | 825116 B |
| Flash 区域 | 847832 B | 843736 B |
| RAM 使用 | 482440 B | 485216 B |

Flash 仍空闲18620B，RAM 空闲47264B，分别满足8KiB/32KiB门禁。升级不是体积优化：
Flash 使用增加2608B，RAM增加2776B。采集容量和队列容量未缩减。

- `make firmware`：最终无观测镜像完整 sysbuild、MCUboot 镜像封装和 combined UF2 生成，exit0；签名策略仍为 NONE，不提供安全启动保证。
- `make gates unit-tests`：383项门禁、全部注册 C 模型、115+16项 Python 通过，exit0。
- DMA 适配及观测清理后再跑 `make unit-tests`：exit0；未受影响的门禁证据复用。
- `node --test scripts/check-persistent-configuration-docs.test.mjs`：63项通过，exit0。
- `make pico2-test`：辅助 app 新 target 完整构建195步通过，exit0；未刷写该 app。
- 板端实际提供的 decoder WASM hash 仍为
  `3f42e618681520ac5e187769d538b1e5b5d90f90d80922890a6205fed5f74c54`。

未变的 Web/Rust/Nix 路径复用此前有效验证，不声称本轮重跑了全部 CI 或跨平台矩阵。

## 最终板级验证

仅操作授权 USB `1-7.4.2`、serial **F2836E780B95F053**；每次启动按规范首先
`5v_out=on`、再 fixed PD20，并用有效 INA226 电压/电流完成预检。发现已有 J4 连接时，
曾在 BOOTSEL 请求前停止；用户关闭连接并确认继续后才刷写。

| 最终无观测镜像 | 结果 |
| --- | --- |
| GP12–16，WS100kHz，post0 | 10.012290s，999424点 |
| GP13，WS1MHz，post0 | 30.014055s，29982720点 |
| WIDE11，TCP100MHz有限 burst | 100000点，正常结束 |
| GP12–16，WS100kHz，post1 | 1点，正常结束 |

上述四项均零 overrun、gap、解码错误和断线，STOP/终态及 fresh-restart 检查通过。
HTTP八通道 ADC 查询和100Hz WS遥测的形状/接收检查通过，不据此声明遥测零丢样。
正常 reboot、HTTP BOOTSEL、控制 CDC BOOTSEL、combined UF2 恢复与各次预检通过。

OTA 使用新镜像825156B：上传 SHA 验证、测试启动通过；启动后实测
`current_image_confirmed=false` / `test_marker_present=true`，随后显式确认，变为
`true` / `false`。结束时 watchdog healthy、J4 ready且无订阅占用，板卡保留新固件在线。

这些是无独立外部刺激的限定回归，不代表高熵吞吐、无限时长、浏览器渲染、3M UART
或物理 BOOTSEL 按键均已验收；辅助 Pico2 app 仅编译，未做其硬件测试。

证据根目录：`/tmp/opencode/zephyr-head-2026-11-06/`，包括 `resolved-west.yml`、
`final/`、`final-validation/`、`short-debug/` 的因果切换及 `final-hil/` 的最终实机结果。

## WebUSB 旁路调研

本次没有添加 WebUSB 描述符或接口。桌面 Chromium 保留 landing-page 通知代码，但要求
可信 HTTP(S) URL；普通 `http://10.42.x.x` / `http://linkr.local` 默认被过滤，不能承诺
当前板卡主页插入即提示。HTTPS 产品页可作可选引导，但不是动态本地控制页；Windows
读取该设备级请求还有 WinUSB 限制。规范只允许浏览器选择提示，点击才导航，不自动打开。
参见 [Chrome 文档](https://developer.chrome.com/docs/capabilities/build-for-webusb)及
[Chromium detector](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/chrome/browser/usb/web_usb_detector.cc)。
