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

示波器中相邻的活动电流通道之间，在表头行和全部六行图形行上都用一个未着色空格
单元格隔开。三路活动通道因此紧贴并排、没有边框；四种规范宽度下其内容列宽与
零号 gutter 列固定为：W=47 时 15/15/15、gutter 在 15 和 31；W=48 时 16/15/15、
gutter 在 16 和 32；W=80 时 26/26/26、gutter 在 26 和 53；W=120 时 40/39/39、
gutter 在 40 和 80。只有一路活动通道时占满整行、不放 gutter。当终端太窄、放不下
全部通道加 gutter 时，gutter 收起、通道平铺整行。完整几何契约（包括不折行与
列宽严格对齐）写在 [cmd-ng/DESIGN.md](../../cmd-ng/DESIGN.md) 第 7 节。

页签栏提供三个功能页：

- **Controls** — 实时 Power、Switch 和 GPIO 控制
- **Saved Config** — 选择、保存或清除固件持有的配置快照
- **Status** — switch 的 desired/actual 路由、监控状态与错误

每个可见页签标签都是独立的鼠标命中区域：活动标签的命中区含其两侧各一格反白
padding，非活动标签的命中区只有标签本身，相邻标签之间的两个空格不属于任一标签。
仅当左键 Down 落在非活动标签上才会切换页面；同样动作落在活动标签上、任一两空格
上或任何其它鼠标事件上都是空操作。Tab/Shift+Tab 在键盘侧保持同样的优先级。

Controls 的 Power 与 Switch 使用固定列（`TYPE`、`NAME`、`STATE-ROUTE`、`LIVE`、
`MODE`、`DESCRIPTION`）；GPIO 使用紧凑连接器行，每行最多两个可独立选择的 cell。
MODE 显示 TUI 实际策略，因此固件广告的每个 switch 都显示 `confirm`。类型和顺序仍
保留三个逻辑分组：

- **Power** — 最新固件 `power_outputs` 目录中的全部可控输出，并保持固件返回顺序
- **Switch** — 固件广告的动态路由，包括 SD（`target` / `usb-reader`）、TF 写保护
  （`writable` / `protected`）、USB（`pc` / `target`）和 VIN（`1.8v` / `3.3v`）
- **GPIO** — 只按固件返回的 `layoutGroup/layoutLabel/layoutRow/layoutColumn`
  metadata 分组的安全引脚

Saved Config 同样每个条目占一行，稳定显示选择、ID、类型、当前值、保存值、风险
和应用状态。Status 每行显示一个 switch 或监控字段，并把 desired/actual 固定在
对应列。窄终端会整列移除末尾低优先级信息，不会把一个对象换成多行。

在 Saved Config 页，每个可见条目行独占一个铺满整行的命中矩形，命中目标携带条目
稳定的固件 ID，不是列表索引。表头、徽标、loading、unavailable、error、`(none)`
和空白行都不注册命中。左键 Down 落在可见条目上时，会按当前权威 items 重新解析
该 ID、把焦点切到 Saved Config、把光标移到解析后的行、并切换本地选择状态恰好
一次。Mouse Up、左键 Drag、Moved、滚轮、中键或右键在条目行上都是惰性；落在
已不在权威 items 里的旧行 ID 上也是惰性。Saved Config 确认框打开时，只有它
自己的 `[ Confirm ]` 和 `[ Cancel ]` 按钮接受左键 Down，模态下的条目行、页签
和硬件确认命中全部为惰性。

实时颜色直接显示状态：电源开启为绿色、关闭为深灰色；switch 正常路由为青色、
pending 为黄色、`desired` 与 `actual` 不一致为红色。GPIO 状态写为
`◌ IN LOW`、`◌ IN HIGH`、`○ OUT LOW` 或 `● OUT HIGH`；LOW 为无浅色背景的
深灰色，HIGH 为粗体红色。Power、Switch 或 Saved Config 的当前项使用铺满终端
整行的浅色焦点背景；GPIO 只高亮自己的 cell，兄弟 cell 仍可独立读取和选择。
GPIO 请求执行期间会在对应 cell 后追加粗体黄色的 `[LOW…]`、`[HIGH…]` 或
`[INPUT…]`，按住左键期间追加 `[HOLD…]`；方向和电平继续显示固件最近一次报告的
权威状态，直到动作后的状态刷新完成。

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
| 左键 Down 落在 Controls / Saved Config / Status 页签的可见标签 | 切换到对应页；只有标签本身（或活动标签两侧各一格反白 padding）响应，且仅当落在非活动页签时切换 |
| 左键 Down 落在活动页签或两空格 gap | 惰性，不切换页面 |
| Mouse Up / Drag / Moved / 滚轮 / 中键 / 右键 在页签栏 | 惰性 |
| 上 / 下 | 按可见行移动 Controls/Saved Config，条件允许时保持 GPIO 左右位置；在 Status 中滚动 |
| 左 / 右 | 在同一投影行的兄弟 GPIO cell 间移动 |
| PgUp / PgDn、Ctrl+U / Ctrl+D、`[` / `]` | 前后移动三个渲染行/条目 |
| Space / Enter | 激活选中控件或切换当前 Saved Config 条目的选择状态；对选中 GPIO 为惰性 |
| GPIO 上的 `l` / `L` | 将选中 GPIO 驱动为 LOW |
| GPIO 上的 `o` / `O` | 将选中 GPIO 驱动为 HIGH |
| GPIO 上的 `i` / `I` | 将选中 GPIO 恢复为输入 |
| GPIO 上的 `0` / `1` | 惰性，不是备用动作绑定 |
| 左键点击 Power/Switch 行 | 只选择并激活对应控件 |
| 左键 Down 落在可见的 Saved Config 条目行 | 切到 Saved Config 焦点、把光标移到该行、并切换本地选择状态恰好一次 |
| Mouse Up / Drag / Moved / 滚轮 / 中键 / 右键 在 Saved Config 行 | 惰性 |
| 左键 Down 落在当前权威 items 已不包含的旧 ID 行 | 惰性，不会重新指向当前占据该位置的新条目 |
| 左键点击 GPIO | 选择该引脚；600ms 前释放后进入 220ms 双击等待窗，窗口到期才驱动 LOW；按住到 600ms 一次性驱动 HIGH；等待窗到期前同一引脚第二次按下恢复输入且无瞬态 LOW |
| 中键 / 右键点击 GPIO | 惰性 |
| `g` | 跳到第一个 GPIO |
| `c` | 在 Controls 与 Saved Config 间切换 |
| `p` / `r` | 暂停轮询 / 立即请求刷新 |
| Saved Config 页的 `s` / `x` | 保存已选条目 / 清除已保存快照 |
| `q` / Ctrl+C | 退出并恢复终端状态 |
| Esc | 从 Saved Config 返回，或关闭当前错误/确认框 |

页签切换只响应可见标签范围内的左键 Down。Mouse Up、Drag、移动、滚轮、
中键/右键，以及 Saved Config 确认/错误或硬件确认状态下的页签输入均为惰性。

底部固定行是当前页键位栏，每个可见单元由青色键块和操作标签组成；剩余宽度不足
时会整段省略，不会截断半个操作。Controls 与 Saved Config 会自动滚动以保持
当前行可见；Status 由导航键直接滚动，且三页分别保留自己的滚动位置。80x24 下
把双列 GPIO cell 分别计数时，Controls 首屏至少暴露 14 个硬件对象；120x32 下在
板卡提供足够对象时至少暴露 21 个。

直接切换电源或固件广告的任一 switch，第一次操作都只会打开居中的红色边框
确认框。TUI 把这套三秒确认门控无差别套到所有固件广告的 switch 上，不再看固件
`requires_confirm` 标志，因此即使固件把它归为安全项，TUI 仍会打开确认框。
鼠标点击会先选中 switch 整行，确认框打开后该整行焦点在所有不被模态遮住的 cell
上仍保持合成 `accent.select` 背景，直到本次确认结束。第一次操作不会发送请求。
需严格早于三秒截止（`now < started + CONFIRM_TIMEOUT`）再次按 Enter/Space 或
点击 **Confirm**，Esc 或 **Cancel** 取消。达到或超过三秒截止才到达的确认按键
或点击按超时处理，发出该命令的 `timeout_message` 而不是执行硬件动作；该判定
在确认事件自身完成，不依赖下一次渲染或 poll tick 先观察到超时。一次有效确认只
路由一次；取消或超时均不路由。固件 `requires_confirm` 数据仍由 Saved Config、
API 与 CLI 使用；TUI 在这条策略里不会硬编码任何生产 switch 名称、路由或 pin。

本次改动只在 TUI 内的激活策略与渲染上生效。Web UI、固件、HTTP API、
persistent-configuration 线协议、以及非 TUI 的 CLI 命令（`power`、`switch`、
`gpio`、`config`）均保持既有语义不变。

MASKROM 与 EDL 来自固件目录的普通自动化任务，不是 TUI 控件。请通过通用
`task` 命令或 Web Automation 工作区列出并执行；TUI 自身不保存任何恢复
配方。

GPIO 使用明确的直发操作而不是 toggle：`l`/`L` 驱动 LOW，`o`/`O` 驱动 HIGH，
`i`/`I` 恢复输入；Enter、Space、`0`、`1` 对 GPIO 均为惰性。直接键解码器只接受
小写 `l`/`o`/`i` 且不带任何修饰键，或大写 `L`/`O`/`I` 且修饰键恰好为 `Shift`；
任何其它修饰键（Ctrl、Alt、Super）以及大小写与 Shift 失配的组合都返回无意图，
不会触碰既有手势状态，也不会派发任何硬件请求。鼠标左键遵循确定性
手势：600ms 前释放后进入 220ms await-second 窗口，窗口到期且没有第二次按下时才
驱动 LOW；按住到 600ms 一次性驱动 HIGH，同一引脚在 await-second 截止前第二次
按下恢复输入且不会产生瞬态 LOW；中键、右键始终惰性。匹配的 Up 到达或超过
600ms 时立即派发 HIGH。await-second 到达或超过 220ms 时的第二次
Down 会先为原始引脚派发到期 LOW 并消费该次 Down，下一次新的 Down 才开始新手势。
按住期间引脚 cell 会显示粗体黄色的 `[HOLD…]` 标记，直到手势结束。同一终端 cell
内的 Moved 或左键 Drag 报告保持手势有效；只有跨入不同 pin、终端列或终端行的
报告才会取消手势。终端 `Resize` 是 TUI 的强制重绘边界：Resize 到来时取消任何
进行中的 GPIO 手势（同时清掉初始左键 Down 与 AwaitSecond 状态）、整体清空
`hit_map` 让旧命中矩形失效，并强制重绘后再消费下一个排队事件；Resize 之后
排队的鼠标事件按新几何评估、不会落在旧 cell 上。Esc、切页、暂停、任何确认框
或错误状态、GPIO 任务进入 pending、或引脚从固件快照中消失也都会静默取消手势。
GPIO 操作不需要确认，也不会乐观更新显示；TUI 会先显示在途动作，再刷新固件权威
状态。每个半行 GPIO 命中区域只属于一个引脚，没有 GPIO 的空半行不响应操作。

## 多实例

TUI 界面会即时响应输入，并每两秒通过 HTTP 轮询一次调试板状态。可以同时开多个实例，互不干扰。

## 高频采集

高频 ADC 采集用 CLI 的 `adc record`——它走独立的 websocket，不经过 TUI。详见 [CLI 参考](cli.zh-CN.md#录制)。
