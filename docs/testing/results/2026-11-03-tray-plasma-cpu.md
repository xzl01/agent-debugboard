# 2026-11-03：Tray 引发 Plasma 高 CPU 的定位与桌面验证

## 结论与范围

Linux KDE Plasma 6.6.6 / Wayland 下，旧 Tray 即使没有采集或串口活动也持续发布
图标，并轮换 `IconThemePath`。受控暂停与替换版本的对照支持将主要额外开销定位到
这条桌面图标发布链，而不是板端操作。

最初的全静态版本同时删掉中心心跳，不满足用户需求。最终保留用户明确要求的
**原 800ms 双脉冲**：亮 200ms、暗 100ms、亮 100ms、暗 400ms；其他装饰动画
仍关闭。Linux 使用安全 AppIndicator 发布有限、固定名称的状态 PNG，文件创建后
不再改写，心跳在两个已缓存名称间切换，不更换主题路径。单次 GLib timer 唤醒
下一边沿，避免依赖 Tao Linux 的 `WaitUntil` 或状态探测事件，没有 20ms 轮询线程。

采集使用静态绿色 X，中心继续心跳；结束后恢复最新电源轨/UART 显示。离线、
未知状态立即停止心跳。相同底层状态发布失败后退避一秒，亮暗相位不绕过退避。

本次是桌面性能及状态通知验证，**不是板级 HIL**。没有重启 Plasma、刷机，
也没有执行电源、PD、GPIO 或采集硬件写入。现有 100ms 状态探测与服务健康逻辑未改。

## 环境与构建身份

- 源码基线：`c614331`，叠加 Tray 源码/测试、单次唤醒模块、Host README、
  Linux 锁内直接依赖及对应 Nix cargoHash 修改。
- 原 Tray PID：1375993；Plasma PID：3350，全程未替换 Plasma。
- 原可执行文件 SHA256：`a7441831d95febb1d77247e7e9ef14d3b40f4a2626dcaf00e2b50b4d783b2aa8`。
  原安装包不提供精确源码 revision，不宣称其与基线逐字节同源。
- 最终带心跳 release SHA256：`d46c7e8e985cbb41351b781b6e53662b5d93aa311e4f444f2e3b7c144cca05ef`。
- 经用户明确授权重启本机 Tray/Host，最终 PID 为 2825623；端口 8787、板卡 URL
  `http://10.123.5.149/` 和原服务 Web 根目录保持，未安装或修改用户 Nix profile。
- 其他 J4/Web 未提交差异与开始时的二进制 patch 完全一致，没有混入本轮构建快照。

## CPU 与 D-Bus 对照

CPU 百分比按一个逻辑核为 100%；这些是短时桌面窗口，不是隔离的通用性能基准。

| 场景 | 采样窗口 | Plasma 平均 CPU | 图标通知 |
| --- | --- | --- | --- |
| 原 Tray，真实空闲 | CPU 8s / D-Bus 12s | 80.25% | `NewIconThemePath=59`，`NewIcon=59` |
| 原 Tray 暂停 | 10s | 1.90% | 首秒残留 16%，其后各秒 0–1%；随后恢复原进程 |
| 恢复原 Tray | 3s | 75.00% | 原高频路径恢复 |
| 全静态中间版，真实空闲 | 15s | 2.00% | 零通知，但中心不闪，不作为最终功能方案 |
| 固定双图名称原节奏探针 | 12.13s | 5.83% | 61 次 `NewIcon`，零主题路径通知，未改写 PNG |
| 最终带心跳 release，真实空闲 | 15s | 3.20% | 76 次 `NewIcon`；零主题路径通知；PNG hash/mtime 不变 |
| 最终版模拟活动前空闲 | 8.10s | 7.62% | 40 次 `NewIcon`，只在两个固定名称间切换 |
| 最终版模拟采集活动 | 12.14s | 6.33% | 60 次 `NewIcon`，只在该采集状态的两个名称间切换 |
| 最终版模拟活动后空闲 | 8.10s | 7.12% | 40 次 `NewIcon`，恢复普通状态的双图名称 |

最终真实空闲窗口 Tray 自身平均为 1.20%。正式二进制四个心跳边沿的周期平均为
**0.8000259 秒**（观测最小 0.798925、最大 0.800775 秒），不是只验证纯 clock
或独立探针。用户在切换后确认中心双闪已恢复。模拟活动阶段同时有正式 Tray 与
另一个模拟 Tray 两个心跳指示器，因此该窗口 CPU 不能直接视为单个 Tray 的开销。
所有稳定阶段及转换中的 `NewIconThemePath` 均为 0。
各次属性采样的当前 PNG 都存在且有有效 PNG 签名；这不等于证明每个微小时刻的
文件可见性或完成全部桌面主题/尺寸视觉矩阵。

中间曾试用固定目录加 2Hz 采集动画：目录通知已归零，但模拟活动仍产生
23 次 `NewIcon` / 12.17s，Plasma 约 25%。最终没有采用该中间策略，也没有用
全静态化替代用户需要的中心心跳。最初脚本的 `pidstat` Average 表头解析错误已保留，修正的是临时
观测器，不是产品断言；该轮在活动测试前终止并清理了自己的进程。

## 模拟活动的隔离边界

使用随机 loopback 端口的 GET-only fake Host/Board，另起独立 XDG 目录下的 Tray，
通过公开状态响应切换 idle/active。生产 Tray 状态解析和真实 AppIndicator/SNI/
Plasma 链路参与；没有向真实板卡建立模拟采集或修改生产返回。
测试只监视按子进程 PID 识别的 SNI sender，结束后该子 Tray exit 0，
监视器及本地 server 均回收。主 Tray/Host 保持运行。

## 验证与剩余边界

- 完整 Host `cargo test --locked --offline --all-targets`：110 项通过，其中 Tray 39 项。
- Host fmt、全 targets clippy `-D warnings`、离线 release build：exit 0。
- 测试注册、文档布局、`git diff --check`：通过。
- 最终隔离快照的完整原生 `nix flake check -L`：exit 0；Host 110 项、CLI 687 项通过。
  早期两个依赖下载中断记录仍保留，没有改上游或关闭校验来绕过。最终新增的
  libappindicator 0.9.0、PNG 0.18.1、GLib 0.18.5 都是原锁中的 Linux 直接依赖，
  没有版本升级；Host cargoHash 按原生构建报告更新为
  `sha256-vxtOKRzKZbknNQ00AHxBNguwgc6QBIGR1MAW4AImeXA=`。
- 当前运行本地带心跳 release；包装版输出已构建到外部证据根的 `result/`。
  用户已安装 Nix profile 仍未替换，从旧安装入口重新启动仍可能回到旧行为。
- 未执行 Windows/macOS 桌面验证；Starting/Offline 等状态的即时转换与去重由
  确定性单元测试覆盖，不扩称为本次实际桌面场景。独立复核已确认 GTK 单次
  唤醒、图标资产生命期、菜单回调沿用与有限 key 映射没有剩余阻断问题。

原始 CPU、D-Bus、状态、构建日志、临时观测脚本及报告：
`/tmp/opencode/tray-plasma-cpu-20261103/`。本轮未提交或推送。
