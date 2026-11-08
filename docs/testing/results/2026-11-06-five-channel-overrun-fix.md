# 2026-11-06：五路 WS 连续采集 overrun 根因与修复

## 结论

已在用户授权的同一板卡确认：**GP12–16连续五路落入逐样本／逐引脚的generic提取路径，
其消费成本超过100kHz的生产预算，unread逐渐达到安全容量并触发overrun。**
不是平均USB带宽耗尽，也不是本次观察中的长socket send。增加连续升序lane子集的
byte shift/mask路径后，同板五路WS100kHz连续10秒限定回归通过。

## 运行证据

目标始终为USB`1-7.4.2`、serial`F2836E780B95F053`、板卡`10.42.51.125`。
未改变接线、GPIO方向、电源默认或采样率，不使用外部刺激。所有镜像均由完整
`make firmware`生成combinedUF2；每次启动先5v_out、再fixedPD20并验证有效INA。

原镜像同板对照：

| 配置 | 结果 |
| --- | --- |
| 五路WS100kHz | 4.384218s、407892点后overrun |
| 全FAST8八路WS100kHz | 5.013979s、499712点，无overrun |
| 五路TCP100kHz | runner窗口5.009822s、475136点通过；STOP边界附近另有内部ring warning，未作全时序保证 |

失败时现有日志：`max_gap_us=4900`，`max_unread=31060`，超过30720点安全容量；
WS `max_send_us=1400`、`max_drain_us=1500`、`max_qdepth=4`。发送不需要等满四槽，
低帧率也没有触发600次/s发送限额。末态reader追上writer是freeze后的drain，不是
触发时没有backlog。

为定位边界，仅临时对sink write/commit计时并复用现有结束汇总，无逐样本日志：

- 五路再次在4.336776s／403524点overrun。
- 99个chunk：提取总 **4000400µs**、最大 **42900µs**；commit总240700µs、最大2600µs。
- 平均消费约42860µs/chunk；100kHz下每4096点只允许 **40960µs**。
- 时间使用现有uptime ticks，短操作可能量化为0；不把这些值当精密CPU周期profiling。

## 因果切换，而非仅源码推测

旧、新候选保留完全相同的临时计时：

| 同板镜像／五路WS100kHz | 观察 |
| --- | --- |
| 旧提取＋计时 | 4.336776s、403524点，overrun |
| 连续子集快路＋计时 | 5.012133s、499712点，零overrun |
| 切回同一冻结旧镜像 | 4.299273s、399616点，再次overrun |

因此失败可随提取路径的切换消失／复现。未通过扩大ring、修改30720阈值、静默丢样或
放宽runner判据来通过。候选的全八路WS、五路TCP5秒对照也通过。

## 最小修复与最终验收

仅推广现有FAST8 byte-span快路：选中pin必须是同一lane内连续升序子集，按
`(byte >> shift) & mask`生成原紧凑位序；全八路仍用原memcpy，非连续／重排仍generic。
不改DMA、ring大小、队列、协议、触发或资源所有权，不新增heap/scratch。
既有LA模型测试补跨环、长sequence、不同连续子集、精确GP12–16／4096点及OR/AND对照。
功能等价测试不是主机性能阈值测试；性能红灯来自上述真实HIL。

临时计时已从最终源码移除。canonical C/Python runner、333项仓库门禁／三个checker及
完整`make firmware`均exit0；未变的Web/Rust/Nix路径复用此前有效证据。
最终Flash **822508/847832 B**（比修复前增加168B），RAM **482440/532480 B**不变。
最终combinedUF2 SHA-256：
`adc3618fc7f946b38853539b2d72890b140705448198993c1aeed88462fac850`。

已只刷入授权板；最终无计时镜像的五路WS100kHz实测：

```text
stream_elapsed_s=10.0149371
received_sample_count=999424
overrun_events=0
sample_index_gaps=0
data_decode_error_frames=0
reason=ok
```

收到STOP_RESP，新连接1点采集probe通过，HTTP健康。此为无已知外部刺激的有限10秒
回归，不声明更高速率、独立多路高熵或无限时长都已验收；非连续选路仍可能受generic成本限制。
此前HIL报告的失败记录保留，不回写成PASS。最终板保持新固件在线、J4ready，未提交或push。

证据目录：`/tmp/opencode/ws-overrun-debug-2026-11-06/`。`probe/`、`candidate-hil/`、
`reverted-hil/`保留反事实；`final/results.json`及`final-hil/`保留最终命令和实机结果。
