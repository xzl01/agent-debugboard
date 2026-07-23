# Ring Buffer 实现差距分析

## 摘要

**目标**：实现文档描述的 DMA 硬件 ring buffer 架构（单通道 + ring wrap + write_addr 轮询）

**现状**：使用 ping-pong 双缓冲 + DMA 回调重载（~6% CPU，125 MHz 受限）

**根本原因**：Zephyr DMA 驱动与 Pico SDK 的 `channel_config_set_ring` 冲突

**已验证**：100 kHz - 100 MHz 全速率工作，触发功能正确

**待解决**：125 MHz 持续流式、硬件 ring wrap、0% CPU 开销

---

## 理想实现（文档描述）

### DMA Ring Buffer 架构

```
PIO SM1 (in pins, 32)
    │
    ▼
DMA Channel ──▶ Ring Buffer (64KB, hardware wrap)
    │              │
    │              ▼
    │         write_addr 轮询 (100µs)
    │              │
    │              ▼
    │         软件读取 + 压缩 + 发送
    │
    └── UINT32_MAX 连续传输，永不停止
```

**关键特性：**
- 单 DMA 通道 + `channel_config_set_ring(true, 16)` 硬件环绕
- `UINT32_MAX` 传输计数，DMA 永不停止
- `dma_hw->ch[n].write_addr` 轮询读取写位置
- 0% CPU 开销（纯硬件）
- 自然支持 pre-trigger（ring buffer 天然存储历史数据）
- 125 MHz 全速持续采集

---

## 当前实现（ping-pong 模式）

### 实际架构

```
PIO SM1 (in pins, 32)
    │
    ▼
DMA Channel ──▶ Buffer A (8KB) / Buffer B (8KB)
    │              │
    │              ▼
    │         DMA 完成中断
    │              │
    │              ▼
    │         回调重载 DMA + 提交 work
    │              │
    │              ▼
    │         work handler 压缩 + 发送
    │
    └── 每 1024 样本重新触发，~75µs 开销
```

**实际特性：**
- ping-pong 双缓冲 + DMA 回调重载
- 每 1024 样本触发一次中断
- 回调中重新配置和启动 DMA
- ~6% CPU 开销
- 独立的 pre-trigger 缓冲区（4096 样本）
- 100 MHz 持续，125 MHz 只能突发

---

## 差距对比表

| 维度 | 理想实现 | 当前实现 | 差距 |
|------|---------|---------|------|
| **缓冲区大小** | 64KB (65536B) | 16KB (2×8KB ping-pong) | 4× |
| **DMA 模式** | 单通道 + 硬件 ring wrap | 双通道 ping-pong + 回调重载 | 完全不同 |
| **传输计数** | `UINT32_MAX`（连续） | 1024（每次重载） | 不连续 |
| **ring wrap** | `channel_config_set_ring(true, 16)` | 无 | 缺失 |
| **CPU 开销** | 0% | ~6% | 高 6% |
| **125 MHz** | 持续流式 | 突发 512 样本 | 受限 |
| **Pre-trigger** | ring buffer 天然支持 | 独立缓冲区 4096 样本 | 受限 |
| **overrun 检测** | 硬件支持 | 软件检测 | 受限 |

---

## 根本原因

### Zephyr DMA 驱动与 Pico SDK 冲突

1. **`dma_start()` 覆盖配置**
   - Zephyr 的 `dma_start()` 调用 `dma_channel_configure()` 写入所有寄存器
   - 无法在 `dma_start()` 后添加 ring wrap

2. **ISR 冲突**
   - Zephyr DMA 驱动用 `IRQ_CONNECT` 注册 `dma_rpi_pico_isr`
   - `irq_connect_dynamic` 需要 `CONFIG_DYNAMIC_INTERRUPTS`
   - `irq_set_exclusive_handler` 会 panic（已有 handler）

3. **通道分配冲突**
   - Zephyr 用 `dma_context.atomic` 位图
   - Pico SDK 用 `_claimed` 位图
   - 两个系统可能分配同一通道

4. **Zephyr DMA ISR 禁用中断**
   - `dma_rpi_pico_isr` 在每个 block 完成后禁用通道中断
   - 破坏连续 DMA

---

## 尝试过的方案

| 方案 | 结果 | 原因 |
|------|------|------|
| Pico SDK `dma_channel_configure` + ring wrap | ❌ 崩溃 | 与 Zephyr DMA 驱动冲突 |
| Zephyr DMA API + `source_burst_length` hack | ❌ 无数据 | `dma_start()` 覆盖配置 |
| `dma_start()` 后修改 `ctrl_trig` | ❌ 无效 | DMA 已启动，修改不生效 |
| `dma_claim_unused_channel` + Pico SDK | ❌ 崩溃 | 与 Zephyr 通道分配冲突 |
| 修改 Zephyr DMA 驱动加 ring wrap | ❌ CPU 3230% | DMA 配置异常 |
| Zephyr `ring_buf` API | ❌ 崩溃 | `ring_buf_get_finish` 参数错误 |
| `CONFIG_DYNAMIC_INTERRUPTS` + `irq_connect_dynamic` | ❌ 未测试 | 需要进一步验证 |

---

## 可行方案

### 方案 A：修改 Zephyr DMA 驱动（推荐）

在 `dma_rpi_pico_config()` 中添加 ring wrap 支持：

```c
// 通过 source_burst_length 传递 ring wrap 大小
if (dma_cfg->source_burst_length > 32U) {
    uint32_t ring_bits = dma_cfg->source_burst_length - 32U;
    channel_config_set_ring(&data->channels[channel].config, true, ring_bits);
}
```

在 `dma_rpi_pico_start()` 中使用最大传输计数：

```c
if (ring_size > 0U) {
    transfer_count = 0x0FFFFFFFU;  // 最大 28 位
}
```

**问题**：之前尝试导致 CPU 3230%，需要进一步调试。

### 方案 B：禁用 Zephyr DMA 驱动

完全使用 Pico SDK DMA API：

```c
dma_claim_unused_channel(true);
dma_channel_get_default_config(chan);
channel_config_set_ring(&cfg, true, 16);
dma_channel_configure(chan, &cfg, ...);
dma_channel_start(chan);
```

**问题**：与其他 DMA 用户冲突，`dma_claim_unused_channel` 可能 panic。

### 方案 C：使用 `CONFIG_DYNAMIC_INTERRUPTS`

启用 `CONFIG_DYNAMIC_INTERRUPTS=y`，用 `irq_connect_dynamic` 注册 DMA IRQ handler。

**问题**：未充分测试，可能与其他中断冲突。

### 方案 D：接受当前方案

继续使用 ping-pong 模式：
- 100 kHz - 100 MHz 持续流式
- 125 MHz 突发 512 样本
- 独立 pre-trigger 缓冲区

**问题**：不满足文档要求的 ring buffer 架构。

---

## 下一步建议

1. **调试方案 A**：修改 Zephyr DMA 驱动，找出 CPU 3230% 的原因
2. **测试方案 C**：启用 `CONFIG_DYNAMIC_INTERRUPTS`，验证 `irq_connect_dynamic` 是否可行
3. **如果都不行**：接受方案 D，更新文档反映实际限制
