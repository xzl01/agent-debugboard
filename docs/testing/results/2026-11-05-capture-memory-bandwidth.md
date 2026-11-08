# 2026-11-05：ADC／logic 内存与带宽优化离线证据

## 验证边界

用户授权先安全回收采集内存，再继续带宽优化，并明确要求“禁止非授权hil”。
本轮未获 HIL 授权：没有设备 API、串口、采集、烧录、重启或电源操作。
以下目标构建与 host-model 结果不构成硬件性能、DMA 可见性或无丢样验收。
源码基线为 `ff3acce` 加本轮未提交改动；原有 Tray/Cargo/Nix/.ignore 暂存内容保留。

## Pass 1：安全回收历史预留区

host-stream power capture 只需要 136 B 状态，而旧分区保留 65672 B。
不能仅收紧宏：原 normal Sigrok WS pool 会因此移入 burst DMA lane，导致其他
WS 客户端断连时读写被 DMA 覆盖的锁、FIFO 节点或 lease 状态。

采用经用户确认的安全分离：

- normal pool 的锁、allocation gate、五个队列/lease 节点常驻于 arena 外。
- arena 只容纳 16480 B payload storage；四个 DATA 槽仍各有 4113 B capacity，
  terminal 槽仍为 15 B，未减少发送队列容量。
- 原锁内原子完成 all-free 检查和 gate 关闭；producer-held、queued、sender-held
  都阻止借用。断连不强行回收仍被持有的槽，恢复不清零稳定锁。
- ADC 的 173 行、32 KiB packed ring、WIDE11 十万点 burst、lane/canary/TX
  固定布局和 quiesce/generation 契约保留；没有新增 payload copy 或 heap 分配。

### 目标构建实证

| 项目 | 基线 | Pass 1 |
| --- | ---: | ---: |
| shared arena | 161144 B | 144184 B |
| 新增常驻 normal pool metadata | — | 144 B |
| 固件 RAM image | 499256 B | 482440 B |
| RAM 净减少 | — | **16816 B（16.421875 KiB）** |
| 532480 B RAM 的剩余量 | 33224 B | 50040 B |
| Flash | 835020 B | 834932 B |

新 map 中 arena 位于 `0x20010000`，大小 `0x23338`；常驻 pool 大小为 `0x90`。
保持原 BSS 起点和 32768 B 起点对齐，normal layout 为 95464 B，最终 backing
allocation 仍由 144184 B burst slice 决定。净值不是把 64 KiB 预留差额直接当节省。

### 离线验证

- canonical C／Python offline runner：exit 0，包含新增生产 WS pool 生命周期回归。
- workflow／repository／registration：333 项及三个 checker 通过，exit 0。
- 完整 `make firmware`：exit 0；目标 ARM layout／资源断言通过。
- 定向测试覆盖借用中其他客户端 cleanup、gate/acquire 顺序及 pthread 并发、
  持有槽位、stale generation、拒绝提交后唯一 abort、burst abort 后 drain。
- 五个旧布局／生命周期负变体均被回归拒绝；不以“改完能编译”替代行为证据。
- combined UF2 SHA-256：
  `215b3ee6917eaafb2c7d65a1e9c15d6a5209dcbd442e20294b8d38affeab72e2`。
  **仅构建，未刷写。**

命令、退出码、日志及冻结 map/UF2 位于
`/tmp/opencode/adc-logic-opt-2026-11-05/pass1/validation/`；定向证据与剩余边界见
同级 `../REPORT.md`。本节证明静态资源缩减及离线生命周期行为，不证明硬件性能不变。

## Pass 2：带宽优化

新增 `DENSE_BITS=7` 与 CAPS compression bit4，保持 v2/HELLO flags、帧与 chunk
上限不变。按 mask 低位到高位的紧凑通道顺序，跨样本 LSB-first 打包为
`ceil(sample_count * popcount(mask) / 8)` 字节；高位 padding 必须为零。
接收端还原原有 byte-aligned LE 样本，不改变样本索引、时基、触发或波形格式。

固件先完整验证输入，再只读比较合法的 raw/RLE/palette/dense 长度，最后只写
赢家；保留原地 RLE 前缀可行性检查，禁止输出覆盖未读输入。dense 只在严格更短
时胜出，平手保留原策略，现有 SINGLE 流快路不变。没有新增 heap、scratch 或
持久 RAM。Web 与两套 Python 解码入口同步严格长度／mask／padding 检查；Web
发布 DATA 前还必须匹配成功 CONFIG 的完整 mask，待提交或失败配置不替换快照。

### 公共编码 API 的离线字节实证

测试输入至少含三种值，且旧 RLE 无收益，故不能把已有两值 palette 压缩算作新收益。

| 输入 | 旧最优 sample payload | codec7 sample payload | payload 减少 | 含17B前缀完整帧 |
| --- | ---: | ---: | ---: | --- |
| 五路 × 4096 点 | 4096 B | 2560 B | 1536 B／37.5% | 4113 → 2577 B |
| 十一路 × 2048 点 | 4096 B | 2816 B | 1280 B／31.25% | 4113 → 2833 B |

C、TS、两套 Python 用独立黄金字节验证相同值，例如五路向量
`20 88 41 8a 39 1f`、十一路向量 `00 08 c0 ff 01 58 15 55 49 13`。
另覆盖 k=1..11、奇数尾部、严格拒绝畸形帧、原地／独立输出、移位重叠拒绝、
RLE／palette／dense 平手、旧压缩更优、CONFIG ACK 与 DATA 合并及 u24 索引回绕。

这证明指定输入的线上编码字节减少，**不是 MCU 周期或 USB 实测提速**。
新编码有有界线性只读规划开销；实际 CPU、时延、持续采样及吞吐仍未硬件验证。
用户给出的约 10 Mbps 仅作为预算条件；旧五路 100 kHz、约 3.523 秒 overrun
也没有因此被判定修复。ADC 遥测的 JSON 编码未在本次改变。

### 集成验证与最终构建

- canonical C／Python offline runner：exit 0，Python lane 115＋16 项通过。
- 完整 Web：Node 454 项通过、原有一项条件跳过；85 个 Vitest 文件、1078 项通过。
- Web production build：exit 0，保留现有 bundle 大小警告。
- workflow／repository／registration：333 项及三个 checker 通过，exit 0。
- 完整 `make firmware`：exit 0；Flash **835488/847832 B**，RAM **482440/532480 B**。
  相比基线仍净省 **16816 B RAM**，额外编码未增加持久 RAM；Flash 剩余12344 B。
- combined UF2 SHA-256：
  `4262342d1bc244dbb27f7ee29f31c665400a3dbebcdc2db517a7da6a969482e7`。
  **仅构建，未刷写，HIL 未授权且未执行。**

首次集成 runner 发现新增 pool 测试把 `0x5a` 用于单通道 mask，违反编码输入契约；
修正为八通道 mask 后保留原样本、容量和 lease 断言，未放宽生产校验。
失败日志保留在 `pass2/validation/integration-attempt1-offline.log`。
最终命令与退出码见
`/tmp/opencode/adc-logic-opt-2026-11-05/pass2/validation/results.json`。

预存 `tools/sigrok` native 原型仍为废弃 v1，并非当前 v2 客户端；本次没有执行、
重写或增加其兼容路径，已修正文档的“标准协议／native 集成一致”假声明。

## 复核收口

内存与 codec 两个独立 review gate 均通过，未发现本次范围内剩余 blocking。
协议文档中的 native driver／PulseView 清单已标为未实现规划。客户端解析异常
统一转换为错误事件／关闭连接属于既有边界，本轮不扩大 UX 改造；不匹配 DATA
已在发布前拒绝。本轮未提交或 push，原有其他任务暂存内容保持不变。
