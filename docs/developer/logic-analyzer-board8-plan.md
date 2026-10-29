# BOARD8 数字采集实施前审查

状态：**方案已做源码审查，尚未实现；不构成板级验收结果。**
本轮不执行 HIL、刷机或串口操作，不把现有五路 Web 选择器视为八路功能完成。

## 需求与单一硬件来源

正式集合是八路数字高低电平，不是三路模拟波形：

| channel ID | 信号 | pad | 物理接点 |
| --- | --- | --- | --- |
| 0–4 | GP12–GP16 | GP12–GP16 | J8 pin 1–5 |
| 5 | ADC1 | GP27 | J8 pin 8 |
| 6 | ADC2 | GP28 | J8 pin 7 |
| 7 | ADC3 | GP29 | J8 pin 6 |

以上硬件事实由现有 board overlay 的连接器 contact 节点表达，不在 Web/CLI
新增平行 pin map。J13 保留物理布局背景，不加入这八路目录。
移除原 GP10/11/17–21 的采集入口与板级特判，不保留未发布旧模式兼容路径。

## 实施阻塞项

1. `linkr_debugger_sigrok_linkr.c` 仍以 `10 + channel` 映射 pad。
   改 Web allowlist 不能使 GP27–29 进入实际采集。
2. `linkr_debugger_logic_analyzer.c` 当前按整 lane span 配置 pad；只选
   GP12–16 也可能接管 GP10/11、暂停 J4。采样范围不等于允许改写的 pad 集合。
3. 当前生产代码已经为现有 FAST8/WIDE11 整个物理 lane 接入 generation
   租约，公共 GPIO control 也会拒绝冲突操作；这不是 BOARD8 所需的
   selected-pad 租约。BOARD8 仍必须把租约收窄到目录选中的 GP12–16，且保证
   GP27–29 只读 tap 不进入任何 pad 写入或恢复路径。
4. Web 选择器依赖 SafeGpio 身份；ADC1/2 没有此身份。不能为了选择采集通道，
   给 ADC1/2 虚增 `controllable`、输出能力或 GPIO 控制别名。

## 推荐的最小契约

- 在现有 DT contact 加入 `capture-channel`，ADC1/2 补充仅供硬件描述的
  `gpio-pin`，ADC contact 标记只读 tap。由 board model 生成唯一目录，校验
  channel ID 连续且唯一、pad 唯一、接点有效；不依赖子节点遍历顺序。
- 目录至少包含 `{channel_id, gpio_pin, connector_order, pin_number,
  read_only_tap}`。Web 消费 CAPS，用连接器顺序及脚号关联物理图，不解析标签
  或 `GPxx` 文本推导硬件映射。缺失目录时不回退到旧常量列表。
- 保留 v2 framing 和现有 CONFIG 字段；唯一模式改为 `BOARD8`（mode ID 3），
  旧 mode 1/2 明确拒绝。CAPS 替换为 BOARD8 能力与目录，更新控制响应上界；
  不新增并行 CONFIG_V3 或旧 CAPS parser。
- CONFIG mask 与 trigger 使用目录 ID；DATA payload 按选中 ID 升序压紧。
  例如选择 ID 0/5/7，mask 为 `0xa1`，payload bit0/1/2 对应 GP12/27/29。
  保存的 capture 必须带明确通道顺序，不用 `pin_base + index` 补造标签。

## PIO、内存与所有权

- 复用双 lane packed/ring 管线：lane A base12、有效输入数5，仍为 IN8 /
  autopush32；lane B base27、输入数3，为 IN3 / autopush30。
  全选数据为 `(A & 0x1f) | ((B & 7) << 5)`。
- RP2350 SDK 的 `sm_config_set_in_pin_count` 会将未选输入位置读为零；
  不能把 IN8 改成 IN5 后继续沿用旧 word 几何。保留现有原始 arena，
  删除旧地址专用 FAST8/WIDE11 快路径与 exact-WIDE11 协商回退。
- START 在 control lock 内原子检查并取得 selected-pad mask＋generation 租约。
  已选 GP12–16 为输出时返回 busy，不偷偷改方向；租约内所有公共 GPIO 重配置
  路径拒绝冲突，未选 GPIO 保持可用。prepare 失败、取消、断线、超时及 teardown
  都释放；旧 generation 不得释放新租约。
- GP27–29 是只读数字 tap：不经过 `gpio_pin_configure`、`pio_gpio_init`、
  PINDIRS 或恢复 SIO 的旧清理流程；保持 ADC mux、数字输入及无内部拉电阻，
  不驱动这些 pad。SDK 明确 PIO 输入不依赖输出 mux，但要求 pad input enable。
- ADC0/GP26、PD 与 I2C 不动。新目录不含 J4，LA 不再暂停/恢复 J4。
- 八路不等于旧 FAST8 的时序/容量。双 lane ring 容量按现有几何计算，
  不能继承旧 FAST8/WIDE11 的最高速率或历史 HIL 性能声明。

## 实施与验证顺序

1. 冻结 DT 目录、CAPS 编码与压紧位序 fixture。
2. 接通固件目录、selected-pad 租约和 PIO planner，移除旧板级映射。
3. Web 通过目录启用 ADC1–3；协议解码与导出消费保存的通道顺序。
4. 同步仓内 C、TS、Python 生产者/消费者、离线测试和当前参考文档。
   失配的仓内 sigrok v1 驱动入口须整体更新或移除，不能只换引脚名。

离线最低验证：八路 one-hot、跨 lane 稀疏位序、4/10/20 样本边界、ring wrap、
两 lane 的 trigger/pretrigger；已选输出 busy、未选输出不变、异常释放；
ADC tap 全生命周期不出现 pad 写操作；CAPS/CONFIG 拒绝旧模式和非法目录；
Web 不依赖 SafeGpio 也能选择 ADC1/2，且解码/导出不误标通道。

测试纳入现有 C/Python runner、Web runner 和注册检查；固件只运行完整
`make firmware`，不做单对象检查。离线通过后仍只能声明模型/契约/构建通过。
未来获准的 HIL 才能核验实际 pad mux、数字门限、双 lane 相位、最高速率、
ADC 精度影响及 J4/PD/I2C 不受扰；本轮不执行。

## 2026-10-23：本轮 UI/串口离线验证

此结果仅覆盖当前未提交的 UI/串口改动，**不是 BOARD8 实现或硬件验收**：

- 统一全宽波形、同轴协议注释，移除独立解码 tab；大样本路径缓存、解码错误
  显示、异步结果失效及 StrictMode 采集生命周期均有回归覆盖。
- 串口覆盖本地布局/字体适配、隐藏保留尺寸、隐藏换字体后重新度量、标准 CPR
  与 CSI 18t 查询；保留单一响应器、DTR、背压和自动化门禁。
- 完整 Web：84 个 Vitest 文件、953 项通过；生产构建通过。
- 仓库 workflow/gate/registration：300 项通过，三个 checker 通过。
- 完整 `make firmware` 通过：应用 Flash 838084/847832 B，RAM
  499248/532480 B。仅构建，未刷入设备。

Web 选择器仍暂限 GP12–16；ADC1–3 数字采集、BOARD8 CAPS 目录、旧采集映射
移除及 BOARD8 selected-pad 租约尚未实施。现有整 lane 租约已经保护当前
FAST8/WIDE11 路径，但不能外推为 BOARD8 完成。测试不证明真实 UART 对端
`resize`、数字门限、实际采样时序或八路硬件能力。本轮没有 HIL、设备访问或提交。

随后用户授权进行当前 UI/串口 HIL，结果见
[2026-10-23 实机记录](../testing/results/2026-10-23-ui-la-j4-hil.md)：
串口尺寸查询及有限采集通过限定测试，但五路 100 kHz 连续采样发生 overrun，
完整验收未通过。BOARD8 与八路 pad 租约仍未实施。
