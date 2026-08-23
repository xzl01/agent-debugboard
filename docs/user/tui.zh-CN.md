# TUI 指南

[English](tui.md)

## 启动

不带子命令运行 CLI 即可启动交互式 TUI：

```sh
radxa-linkr-debuggerctl
```

## 布局

常态界面是无边框的垂直堆叠：两行状态、可选的七行三通道电流示波器、固定页签栏、
当前页表头、高密度数据行，以及一行 htop 式键位栏。示波器由一行表头和六行图形
组成，每个通道按可见峰值独立缩放；无遥测数据时不会预留空白区域。

页签栏提供三个功能页：

- **Controls** — 实时 Power、Switch 和 GPIO 控制
- **Saved Config** — 选择、保存或清除固件持有的配置快照
- **Status** — switch 的 desired/actual 路由、监控状态与错误

Controls 的 Power 与 Switch 使用固定列（`TYPE`、`NAME`、`STATE-ROUTE`、`LIVE`、
`MODE`、`DESCRIPTION`）；GPIO 使用紧凑连接器行，每行最多两个可独立选择的 cell。
类型和顺序仍保留三个逻辑分组：

- **Power** — 最新固件 `power_outputs` 目录中的全部可控输出，并保持固件返回顺序
- **Switch** — SD（`target` / `usb-reader`）、USB（`pc` / `target`）、VIN（`1.8v` / `3.3v`）
- **GPIO** — 只按固件返回的 `layoutGroup/layoutLabel/layoutRow/layoutColumn`
  metadata 分组的安全引脚

Saved Config 同样每个条目占一行，稳定显示选择、ID、类型、当前值、保存值、风险
和应用状态。Status 每行显示一个 switch 或监控字段，并把 desired/actual 固定在
对应列。窄终端会整列移除末尾低优先级信息，不会把一个对象换成多行。

实时颜色直接显示状态：电源开启为绿色、关闭为深灰色；switch 正常路由为青色、
pending 为黄色、`desired` 与 `actual` 不一致为红色。GPIO 状态写为
`◌ IN LOW`、`◌ IN HIGH`、`○ OUT LOW` 或 `● OUT HIGH`；LOW 为无浅色背景的
深灰色，HIGH 为粗体红色。Power、Switch 或 Saved Config 的当前项使用铺满终端
整行的浅色焦点背景；GPIO 只高亮自己的 cell，兄弟 cell 仍可独立读取和选择。

终端宽度不小于 48 列时，同一固件物理行最多放两个 GPIO cell；更窄时每针单行。
metadata 不完整的引脚按固件快照顺序回退显示，host 不会自行推断连接器名称、
pin map、镜像方向或缺失的物理行关系。

Power 目录遵循同一所有权规则。TUI 不在 host 端维护 rail 列表：固件新增的输出会
自动出现，后续状态快照中不存在的输出会自动移除，固件重排目录时焦点仍跟随同名
硬件。`5V_FIN` 由固件分类为输入/电源来源而不是可控输出，因此不会显示为 Power 行。

## 导航

| 按键 | 操作 |
| --- | --- |
| Tab / Shift+Tab | 切换到下一个 / 上一个功能页 |
| 上 / 下 | 按可见行移动 Controls/Saved Config，条件允许时保持 GPIO 左右位置；在 Status 中滚动 |
| 左 / 右 | 在同一投影行的兄弟 GPIO cell 间移动 |
| PgUp / PgDn、Ctrl+U / Ctrl+D、`[` / `]` | 前后移动三个渲染行/条目 |
| Space / Enter | 激活控件或切换当前 Saved Config 条目的选择状态 |
| 点击 Power/Switch 行或 GPIO cell | 只选择并激活对应控件 |
| `i` / 右键点击 GPIO | 将该 GPIO 恢复为输入 |
| `g` | 跳到第一个 GPIO |
| `c` | 在 Controls 与 Saved Config 间切换 |
| `p` / `r` | 暂停轮询 / 立即请求刷新 |
| Saved Config 页的 `s` / `x` | 保存已选条目 / 清除已保存快照 |
| `q` / Ctrl+C | 退出并恢复终端状态 |
| Esc | 从 Saved Config 返回，或关闭当前错误/确认框 |

底部固定行是当前页键位栏，每个可见单元由青色键块和操作标签组成；剩余宽度不足
时会整段省略，不会截断半个操作。Controls 与 Saved Config 会自动滚动以保持
当前行可见；Status 由导航键直接滚动，且三页分别保留自己的滚动位置。80x24 下
把双列 GPIO cell 分别计数时，Controls 首屏至少暴露 14 个硬件对象；120x32 下在
板卡提供足够对象时至少暴露 21 个。

直接切换电源或固件标记需要确认的 switch（如 VIO），第一次操作都只会打开居中的红色边框
确认框，不会改变硬件；需在三秒内再次按 Enter/Space 或点击 **Confirm**，Esc
或 **Cancel** 取消。确认浮层是 TUI 常态界面之外唯一带边框的区域。

MASKROM 与 EDL 来自固件目录的普通自动化任务，不是 TUI 控件。请通过通用
`task` 命令或 Web Automation 工作区列出并执行；TUI 自身不保存任何恢复
配方。

GPIO 与 Web UI 保持一致：主操作会把输入切为输出 HIGH，输出状态下则在 HIGH/LOW
间切换；按 `i` 或右键点击可恢复输入。每个半行 GPIO 命中区域只属于一个引脚，
没有 GPIO 的空半行不响应操作。

本次显示重设计不改变 HTTP 端点、两秒轮询周期、确认/动作语义、硬件默认值或固件行为。

## 多实例

TUI 界面会即时响应输入，并每两秒通过 HTTP 轮询一次调试板状态。可以同时开多个实例，互不干扰。

## 高频采集

高频 ADC 采集用 CLI 的 `adc record`——它走独立的 websocket，不经过 TUI。详见 [CLI 参考](cli.zh-CN.md#录制)。
