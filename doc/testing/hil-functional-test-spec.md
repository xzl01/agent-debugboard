# HIL 功能测试规范

本文档定义 `radxa-linkr-debugger` 固件与主机控制逻辑改动后的板级 HIL（Hardware-in-the-Loop）功能测试规范。

## 适用范围

以下改动不能只依赖编译/静态检查/unit test，必须完成 HIL 实机功能验证：

- RP2040 / RP2350 固件逻辑改动
- 主机 CLI/TUI 真实控制逻辑改动
- 电源输出、switch 路由、ADC 电流监测、safe GPIO、watchdog、BOOTSEL 相关改动
- 任何新增面向硬件的功能

以下改动通常不需要 HIL：

- 文档、许可证、NOTICE、说明文字
- CI/workflow 解析、脚本语法检查、格式检查
- 不涉及真实硬件交互的 host-side 工具链/依赖更新

## 强制验收要求

固件相关改动必须同时满足：

1. 完整构建固件
2. 实机烧录并确认正常启动
3. 验证 HTTP/WS 控制通路
4. 验证 USB CDC ACM 串口 fallback 路径（USB CDC ACM serial fallback path）
5. 验证 BOOTSEL 进入能力（BOOTSEL entry path）
6. 新增功能补对应 functional test（可行时）

## 标准构建与烧录

### 构建

```sh
west build -p always -b rpi_pico/rp2040 apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
west build -p always -b rpi_pico2/rp2350a/m33 apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
```

按待测硬件选择其中一个完整构建命令。由于两者共用 canonical build 目录，烧录前最后一次
构建必须对应当前连接的 MCU。

### 烧录

```sh
picotool load -v -x build/radxa_linkr_debugger/zephyr/zephyr.uf2
```

或使用 RPI-RP2 拖拽方式：

```text
build/radxa_linkr_debugger/zephyr/zephyr.uf2
```

Linux 下免 root 烧录必须按本规范第 9 节通过 VENDOR 为 `RPI` 发现实际磁盘和分区，
再用 `udisksctl` 挂载；不得固定假设 `/dev/sdX1`。只复制 canonical artifact：
`build/radxa_linkr_debugger/zephyr/zephyr.uf2`。

## 标准 HIL 验证 checklist

默认优先使用 release 发布的 `radxa-linkr-debuggerctl` CLI 执行以下主机侧验收命令。
只有在你正在验证尚未发布的 `cmd-ng` 改动时，才将同一组命令替换为等价的
`cargo run --manifest-path cmd-ng/Cargo.toml -- ...` 源码运行方式。

### 1. 主机诊断与状态

```sh
timeout 5s radxa-linkr-debuggerctl --json doctor
timeout 5s radxa-linkr-debuggerctl --json status
```

验证：

- `schema`
- `ok`
- `command`
- `error.code`（异常时）

### 2. HTTP API 连通性

```sh
timeout 5s curl -fsS http://172.29.203.1:8080/api/v1/status
timeout 5s curl -fsS http://172.29.203.1:8080/api/v1/watchdog
timeout 5s curl -fsS http://172.29.203.1:8080/api/v1/switch
timeout 5s radxa-linkr-debuggerctl --json power list
timeout 5s radxa-linkr-debuggerctl --json switch list
```

G3 额外执行 `timeout 5s curl -fsS http://172.29.203.1:8080/api/v1/switch/vin`；
RP2040 不暴露该 switch。

### 2b. Memory monitoring (Phase 2)

验证 HTTP `/api/v1/status` 返回的 `board_monitoring.memory` 字段形状和 Phase 2 加性对象：

```sh
timeout 5s curl -fsS http://172.29.203.1:8080/api/v1/status | python3 -c "
import sys, json
data = json.load(sys.stdin)
mem = data.get('board_monitoring', {}).get('memory')
print(json.dumps(mem, indent=2))
assert mem is not None, 'memory field must be present'

# Phase 1 legacy root must be present for backward compatibility
p = mem.get('pressure_pct_x100')
assert p is not None, 'pressure_pct_x100 (legacy root) required'
assert 0 <= p <= 10000, f'pressure_pct_x100 must be 0..10000, got {p}'

# Phase 2 additive objects
cp = mem.get('current_pressure')
assert cp is not None, 'current_pressure required'
ap = mem.get('peak_pressure')
assert ap is not None, 'peak_pressure required'

# current_pressure fields
assert cp.get('available') in (True, False), 'current_pressure.available must be bool'
if cp.get('available'):
    cpp = cp.get('pressure_pct_x100')
    assert cpp is not None and 0 <= cpp <= 10000, f'current_pressure.pressure_pct_x100 must be 0..10000, got {cpp}'
    lc = cp.get('limiting_component')
    assert lc in ('system_heap', 'net_pkt_rx', 'net_pkt_tx', 'net_buf_rx_data', 'net_buf_tx_data'), f'unexpected current_pressure.limiting_component: {lc}'
    assert isinstance(cp.get('limiting_name', ''), str), 'limiting_name must be string'
    assert isinstance(cp.get('tie_count', 0), int), 'tie_count must be int'
cov = mem.get('coverage')
assert cov is not None, 'coverage required'

# peak_pressure fields
assert ap.get('available') in (True, False), 'peak_pressure.available must be bool'
if ap.get('available'):
    pp = ap.get('pressure_pct_x100')
    assert pp is not None and 0 <= pp <= 10000, f'peak_pressure.pressure_pct_x100 must be 0..10000, got {pp}'
    lc = ap.get('limiting_component')
    assert lc in ('system_heap', 'net_pkt_rx', 'net_pkt_tx', 'net_buf_rx_data', 'net_buf_tx_data', 'thread_stack'), f'unexpected peak_pressure.limiting_component: {lc}'
    assert isinstance(ap.get('limiting_name', ''), str), 'limiting_name must be string'
    assert isinstance(ap.get('tie_count', 0), int), 'tie_count must be int'
since = ap.get('since')
assert since == 'boot', f'peak_pressure.since must be boot, got {since}'

# physical and stacks remain unchanged from Phase 1
phys = mem.get('physical', {})
assert phys.get('total_bytes', 0) > 0, 'physical.total_bytes must be > 0'
assert phys.get('reserved_pct_x100', 0) <= 10000, 'reserved_pct_x100 must be 0..10000'
tb = phys.get('total_bytes', 0)
assert 200_000 < tb < 1_000_000, f'physical.total_bytes({tb}) outside expected 200KB..1MB range'
stacks = mem.get('stacks', {})
mc = stacks.get('measured_count', 0)
tc = stacks.get('thread_count', 0)
assert mc <= tc, f'measured_count({mc}) must be <= thread_count({tc})'
ec = stacks.get('error_count', 0)
assert 0 <= ec <= tc, f'error_count({ec}) must be 0..thread_count({tc})'
print('Phase 2 memory schema check passed')
"
```

验证 HTTP 响应 JSON 长度低于 4096 字节（协议限制检查）：

```sh
LEN=$(timeout 5s curl -fsS http://172.29.203.1:8080/api/v1/status | wc -c)
echo "status response size: $LEN bytes"
[ "$LEN" -lt 4096 ] || { echo "status response must be below 4096 bytes"; exit 1; }
```

验证 WebSocket `snapshot/status` 包含相同的 `memory` 形状和 Phase 2 对象：

```sh
timeout 5s node <<'NODE'
const base = 'http://172.29.203.1:8080';
const httpMemory = await fetch(`${base}/api/v1/status`)
  .then((r) => {
    if (!r.ok) throw new Error(`status fetch failed: HTTP ${r.status}`);
    return r.json();
  })
  .then((data) => data.board_monitoring?.memory);
if (!httpMemory) throw new Error('HTTP status missing memory');

const shape = (value) => {
  if (Array.isArray(value)) return value.map(shape);
  if (value !== null && typeof value === 'object') {
    return Object.fromEntries(Object.keys(value).sort().map((key) => [key, shape(value[key])]));
  }
  return typeof value;
};

const session = await fetch(`${base}/api/v1/live-sessions`, { method: 'POST' }).then((r) => {
  if (!r.ok) throw new Error(`session create failed: HTTP ${r.status}`);
  return r.json();
});
const rawUrl = session.ws_url ?? session.session?.ws_url;
if (!rawUrl) throw new Error('session response missing ws_url');
const wsUrl = new URL(rawUrl, base);
wsUrl.protocol = wsUrl.protocol === 'https:' ? 'wss:' : 'ws:';

let snapshotCount = 0;
const MAX_SNAPSHOTS = 3;

await new Promise((resolve, reject) => {
  const ws = new WebSocket(wsUrl);
  const timer = setTimeout(() => reject(new Error('status snapshot timeout')), 4000);
  ws.addEventListener('open', () => {
    ws.send(JSON.stringify({ type: 'subscribe', topic: 'live', rate_hz: 10, id: 'memory-hil' }));
  });
  ws.addEventListener('message', (event) => {
    const data = JSON.parse(event.data);
    if (data.type !== 'snapshot' || data.topic !== 'status') return;
    const mem = data.board_monitoring?.memory;
    if (!mem) {
      reject(new Error('WS status snapshot missing memory'));
      return;
    }
    // Verify Phase 2 objects are present
    if (mem.current_pressure == null || mem.peak_pressure == null) {
      reject(new Error('WS status snapshot missing Phase 2 memory objects'));
      return;
    }
    if (mem.peak_pressure.since !== 'boot') {
      reject(new Error('WS peak_pressure.since must be boot'));
      return;
    }
    if (JSON.stringify(shape(mem)) !== JSON.stringify(shape(httpMemory))) {
      reject(new Error('HTTP and WS memory shapes differ'));
      return;
    }
    snapshotCount++;
    if (snapshotCount >= MAX_SNAPSHOTS) {
      clearTimeout(timer);
      ws.close();
      resolve();
    }
  });
  ws.addEventListener('error', () => reject(new Error('WebSocket error')));
});
console.log(`WS memory Phase 2 check passed (${snapshotCount} snapshots received)`);
NODE
```

验证 Rust CLI JSON 透传 Phase 2 memory 对象：

```sh
timeout 5s radxa-linkr-debuggerctl --json status | python3 -c "
import sys, json
d = json.load(sys.stdin)
m = d.get('board_monitoring', {})
assert 'memory' in m or 'heap' in m, 'must have memory or heap'
if m.get('memory'):
    mem = m['memory']
    # Legacy root
    p = mem.get('pressure_pct_x100')
    assert p is not None and 0 <= p <= 10000, f'pressure_pct_x100 out of range: {p}'
    # Phase 2 current_pressure
    cp = mem.get('current_pressure')
    assert cp is not None, 'current_pressure required'
    cpp = cp.get('pressure_pct_x100')
    assert cpp is not None and 0 <= cpp <= 10000
    # Phase 2 peak_pressure
    pp = mem.get('peak_pressure')
    assert pp is not None, 'peak_pressure required'
    assert pp.get('since') == 'boot', 'peak_pressure.since must be boot'
print('Rust CLI Phase 2 memory display check passed')
"
```

动态采样验证：在 WebSocket 订阅期间多次轮询 HTTP 端点，观察 `current_pressure` 可以在不同时间点有不同的值（尽管在正常 idle 状态下值可能稳定）。此验证演示 Phase 2 字段在 HTTP 和 WS 两条通路上语义一致，不要求值在单次 HIL 中发生实际变化：

```sh
for i in 1 2 3; do
  timeout 5s curl -fsS http://172.29.203.1:8080/api/v1/status | python3 -c "
import sys, json
d = json.load(sys.stdin)
mem = d.get('board_monitoring', {}).get('memory', {})
cp = mem.get('current_pressure', {})
pp = mem.get('peak_pressure', {})
print(f'sample $i: current={cp.get(\"pressure_pct_x100\")} peak={pp.get(\"pressure_pct_x100\")} lc={cp.get(\"limiting_component\")}')
"
  sleep 1
done
```

旧固件降级路径无法在已经刷入新固件的同一轮 HIL 中直接复现；由 Rust 单元测试和
Web UI mock 场景验证无 `memory` 字段时分别回退到 heap 摘要和 heap free 显示。

验证固件内嵌 Web UI：

```sh
timeout 5s curl --compressed -fsS http://172.29.203.1:8080/ | grep -q 'Radxa Linkr Debugger'
timeout 5s curl --compressed -fsS http://172.29.203.1:8080/assets/app.css >/dev/null
timeout 5s curl --compressed -fsS http://172.29.203.1:8080/assets/app.js >/dev/null
```

再用 Edge/Chromium 打开 `http://172.29.203.1:8080/`，确认页面识别正确 MCU、状态轮询
没有失败请求，并切换到实时模式验证 `/api/v1/ws/<slot>` 持续收帧。

串口卡片必须按以下两个分支之一完成验证：

**分支 A（override 已启用，直连 Web Serial）**：

1. 在 `chrome://flags/#unsafely-treat-insecure-origin-as-secure` 中加入精确来源
   `http://172.29.203.1:8080`（将地址复制后粘贴到浏览器地址栏，普通网页无法直接导航到
   `chrome://` 页面）。Edge 同样接受该 Chromium 地址。
2. 重启浏览器，重新打开板载页面 `http://172.29.203.1:8080/`。
3. 确认卡片不再显示红色设置状态，**Web Serial** 按钮可发起浏览器设备选择器；手动选择
   CH347 设备，验证可以收发串口数据。chooser 是浏览器的强制安全机制，测试中不可绕过。

该 override 属于实验性配置，会降低该来源安全性；它不会移除用户手势或设备选择器要求。

**分支 B（override 未启用，bridge 回退）**：

1. 确认板载页面串口卡片显示红色 **Web Serial** 按钮；点击后弹出三步设置说明。
2. 依次点击 Chromium 标志页地址块和精确来源地址块，确认两项都能独立复制并各自显示成功反馈。
   copy 成功仅确认剪贴板写入，不代表串口连接建立。
3. 因板载页面为 HTTP，Clipboard API 在部分浏览器上下文中可能不可用；此时复制控件必须
   尝试 HTTP 兼容 fallback。若 fallback 也失败，完整地址仍需可见并允许手动选择复制。
4. 弹窗需满足无障碍要求：`role="dialog"`、`aria-modal="true"`、初始焦点在弹窗内、
   Tab/Shift+Tab 限于弹窗内、Escape 关闭弹窗并恢复焦点到触发元素、关闭后恢复 body 滚动。
5. 另开终端在 `web/` 目录运行 `npm run device-bridge` 并保持进程存活，使用 **Bridge**
   按钮连接 CH347，验证可以收发串口数据。

**Playwright 自动化路径**：区分两类失败模式。`page.goto` 失败或资源加载超时，指向板子
NCM、HTTP 服务或浏览器配置问题；页面加载成功后断言失败，指向 UI 回归。insecure-context
（HTTP）测试必须确认红色按钮只打开设置弹窗、不请求串口；override-active 测试必须确认按钮
进入直连 Web Serial 路径。chooser 展示、手动选择 CH347 和实际串口收发仍由分支 A 的人工 HIL
完成，不得在自动化中绕过浏览器安全机制。

**bounded retries 要求**：所有轮询检测均使用有界重试（有限次数 + 有限间隔），不得使用无限循环。
超时时间使用 `timeout 5s` 前缀。

WebSocket 订阅使用 Rust CLI 的 `adc record` 流程创建 live session：

```sh
timeout 10s radxa-linkr-debuggerctl adc record /tmp/linkr-hil.ndjson 3 --rate-hz 10
```

验证输出文件包含 3 条 `radxa-linkr-debugger.v1` telemetry record。
高频路径还必须执行：

```sh
timeout 15s radxa-linkr-debuggerctl adc record /tmp/linkr-hil-1000.ndjson 1000 --rate-hz 1000
test "$(wc -l < /tmp/linkr-hil-1000.ndjson)" -eq 1000
! grep -q '"dropped_samples"' /tmp/linkr-hil-1000.ndjson
```

用连续记录的 `sequence` 和 `metadata.device_timing.uptime_us` 计算实际采样间隔；不得用
WebSocket 帧数代替样本数。固件支持最多四个并发客户端，四路并发测试必须确认每个
输出文件都持续增长，且任一客户端退出不会中断其他客户端。

### 3. 电源输出 get/set

```sh
timeout 5s radxa-linkr-debuggerctl --json power set 12v_out on
timeout 5s radxa-linkr-debuggerctl --json power set 12v_out off

timeout 5s radxa-linkr-debuggerctl --json power set 5v_out on
timeout 5s radxa-linkr-debuggerctl --json power set 5v_out off

# 5v_ws is board-internal: list/status must omit it and direct CLI access must fail locally.
timeout 5s radxa-linkr-debuggerctl --json power list > /tmp/linkr-power-list.json
! grep -q '"name":"5v_ws"' /tmp/linkr-power-list.json
! timeout 5s radxa-linkr-debuggerctl --json power get 5v_ws
! timeout 5s radxa-linkr-debuggerctl --json power set 5v_ws off

# The raw API compatibility entry remains available for low-level diagnostics.
timeout 5s curl -fsS http://172.29.203.1:8080/api/v1/power/5v_ws

timeout 5s radxa-linkr-debuggerctl --json power set 20v_out on
timeout 5s radxa-linkr-debuggerctl --json power set 20v_out off
```

### 4. ADC 电流监测

```sh
timeout 5s radxa-linkr-debuggerctl --json adc read
timeout 5s radxa-linkr-debuggerctl --json adc read 5v_out
timeout 5s radxa-linkr-debuggerctl --json adc read 12v_out
timeout 5s radxa-linkr-debuggerctl --json adc read 20v_out
```

验证：

- 返回字段合理
- `raw / mv / current_ua / sensor_value` 可解析

### 5. switch 路由控制

```sh
timeout 5s radxa-linkr-debuggerctl --json switch get sd
timeout 5s radxa-linkr-debuggerctl --json switch route sd target
timeout 5s radxa-linkr-debuggerctl --json switch get sd

timeout 5s radxa-linkr-debuggerctl --json switch get usb
timeout 5s radxa-linkr-debuggerctl --json switch route usb target --confirm
timeout 5s radxa-linkr-debuggerctl --json switch get usb
```

注意：

- `set` 后等待硬件稳定再 `get`
- 优先顺序验证，避免并行反向测试干扰

### 6. VIN 控制（G3 专用）

默认 HIL 只确认 VIN 保持在安全默认值 3.3V，不执行 1.8V 切换：

```sh
timeout 5s radxa-linkr-debuggerctl --json switch get vin
# 物理验证：测量目标板 VIO 引脚电压是否为 ~3.3V
```

验证：

- `route` 字段回读为 `3.3v`
- 物理测量确认 VIO 电压约为 3.3V

只有同时满足以下条件时，才允许扩展执行 1.8V 切换测试：

- 已明确确认外接目标和 CH347 侧器件均支持 1.8V
- 已连接物理测量设备并能在切换后立即确认 VIO 电压
- 操作者已接受切换的硬件副作用

满足上述条件后，严格按顺序执行，并在结束时恢复 3.3V：

```sh
timeout 5s radxa-linkr-debuggerctl --json switch route vin 1.8v --confirm
sleep 5
timeout 5s radxa-linkr-debuggerctl --json switch get vin
# 物理验证：测量目标板 VIO 引脚电压是否为 ~1.8V
timeout 5s radxa-linkr-debuggerctl --json switch route vin 3.3v --confirm
sleep 5
timeout 5s radxa-linkr-debuggerctl --json switch get vin
# 物理验证：测量目标板 VIO 引脚电压是否为 ~3.3V
```

### 7. safe GPIO

```sh
timeout 5s radxa-linkr-debuggerctl --json gpio list
timeout 5s radxa-linkr-debuggerctl --json gpio set GP13 1
timeout 5s radxa-linkr-debuggerctl --json gpio input GP13
timeout 5s radxa-linkr-debuggerctl --json gpio set GP13 0
timeout 5s radxa-linkr-debuggerctl --json gpio input GP13
```

### 8. watchdog 状态

```sh
timeout 5s radxa-linkr-debuggerctl --json watchdog status
```

G3 额外观察 GPIO25 心跳 LED：在 `/api/v1/watchdog` 返回 healthy 状态后，
目视检查蓝色状态 LED 应在大约 1 秒周期内亮灭交替。视觉观察时间窗口
bound 到 5 秒以内（足够看到 2-3 次完整周期）。

G2 无固件心跳 LED，跳过 LED 观察项。

### 9. BOOTSEL 进入

```sh
timeout 5s curl -fsS -X POST http://172.29.203.1:8080/api/v1/bootloader || true
```

G3 额外观察：进入 BOOTSEL 后，GPIO25 心跳 LED 必须熄灭（inactive）。
BOOTSEL 运行期间 LED 保持熄灭是预期行为。

MCU 重启时 USB 连接可能先断开，因此以 BOOTSEL 枚举结果作为成功判据。使用有界重试循环
（最多 10 次，每次间隔 1 秒）通过 `timeout 5s lsblk` 轮询 VENDOR 列严格等于 `RPI` 的磁盘：

```sh
RPI_DISK=
attempts=10
while [ "$attempts" -gt 0 ]; do
  RPI_DISK=$(timeout 5s lsblk -dpno NAME,VENDOR | awk '$2 == "RPI" { print $1; exit }')
  [ -n "$RPI_DISK" ] && break
  attempts=$((attempts - 1))
  sleep 1
done
[ -n "$RPI_DISK" ] || { echo "BOOTSEL device not found after 10s"; exit 1; }
```

不得假设设备字母（如 `/dev/sdb`）。设备名称取决于当前连接的 USB 存储设备数量，`lsblk`
加严格的 `RPI` vendor 匹配是可靠的发现方式。

挂载发现的分区，复制 canonical UF2：

```sh
RPI_PART=$(timeout 5s lsblk -lnpo NAME,TYPE "$RPI_DISK" | awk '$2 == "part" { print $1; exit }')
[ -n "$RPI_PART" ] || { echo "BOOTSEL partition not found"; exit 1; }
RPI_MOUNT=$(timeout 5s udisksctl mount -b "$RPI_PART" | awk -F" at " '{print $2}' | tr -d '[:space:]')
cp build/radxa_linkr_debugger/zephyr/zephyr.uf2 "$RPI_MOUNT/"
```

烧录完成后，使用有界重试（最多 15 次，每次间隔 2 秒）轮询 HTTP 端点，确认板子已重新枚举并恢复响应：

```sh
BOARD_READY=
attempts=15
while [ "$attempts" -gt 0 ]; do
  if timeout 5s curl -fsS http://172.29.203.1:8080/api/v1/status >/dev/null; then
    BOARD_READY=1
    break
  fi
  attempts=$((attempts - 1))
  sleep 2
done
[ "$BOARD_READY" = 1 ] || { echo "board HTTP did not recover"; exit 1; }
```

若 HTTP 不可用，使用串口 fallback：

```text
linkr-debugger:~$ bootloader
```

随后重新烧录（使用 canonical UF2 路径）：

```sh
picotool load -v -x build/radxa_linkr_debugger/zephyr/zephyr.uf2
```

### 10. USB CDC ACM fallback

验证：

- HTTP/WS 不可用时，CDC ACM 串口仍可访问
- fallback shell 仍可执行基本命令
- fallback 路径仍可进入 BOOTSEL

CDC ACM shell 验证（G3 固件）：

```text
linkr-debugger:~$ vin get
linkr-debugger:~$ bootloader
```

默认 CDC fallback 验证不切换 VIN。`vin set 1.8v` / `vin set 3.3v` 只允许在满足
第 6 节的目标兼容性、物理测量和副作用确认条件后执行。

RP2040 (G2) 固件仍注册 `vin` shell 命令，但执行时会明确返回 unavailable；
只有 G3 支持实际 VIN 控制。

## 最短 HIL smoke test

时间紧张时，至少验证以下 6 项：

1. `--json doctor`
2. `--json status`
3. `power set/get`
4. `adc read`
5. `switch route/get`
6. `bootloader`

VIN 1.8V 切换不属于默认 smoke test 范围；它是 G3 专用且需要目标板电压兼容确认和物理测量验证，属于条件限定的可选步骤，详见上方第 6 节。

## 验证结果记录建议

建议记录：

- 提交 hash
- 固件版本/tag
- 构建目录与 UF2 路径
- 烧录方式
- 实测命令与返回值摘要
- 是否通过 BOOTSEL 与 CDC ACM fallback
- 失败项与风险说明

## 参考来源

- `README.md`
- `AGENTS.md`
- `skills/radxa-linkr-debugger/SKILL.md`
