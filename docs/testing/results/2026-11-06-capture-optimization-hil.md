# 2026-11-06：指定 Dev 051 的采集优化 HIL

## 结论：部分通过，存在连续采集失败

**GP12–GP16 五路、WS、100 kHz、请求连续5秒：FAIL。**
在约 **4.3791585秒**收到 **407696点／100个DATA帧**后出现 OVERRUN，终止索引407696。
此前索引无gap不构成PASS；没有达到95%持续时间或样本量，也没有发送普通结束STOP。
服务器明确终止后，新采集探针成功，HTTP健康、J4恢复ready。

其余本轮有限采集、ADC/burst隔离、codec7短帧、正常重启及HTTP/CDC BOOTSEL恢复检查
通过各自下述范围。**不声明整体HIL完成、不声明性能无回退或旧overrun已修复。**

该次失败之后的独立定位和修复记录见
[五路overrun根因与复验](2026-11-06-five-channel-overrun-fix.md)；下文原始失败不改写为PASS。

## 授权目标、构建与预检

用户本轮明确授权所贴 Port002／Dev051 的Mass Storage与Vendor接口进行HIL。
操作前重新核对：

| 项目 | 实际值 |
| --- | --- |
| 唯一授权物理拓扑 | `1-7.4.2`，初始bus1/dev51 |
| BOOTSEL VID/PID | `2e8a:000f` |
| BOOTSEL serial／CHIPID | **`F2836E780B95F053`**／`0xf2836e780b95f053` |
| MCU | RP2350 A4、QFN60 |
| runtime NCM | `eth1`，sysfs证明属于同一拓扑 |
| 板卡／主机地址 | `10.42.51.125`／`10.42.51.126`，`/30` |
| 合成完整UF2 SHA-256 | `53a8228c295b6ddb18d38cead9c17d10ce256957ecb7f1089d3e37b8af9e004d` |
| 构建资源 | Flash822340/847832 B，RAM482440/532480 B |

这是本轮新板身份，不沿用此前其他序列号。未访问另一板`1-1.4.2`或旧固定IP。
现有正式完整构建、源码hash及UF2核对一致，复用已通过离线证据，没有为HIL重复构建。
线刷始终使用冻结的 **combined MCUboot＋应用UF2**；每次读取实时bus/address再绑定
`picotool load -v -x`，load/verify/execute成功。未使用应用`zephyr.uf2`、未整片擦除。

首次启动和后续每次cold reboot／BOOTSEL恢复后，transport ready的第一条host硬件
mutation为`5v_out=on`，第二条fixed PD20000mV，随后验证有效INA226。
首次5Vout为 **5152500µV、−250µA**；后续预检同样通过，最终一次为5196250µV、−750µA。
PD最终available=true、20000mV、last_error=0。小负电流为原始读数，未做客户端零点修正。

## 采集矩阵

未改变GPIO方向或接线，未启用UART／外部信号刺激。公开GPIO目录中的GP12–16均为input。
SINGLE选择GP13（mask`0x0008`）；五路选择GP12–16（mask`0x007c`），不混写为默认GP10
SINGLE或全FAST8。多路后端仍使用其完整物理采样lane，非新增独立五路硬件引擎。

所有表中速率均请求100kHz、无触发、pre=0；bounded目标4096点，continuous目标5秒。
通过项均确认零overrun/error/gap/disconnect、STOP_RESP、新连接1点采集探针及结束后J4ready；
continuous还要求至少95%请求时长和协商样本量。未把runner宽松的`overall_pass`直接当验收。

| 输入／传输 | 模式 | 样本数 | 主机接收窗口 | 结果 |
| --- | --- | ---: | ---: | --- |
| GP13／TCP | bounded | 4096 | 0.006252s | PASS |
| GP13／TCP | continuous | 507904 | 5.047289s | PASS |
| GP13／WS | bounded | 4096 | 0.055815s | PASS |
| GP13／WS | continuous | 491520 | 5.001032s | PASS，见runner问题说明 |
| GP12–16／TCP | bounded | 4096 | 0.005544s | PASS |
| GP12–16／TCP | continuous | 475136 | 5.011758s | PASS，有限静态输入、接近95%样本门槛 |
| GP12–16／WS | bounded | 4096 | 0.062429s | PASS |
| GP12–16／WS | continuous | **407696** | **4.379158s** | **FAIL：OVERRUN** |

bounded窗口只反映START响应后主机drain，不能当硬件采集速率。连续矩阵的输入主要为
静态，单路使用codec5、多路使用codec3，并非高熵codec7带宽测试。

2026-10-23旧板五路WS同速率曾在约3.523秒overrun。两次设备、状态及时间不同，不能
据4.379大于3.523宣称改进；当前根因仍未隔离，本轮只记录失败，没有改固件或自动重跑
失败矩阵直至变绿。

### 临时runner异常分类修正

第一次单路WS连续用例在5.001199秒收到491520点，零overrun/gap，但runner将
`websocket-client.WebSocketTimeoutException`计为disconnect。该异常发生在读取截止时间，
随后**同一连接**成功收到STOP_RESP且新采集探针通过，不能据此认定断线。

保留原失败JSON，在临时适配器中仅将此异常分类为Python `TimeoutError`，沿用原有截止
行为；只复核该用例，其他已通过项不重跑。复核收到491520点、STOP_RESP和probe通过。
没有修改仓库runner、放宽固件检查或隐藏真实OVERRUN；五路WS真实失败原样保留。

## ADC、密集编码与共享arena

### ADC

- HTTP返回固定顺序的八个descriptor；ADC0为`pd_input`、canonical source-side值与
  pad诊断符合固件换算契约，ADC1–3保留native pad值。这不是与仪表比对的模拟精度认证。
- WS接受250Hz、batch18：10个完整batch、**180行**，八通道values／四路voltage及mask
  shape正确，sample sequence无gap、设备时间单调。
- 首末样本设备时间跨度 **747100µs**，wire累计 **8个dropped_samples**。
  因为source/consumer drop共用wire计数，未进一步归因；不写成250Hz零丢样或恒定时序PASS。
- rate251明确返回`invalid_rate`，未静默clamp。

### codec7实际路径

CAPS原始payload为`02013f080148e8011f023f0b0248e8011b`；两模式compression flags
分别31／27，均含bit4。

TCP与WS各执行五路、3点bounded捕获，实际各收到1个 **codec7帧、2B样本payload**，
count=3、mask匹配、解码成功。短静态输入足以使dense胜出，因此能验证板上实际编码／
传输／解码路径，**但不证明4096点高熵payload节省、满链路吞吐或真实采样值bit-exact**。

### arena复用、其他客户端关闭与恢复

分别以TCP owner和WS owner执行WIDE11、100MHz、pre0/post100000的packed burst：

- 两次均精确收到 **100000点、98个DATA帧**，索引连续、无解码/长度/mask错误或overrun。
- 同时保持ADC遥测连接，在START_RESP后、DATA drain前关闭本轮额外创建的非owner WS；
  没有关闭外部用户会话。两个owner路径均正常完成。
- 同一ADC WS恢复有效数据；TCP isolation原有判据通过，结束后J4ready、HTTP健康。
- host时序不能证明关闭动作精确落在DMA写入的某一周期，也没有覆盖每一种producer／
  queued／sender lease交错。没有外部已知波形，不能把完整帧解码当作模拟输入正确性证明。

## 固件资源、控制与生命周期

- 从板卡HTTP实际下载app.js与decoder.wasm gzip，解压hash与正式构建一致；WASM模块
  SHA-256为`3f42e618681520ac5e187769d538b1e5b5d90f90d80922890a6205fed5f74c54`。
  本轮没有在板载页面重跑完整浏览器功能／性能矩阵。
- runtime报告physical.image_reserved_bytes=482440，与构建一致；未宣称新增RAM压力认证。
- 控制CDC只使用同拓扑interface0，`uname`收到Zephyr shell应答；未打开另一板或发送J4目标输入。
  这是CDC控制命令能力验证，不是人为切断HTTP后的完整故障切换实验。
- HTTP normal reboot确认uptime归零、正常runtime返回，没有把它当作BOOTSEL。
- HTTP bootloader与CDC `bootloader`分别进入ROM；每次核对相同新serial后恢复同一combinedUF2，
  load/verify/execute及启动预检均通过。
- 生命周期脚本在尚未发送CDC bootloader前因缺`fuser`退出；通过临时Nix提供工具后，仅续跑
  未执行的CDC阶段，没有重放已通过的normal reboot或HTTP bootloader。
- 未测物理BOOTSEL按键、OTA、J4高速双向收发或无独立generator的多路高熵矩阵。

## 收尾与证据

最终仍是本轮板卡`10.42.51.125`在线、PD20V、5Vout使能；J4ready、默认115200 8N1，
queue0、sink_count0；watchdoghealthy/armed，没有活动采集或本轮遗留WS连接。
未保存／清空持久化配置，未改其他电源轨、路由或GPIO方向。未提交或push。

原始身份、请求、预检、picotool日志与JSON结果位于
`/tmp/opencode/capture-hil-2026-11-06/`：`capture-matrix-initial.json`保留初次runner失败，
`capture-matrix.json`保留复核与真实五路失败，`adc-burst.json`、`ws-owner-burst.json`、
`served-assets.json`、`lifecycle.json`记录其余证据。临时脚本绑定目标IP与NCM源地址，
运行前／连接时复核物理拓扑；没有使用脚本里的旧默认板地址或自动串口刺激入口。
