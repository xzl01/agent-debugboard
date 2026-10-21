# HIL 功能测试规范

本文档定义 `radxa-linkr-debugger` 固件与主机控制逻辑改动后的板级 HIL（Hardware-in-the-Loop）功能测试规范。

## 适用范围

以下改动不能只依赖编译/静态检查/unit test，必须完成 HIL 实机功能验证：

- RP2350 固件逻辑改动
- 主机 CLI/TUI 真实控制逻辑改动
- 电源输出、switch 路由、ADC 电流监测、safe GPIO、watchdog、reboot、物理 BOOTSEL 按键、BOOTSEL 相关改动
- OTA 路径相关改动（MCUboot OTA 是 RP2350 专用功能）
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
5. 验证 reboot 是正常冷重启，以及 BOOTSEL 进入能力（HTTP/CDC/物理按键，BOOTSEL entry path）
6. OTA 路径相关改动必须验证上传、测试启动、手动确认基本流程
7. 新增功能补对应 functional test（可行时）

持久化配置可能在 HTTP/CDC/WS control transport ready 之前重放并打开 `5v_out`、
`vdd_5v` 或 `5v_usb_hub`。这不改变 HIL 操作顺序：transport ready 后的
**第一条 host hardware mutation** 仍必须显式确认/设置 `5v_out=on`，第二条
mutation 请求 fixed PD 20 V，
随后用 INA226 验证 5 V rail 后才能继续其它硬件操作。报告必须区分“启动重放已为
on”和“host 首次 mutation 再次确认 on”。

## 标准构建与烧录

### 构建

```sh
west build -p always -b rpi_pico2/rp2350a/m33/mcuboot --sysbuild apps/radxa_linkr_debugger -d build/radxa_linkr_debugger
```

### 烧录

RP2350 初次安装或恢复必须使用由 MCUboot 和应用 `zephyr.signed.hex` 合并得到的
`radxa-linkr-debugger-rp2350.uf2`。构建系统自动生成此文件到
`build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`。

**警告**：应用产物
`build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.uf2`
不包含完整 MCUboot 安装，绝不能用于 ROM BOOTSEL 线刷；误刷会导致板子无法正常启动，
必须使用上述合成完整 UF2 恢复。

```sh
picotool load -v -x build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2
```

或使用 RPI-RP2 拖拽方式：

```text
build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2
```

Linux 下免 root 烧录必须按本规范第 9 节通过 VENDOR 为 `RPI` 发现实际磁盘和分区，
再用 `udisksctl` 挂载；不得固定假设 `/dev/sdX1`。只复制 combined initial-install/recovery UF2。

RP2350 BOOTSEL 的 MSC 在收到 UF2 最后一个 block 后会立即复位。先 `cp` 再
`sync` 存在竞态：内核 page cache 可能尚未回写，板子已经启动，造成镜像不完整。
优先使用上面的 `picotool load -v -x`。若必须挂载复制，使用同步挂载，或直接写：

```sh
udisksctl mount -b /dev/sdd1 -o sync
# 或
MP=$(findmnt -n -o TARGET /dev/sdd1)
dd if=build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2 \
   of="$MP/radxa-linkr-debugger-rp2350.uf2" bs=512 conv=fsync oflag=direct
```

复制完成后等待板子自行复位。复制末尾出现 I/O error、`device offline` 或设备
消失是 bootloader 复位的正常现象，不能据此判定失败；也不能依赖随后的 `sync`。
HIL 记录必须注明使用 `picotool` 还是同步复制，以及是否出现
`lost async page write`。

## 标准 HIL 验证 checklist

默认优先使用 release 发布的 `radxa-linkr-debuggerctl` CLI 执行以下主机侧验收命令。
只有在你正在验证尚未发布的 `cmd-ng` 改动时，才将同一组命令替换为等价的
`cargo run --manifest-path cmd-ng/Cargo.toml -- ...` 源码运行方式。

在连接或重新启用 NCM 接口前，先在同一个 shell 中记录现有非 NCM 默认路由和
DNS 基线。若板子已经连接，先断开 NCM 网络连接或释放该接口的 lease，再采集；
不得在完成 NCM DHCP 后才把当前状态记作 `ORIG_*`：

```sh
NCM_IFACE=<ncm-interface>
ORIG_DEFAULT_GW=$(ip route show default | grep -v "dev $NCM_IFACE" | awk '{print $3; exit}')
ORIG_DNS=$(grep '^nameserver' /etc/resolv.conf 2>/dev/null | awk '{print $2}' | head -1)
export NCM_IFACE ORIG_DEFAULT_GW ORIG_DNS
echo "Pre-NCM baseline: default gw=$ORIG_DEFAULT_GW dns=$ORIG_DNS"
```

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
timeout 5s curl -fsS http://172.29.203.1/api/v1/status
timeout 5s curl -fsS http://172.29.203.1/api/v1/watchdog
timeout 5s curl -fsS http://172.29.203.1/api/v1/switch
timeout 5s radxa-linkr-debuggerctl --json power list
timeout 5s radxa-linkr-debuggerctl --json switch list
```

### 2b. Memory monitoring (Phase 2)

验证 HTTP `/api/v1/status` 返回的 `board_monitoring.memory` 字段形状和 Phase 2 加性对象：

```sh
timeout 5s curl -fsS http://172.29.203.1/api/v1/status | python3 -c "
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

验证 HTTP 响应 JSON 长度低于专用 6656 字节 status buffer（协议限制检查）：

```sh
LEN=$(timeout 5s curl -fsS http://172.29.203.1/api/v1/status | wc -c)
echo "status response size: $LEN bytes"
[ "$LEN" -lt 6656 ] || { echo "status response must be below 6656 bytes"; exit 1; }
```

验证 WebSocket `snapshot/status` 的 initial snapshot 只用于形状校验，后续 event-driven subsequent snapshots 由状态变化触发：

```sh
timeout 5s node <<'NODE'
const base = 'http://172.29.203.1';
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
const MAX_SNAPSHOTS = 1;

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
console.log(`WS memory initial snapshot check passed (${snapshotCount} snapshot received); later snapshots are event-driven`);
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
timeout 5s curl -fsS http://172.29.203.1/api/v1/status | python3 -c "
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
timeout 5s curl --compressed -fsS http://172.29.203.1/ | grep -q 'Radxa Linkr Debugger'
timeout 5s curl --compressed -fsS http://172.29.203.1/assets/app.css >/dev/null
timeout 5s curl --compressed -fsS http://172.29.203.1/assets/app.js >/dev/null
```

再用 Edge/Chromium 打开 `http://172.29.203.1/`，确认页面识别正确 MCU、状态轮询
没有失败请求，并切换到实时模式验证 `/api/v1/ws/<slot>` 持续收帧。

#### J4 xterm.js terminal

产品只提供单一逻辑串口 `j4`。使用 Edge/Chromium 打开板载页面后进入 **Web
Terminal**，必须验证：

1. xterm.js 终端自动创建 `/api/v1/live-sessions`、连接返回的 WS slot 并订阅
   `topic:"serial.j4"`，无需 browser chooser、本机 tty、host broker 或安全来源 override。
2. 终端连接到真实目标 Linux console，可看到启动/登录输出并完成一次双向输入。手动点击
   disconnect 后必须保持 sticky disconnected，不得出现自动 retry storm；显式 reconnect
   后恢复。
3. 一个完整 final binary WS message 才能成为一条 J4 TX；分别验证两段到达的 480 字节、
   NUL/`0xff`、前一帧 TEXT 后 BINARY、continuation FIN。481 字节及以上返回明确 error，
   不能写入 UART。每次成功写先收到 `serial_write_accepted`，物理 wire complete 后再收到
   同 event id 的 `serial_write_complete`；断连/复用 slot 不得收到旧 completion。
4. 同时打开 J4 CDC ACM、四个 WS sink 中至少一个，以及两个 `nc <board-ip> 5555`
   raw-TCP 客户端；让一个 TCP 客户端故意变慢。其它 sink 必须持续收到数据，慢 sink 只增长
   自己的 drop/overrun。raw 5555 必须保持 binary-transparent，无 Telnet/IAC、banner、echo
   或 CR/LF 转换。
5. `GET /api/v1/serial/j4` 的 state、line coding、generation、DTR/RTS、USB configured、
   sink count、queue/counters 可解析。用当前 generation 执行一次 PUT CAS 并确认新
   generation；旧 generation 返回 conflict，非法 tuple 不窄化/不排队。
6. baud范围是4800..115200的连续整数，不是preset白名单。在同一CDC fd上依次验证
   115200→custom 5200→4800→115200，每次以GET current/generation为权威；
   115201的HTTP请求必须400且current不变。DTR持有配置所有权时，HTTP CAS可先accepted
   后异步完成`result=-16`，tuple/generation保持不变；释放DTR后custom 5200必须可应用。
   Web UART settings必须提供custom numeric input及五个推荐preset。CDC SET本身无法报告
   application rejection，不能作为最终权威。230400、921600、1500000同样必须被拒绝；
   这些 high-rate 只作为上限诊断，不得记为产品 pass；若使用单独标注的 non-product
   diagnostic build 观察高 rate，也必须与产品验收分开记录。
7. 在4800 baud、8E2下发送480个zero byte；write timeout必须按八槽队列wire time加2秒
   margin推导。物理complete后再启动拥有GP10的SINGLE采集；LA pause最大等待1250ms。
8. 对侧 UART 可被独占时，临时停止 getty，将两端设为 raw 8N1，并只在支持的4800、
   custom 5200、115200逐档执行确定性伪随机payload的双向bit compare。记录每档每方向
   bytes、missing bytes、bit errors与有限样本BER；任何中途重启、人工串口操作或整组
   USB断开均使该轮无效，必须从完整矩阵开头重跑。最终镜像中4800/5200各方向
   65536 bits、115200各方向1048576 bits，支持档聚合为2,359,296 tested bits
   （131072+131072+2097152），零观测错误。结束后恢复两端115200、原tty设置和getty，
   并确认干净shell输出。BER=0只能表述为该有限样本零观测错误，不能代替模拟信号完整性
   认证。high-rate 诊断必须单独记录、不得并入支持档 BER：230400 的0x55 peer读数主要
   为0xB5/0x95且有明显framing error；921600/1500000无法稳定sync；早期一次1500000
   零错误样本不可复现，后续1500000记录560 bit errors。未做oscilloscope bit-width测量。
9. 用 SINGLE/FAST8/WIDE11 中拥有 GP10/GP11 的模式执行一次采集：J4 必须先报告 PAUSED，
   capture 中 payload 明确被拒绝但 WS/TCP session 不被注销；STOP/terminal cleanup 后
   UART mux 与 fresh sink epoch 恢复，同一终端无需刷新即可继续。
10. 无 host CDC 默认值回归：刷写/正常重启后不连接任何 CDC host，等 `usb_configured`
    为 true 后至少 4 秒，`GET /api/v1/serial/j4` 必须仍为 `115200`、`dtr=false`、
    sink 数为 0。旧镜像在此场景变成 `9600` 的行为必须判失败。
11. CDC line coding 低态与 promotion：DTR=false 时用 HTTP/WS 请求 `5200` 必须立即成功
    并更新 current/generation；随后真实 ACM4 以 `115200` 打开，GET 必须先显示 active
    `115200`，之后才发布 `dtr=true`，在目标上执行 `uname -s` 必须干净返回；关闭 ACM4
    后至少 3 秒 GET 仍须为 `115200`。再执行一次 TCP SINGLE 100 kHz / 2 s 采集，J4
    必须保持 `115200`。
12. CDC 内部四态 DOWN/PROMOTING/UP/BLOCKED、USB default line coding 仅 stage、
    LOW→HIGH 先应用 staged tuple 再 publish DTR/ingress、失败 BLOCKED、notification
    sequence/inflight promotion fence、session epoch + mailbox attempt 身份，以及
    deferred USB notification（callback 只 stage/commit 并唤醒 worker，不在通知上下文
    做 HAL attach/promotion/configure）必须由 host-model 覆盖。外部 API 不得公开
    phase 或 block reason；HTTP/WS 在 CDC 非 DOWN 时返回冲突而不是排队 stage。

##### 自动终端 responder 矩阵

显式 J4 输入仍是 multiwriter；不得把未选中自动回复的 client 描述为 read-only
或 TX owner。终端 query 只由固件选中的唯一 responder 回答：CDC DTR 有效时选
CDC，否则选编号最小的已连接 WebSocket。其它 Web 页面仍必须能发送键盘、IME、
粘贴和 Ctrl-C，ANSI 不得被过滤。使用受控目标 tty 和可控的
DSR/DA/CPR 查询执行以下矩阵，并同时记录每个 client 的
`terminal_responder`/`responder` 状态：

| 连接组合 | 预期自动 responder | 必须观测到的自动回复 |
| --- | --- | --- |
| 一个 Web，无 CDC DTR | 该 Web slot | 恰好一条 `ESC[2;1R` |
| 两个 Web，无 CDC DTR | 编号最小的 Web slot | 恰好一条 `ESC[2;1R` |
| Web + CDC DTR | CDC | 仅 CDC 的 `ESC[24;80R`，Web 不回复 |
| 被动 Web 手动按 `x` | 另一个 client 或 none | 目标收到 `0x78`，且不产生自动 query 回复 |

每次 generation 或 sink epoch 变化后，xterm parser 必须重建；responder
promotion 则必须保留同一套 xterm modes，在自动回复关闭状态下排空 promotion
前的输出，并用 zero-byte parser fence 确认旧写入完成后才启用自动回复。旧的
半截或已排队 query 不得在新状态被回复。该矩阵是有限功能证据，不是对所有终端
escape 序列或多 client 竞态的穷举证明。

测试必须针对板载 port 80 production bundle。port 5173 只是 Vite 开发预览，可额外做
UI 回归，但不能代替 embedded Web 与真实 MCU WS 路径。所有轮询使用有界重试。

**bounded retries 要求**：所有轮询检测均使用有界重试（有限次数 + 有限间隔），不得使用无限循环。
超时时间使用 `timeout 5s` 前缀。

WebSocket 订阅使用 Rust CLI 的 `adc record` 流程创建 live session：

```sh
rm -f /tmp/linkr-hil.ndjson
timeout 5s radxa-linkr-debuggerctl adc record /tmp/linkr-hil.ndjson 3 --rate-hz 10
python3 - <<'PY'
import json
from pathlib import Path

rows = [json.loads(line) for line in Path('/tmp/linkr-hil.ndjson').read_text().splitlines()]
assert len(rows) == 3, f'expected 3 telemetry records, got {len(rows)}'
for row in rows:
    assert row.get('schema') == 'radxa-linkr-debugger.v1'
    assert row.get('type') == 'telemetry-record'
    timing = row.get('metadata', {}).get('device_timing', {})
    assert isinstance(timing.get('sample_sequence'), int), 'missing sample_sequence'
    assert isinstance(timing.get('device_t_mono_us'), int), 'missing device_t_mono_us'
    assert isinstance(timing.get('uptime_us'), int), 'missing uptime_us'
    dropped = row.get('metadata', {}).get('dropped_samples')
    if dropped is not None:
        assert isinstance(dropped, int) and dropped > 0, f'invalid dropped_samples: {dropped}'
print('single telemetry timing aliases passed')
PY
```

验证输出文件包含 3 条 `radxa-linkr-debugger.v1` telemetry record，且单样本 telemetry
保留 `sample_sequence`、`device_t_mono_us` 和 `uptime_us`。若出现
`metadata.dropped_samples`，其值必须为正整数；未发生采样环覆盖时不得强制要求该字段出现。
高频 batch 路径还必须执行：

```sh
rm -f /tmp/linkr-hil-adc-max.ndjson
timeout 5s radxa-linkr-debuggerctl adc record /tmp/linkr-hil-adc-max.ndjson 250 --rate-hz 250
python3 - <<'PY'
import json
from pathlib import Path

rows = [json.loads(line) for line in Path('/tmp/linkr-hil-adc-max.ndjson').read_text().splitlines()]
assert len(rows) == 250, f'expected 250 telemetry records, got {len(rows)}'
for index, row in enumerate(rows):
    timing = row.get('metadata', {}).get('device_timing', {})
    for field in ('sample_sequence', 'device_t_mono_us', 'uptime_us'):
        assert isinstance(timing.get(field), int), f'row {index} missing {field}'
    dropped = row.get('metadata', {}).get('dropped_samples')
    if dropped is not None:
        assert isinstance(dropped, int) and dropped > 0, f'row {index} invalid dropped_samples: {dropped}'
print('batch telemetry timing aliases passed at the 250 Hz limit')
PY
```

`rate_hz` 的当前验收范围是 `1..250`。`251` 及以上必须返回 `invalid_rate`，
不得 clamp；HIL 记录应明确写出 251 被拒绝的观测结果，而不是只测 250 Hz
成功。用连续记录的 `sequence`、`metadata.device_timing.sample_sequence` 和
`metadata.device_timing.device_t_mono_us` 计算实际采样间隔；紧凑 batch 在线路上只提供
`sequence` 与 `uptime_us` 时，Rust recorder 必须归一化出前述别名。不得用 WebSocket 帧数代替样本数。固件支持最多四个并发客户端，四路并发测试必须确认每个
输出文件都持续增长，且任一客户端退出不会中断其他客户端。

### 2c. 强制门户发现

验证 DHCP option 114（Captive Portal URI）已正确通告。利用 tcpdump 在 NCM 接口
捕获 DHCP DORA 过程，解析客户端 Parameter Request List（PRL）和 ACK 中的 option
114。该 NCM 链路仅供本地访问：固件不得通告 option 3（router）或 option 6（DNS）。
以下为 Linux 示例；macOS/Windows 使用对应平台 tcpdump 或 DHCP 工具：

```sh
# 先用 ip link 确认 NCM 接口名，再显式填写；不要自动选择第一块网卡。
NCM_IFACE=<ncm-interface>
DHCP_CAPTURE=/tmp/linkr-dhcp.pcap
DHCLIENT_CONF=/tmp/linkr-dhclient-captive.conf

# ISC dhclient only requests server options listed in its Parameter Request List.
# Define option 114 and append it to the normal request list for this HIL lease.
cat > "$DHCLIENT_CONF" <<'EOF'
option captive-portal code 114 = text;
also request captive-portal;
EOF

# Give this dedicated dhclient exclusive ownership of the interface. Record and
# restore NetworkManager ownership after collecting the DHCP exchange.
NCM_WAS_MANAGED=no
if command -v nmcli >/dev/null 2>&1 &&
   [ "$(nmcli -g GENERAL.MANAGED device show "$NCM_IFACE" 2>/dev/null)" = "yes" ]; then
  NCM_WAS_MANAGED=yes
  sudo nmcli device disconnect "$NCM_IFACE" || true
  sudo nmcli device set "$NCM_IFACE" managed no
fi

# 必须先启动抓包，再触发 lease 更新，避免漏掉 DORA。
sudo timeout 15s tcpdump -U -i "$NCM_IFACE" -w "$DHCP_CAPTURE" \
  'port 67 or port 68' &
TCPDUMP_PID=$!
sleep 1
sudo timeout 5s dhclient -r "$NCM_IFACE" || true
sudo timeout 5s dhclient -cf "$DHCLIENT_CONF" "$NCM_IFACE"
wait "$TCPDUMP_PID" || [ "$?" -eq 124 ]

# 展开客户端请求和 ACK 的 DHCP options。不同 Wireshark 版本的字段名不同，
# 因此保留 verbose 文本作为 HIL 记录，并人工确认请求 PRL 包含 114，且 ACK
# 包含 option 114 而不通告 router 或 DNS。
timeout 5s tshark -r "$DHCP_CAPTURE" -Y 'dhcp.option.dhcp == 3' -V \
  > /tmp/linkr-dhcp-request.txt
timeout 5s tshark -r "$DHCP_CAPTURE" -Y 'dhcp.option.dhcp == 5' -V \
  > /tmp/linkr-dhcp-ack.txt
grep -E 'Parameter Request List|Captive-Portal|Option.*114' \
  /tmp/linkr-dhcp-request.txt
grep -E 'Captive-Portal|Option.*114' \
  /tmp/linkr-dhcp-ack.txt
if grep -E 'Router|Domain Name Server|Option.*(3|6)' /tmp/linkr-dhcp-ack.txt; then
  echo "DHCP ACK must not advertise router or DNS"
  exit 1
fi

if [ "$NCM_WAS_MANAGED" = yes ]; then
  sudo timeout 5s dhclient -r "$NCM_IFACE" || true
  sudo nmcli device set "$NCM_IFACE" managed yes
  sudo nmcli device connect "$NCM_IFACE"
fi
```

DHCP DORA 的客户端 PRL 必须请求 DHCP option 114。DHCPACK 必须包含 DHCP option
114，URI 必须为 `http://172.29.203.1/captive-portal/api`。DHCPACK 不得通告 DHCP
option 3（router）或 DHCP option 6（DNS）。把 `/tmp/linkr-dhcp-ack.txt` 留作 HIL
记录。上述命令会恢复原 NetworkManager 所有权；不要同时运行两个 DHCP client。

板子不提供 DNS 服务，也不提供 wildcard A/AAAA 响应；不要把 `172.29.203.1` 当作
DNS resolver 进行直接查询。继续使用下方多宿主检查验证主机原有的外部 DNS 和默认
路由在 NCM DHCP 后仍可用。

验证 HTTP port 80 `/captive-portal/api` 返回 `Content-Type: application/captive+json` 和 HTTP 200：

```sh
timeout 5s curl -fsS -D - -o /dev/null http://172.29.203.1:80/captive-portal/api
```

期望 HTTP 200，`Content-Type` 包含 `application/captive+json`。

验证 HTTP port 80 读取 JSON 内容，断言 `user-portal-url` 和 `venue-info-url`
精确等于 `http://172.29.203.1/`：

```sh
timeout 5s curl -fsS http://172.29.203.1:80/captive-portal/api | python3 -c "
import sys, json
d = json.load(sys.stdin)
assert d.get('captive') == True
expected_url = 'http://172.29.203.1/'
assert d.get('user-portal-url') == expected_url, f'user-portal-url={d.get(\"user-portal-url\")} != {expected_url}'
assert d.get('venue-info-url') == expected_url, f'venue-info-url={d.get(\"venue-info-url\")} != {expected_url}'
print('CAPPORT JSON OK')
"
```

根路径由已注册的静态资源直接提供 Web UI（前述 Web UI 检查应返回 HTTP 200）。
验证未命中静态资源的传统检测路径返回 HTTP 302，且 `Location` 精确为
`http://172.29.203.1/`：

```sh
for path in '/generate_204' '/hotspot-detect.html' '/connecttest.txt'; do
  HEADERS=$(timeout 5s curl -sS -D - -o /dev/null "http://172.29.203.1:80${path}")
  STATUS=$(printf '%s\n' "$HEADERS" | awk 'NR == 1 {print $2}')
  LOCATION=$(printf '%s\n' "$HEADERS" | awk 'BEGIN {IGNORECASE=1} /^Location:/ {gsub("\\r", "", $2); print $2}')
  [ "$STATUS" = "302" ] || { echo "path ${path}: expected 302, got ${STATUS}"; exit 1; }
  [ "$LOCATION" = "http://172.29.203.1/" ] || {
    echo "path ${path}: unexpected Location ${LOCATION}"
    exit 1
  }
  echo "path ${path}: 302 + Location OK"
done
```

当前 pinned Zephyr HTTP/1 server 会在应用 callback 前直接处理 dynamic resource 的
`HEAD` 并返回默认 headers-only HTTP 200；以上门户行为必须使用 `GET` 验证。

验证 HTTP port 80 对非 GET/HEAD 方法返回 HTTP 405，包括门户、status 和 ADC
只读端点：

```sh
for path in /captive-portal/api /api/v1/status /api/v1/adc/read; do
  STATUS=$(timeout 5s curl -sS -o /dev/null -w '%{http_code}' \
    -X POST "http://172.29.203.1:80${path}")
  [ "$STATUS" = "405" ] || {
    echo "POST ${path}: expected 405, got ${STATUS}"
    exit 1
  }
done
echo "POST method rejection OK"
```

**多宿主路由回归检查**：验证在连接 NCM 接口后，主机的原有默认路由/DNS
配置未被 NCM DHCP 覆盖；最大概率自动打开机制不影响已有网络路径。

```sh
# ORIG_* 必须来自本 checklist 开始前、NCM 尚未连接或启用时记录的基线。
[ "${NCM_IFACE+x}" = x ] && [ "${ORIG_DEFAULT_GW+x}" = x ] && [ "${ORIG_DNS+x}" = x ] || {
  echo "Missing pre-NCM baseline; restart the checklist and capture it before enabling NCM"
  exit 1
}

# 连接 NCM 后，验证原有路由/DNS 未变
CURR_DEFAULT_GW=$(ip route show default | grep -v "dev $NCM_IFACE" | awk '{print $3; exit}')
CURR_DNS=$(cat /etc/resolv.conf 2>/dev/null | grep '^nameserver' | awk '{print $2}' | head -1)
echo "After NCM connect: default gw=$CURR_DEFAULT_GW dns=$CURR_DNS"
[ "$ORIG_DEFAULT_GW" = "$CURR_DEFAULT_GW" ] || {
  echo "Default gateway changed; NCM DHCP affected the existing route"
  exit 1
}
! ip route get 8.8.8.8 | grep -q "dev $NCM_IFACE" || {
  echo "Internet traffic is being routed through the NCM interface"
  exit 1
}
EXTERNAL_ADDR=$(timeout 5s getent ahostsv4 example.com | awk 'NR == 1 {print $1}')
[ -n "$EXTERNAL_ADDR" ] && [ "$EXTERNAL_ADDR" != "172.29.203.1" ] || {
  echo "External DNS resolution is unavailable or intercepted by the host environment"
  exit 1
}
echo "Existing Internet route and DNS remain usable"
```

**多宿主路由说明**：若 host 同时有其他网络接口且默认路由指向其他适配器，
操作系统可能不会向 NCM 接口发起强制门户检测请求，自动打开不保证发生。
此时用户可直接打开 `http://172.29.203.1/` 或使用 `curl`、CLI/TUI。

### 2d. 持久化配置

本节覆盖两个独立验收阶段。Todo 14 本地文档验收只验证文档契约与 loopback
mock，与真实硬件无关。Todo 16 实机 HIL 必须在 Todo 15 通过、固件构建链路与
指定板子前置条件齐备之后，再使用本节列出的命令与产物执行。两种验收不能互相
替代，缺少任一项都不能记为完成。

本节只有 Todo 14 本地 checker/test 和 runner 的 `--dry-run` 可使用 `sh`
代码块。未来真实硬件命令一律放在 `text` 块或 prose，防止本地文档检查意外
触发真实硬件操作。

参见 [持久化配置文档](../reference/persistent-configuration.md)。本地验证不等于真实硬件
HIL。2026-08-05 真实硬件 HIL 通过 v1 save-and-apply flow，见
[日期 v1-save HIL 报告](results/2026-08-05-persistent-config-v1-save-hil.md)。历史 2026-07-30
真实硬件 HIL 的六个 runner flow 全部通过，见
[历史六 flow 报告](results/2026-07-30-persistent-config-hil.md)。未来本地测试仍不能
替代硬件行为变更后的新板卡 HIL。

#### Todo 14 本地文档验收

Todo 14 的目标是用本地工具证明六个文档面与契约摘要完全一致，不需要任何真实
板子。在 repo 根目录运行：

<!-- persistent-config-example: hil-todo14-checker -->
```sh
node scripts/check-persistent-configuration-docs.mjs --root .
```

期望输出无 `surface-missing`、`section-missing`、`heading-missing`、
`summary-contract`、`forbidden-claim`、`link-required` 等错误。Node 单元
测试覆盖 9 个禁用表述、shell 示例提取、curl/CLI 执行、port 8080 拒绝与
loopback mock：

<!-- persistent-config-example: hil-todo14-node-tests -->
```sh
node --test scripts/check-persistent-configuration-docs.test.mjs
```

`checkPersistentConfigurationDocs(repositoryRoot).ok === true` 通过即代表
Todo 14 文档验收完成，与硬件状态无关。Todo 14 不接触任何电源轨、路由
1.8V 切换、输出 GPIO 或 BOOTSEL 入口。

<!-- persistent-config-example: hil-todo16-runner-dry-run -->
```sh
sh skills/radxa-linkr-debugger/scripts/config-persistence-hil.sh --dry-run safe-reboot
```

这个 dry-run 只打印计划，不会访问 URL、CDC、BOOTSEL、挂载、刷写或真实硬件。

#### Todo 16 实机前置条件

在执行任何真实硬件 HIL 之前，下列前置条件必须全部满足；缺失任一项的步骤
在报告中记为 `[blocked]`，永不可记为 pass：

- Todo 15 单元测试、`cargo test --manifest-path cmd-ng/Cargo.toml --all-targets`
  与 `cargo clippy --manifest-path cmd-ng/Cargo.toml --all-targets -- -D warnings`
  全绿，且 firmware sysbuild 通过。
- 指定的目标板身份已记录，包括板子型号、RP2350 批次和固件版本；已从实际
  枚举中识别用于 Zephyr cmdline 的 CDC device，并在所有 CDC 流程中显式传给
  runner `--serial <identified-cdc-device>`。J4 target UART 不是持久化配置
  的前置条件。
- 启动用的合成完整 UF2 文件 `radxa-linkr-debugger-rp2350.uf2` 已构建并
  校验 SHA256，OTA 用 `.bin` 文件 `radxa-linkr-debugger-rp2350-ota.bin`
  已生成。应用产物
  `build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.uf2`
  仅作为应用槽存在，对 ROM BOOTSEL 无效；不能用作线刷源。
- 已识别的 CDC device 可用且未被 terminal 程序占用；其身份、权限和
  `--serial` 实参必须写入 HIL 报告。没有已识别 CDC device 的流程记为
  `[blocked]`，不得猜测固定设备路径。
- `nix-shell` 提供的测试依赖（`python3Packages.websocket-client`、
  `python3Packages.pyserial`）已就绪，runner 入口可启动。

#### 安全恢复

Todo 16 实机 HIL 的 safe-reboot 路径必须选用安全值
`switch/sd=usb-reader` 和 `switch/tf_wp=protected`，保存后重启。HTTP GET 确认
两项均由 v1 full restore 恢复；CDC `config show` 只证明摘要计数、可用性和
命令 fallback，不证明逐项值。随后必须把两项恢复为
`switch/sd=target` 和 `switch/tf_wp=writable`。任何缺少物理、网络或 CDC
前置条件的步骤标记为 `[blocked]`，而不是 pass。

每次重启或 BOOTSEL 刷写后，runner 必须在固定 5 秒等待之后，对
`GET /api/v1/config` 执行最长 45 次、每次最长 2 秒的 transport-readiness
探测。只有 curl transport 失败可以重试；一旦设备返回 HTTP 响应，runner 必须
立即验证 HTTP 200、schema、`ok=true`、command 和 action，任何非法响应都立即
失败，不得把业务断言失败误判为尚未 ready。readiness 成功后仍须执行该流程原有
的完整 config 断言，不能用 probe 替代、跳过或放宽业务验证。

实机 `all` 流程的完整 runner 表面如下，只作为不可执行参考；其中所有尖括号
实参必须替换为本次已识别的设备或受控 helper：

```text
sh skills/radxa-linkr-debugger/scripts/config-persistence-hil.sh --execute --url http://172.29.203.1 --serial <identified-cdc-device> --reboot-command <identified-reboot-command> --capture-start <logic-or-sigrok-capture-start-command> --capture-stop <logic-or-sigrok-capture-stop-command> --combined-uf2 build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2 --ota-image build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350-ota.bin --confirm-dangerous-save all
```

其核心 `config-persistence-hil.sh --execute --url http://172.29.203.1 all`
形状不免除这些显式参数。该 `all` 流程包含 `safe-reboot`、
`dangerous-auto-restore`、`capture-busy`、`ota-preserve`、`bootsel-preserve`
和 `cdc-fallback` 六个真实 runner flow。Save 是唯一的持久化变更操作：
保存即应用，没有独立的 Apply 操作；`apply_failed` 部分失败响应字段由
firmware/HTTP host-model 测试与 offline shell fixture 覆盖。ROM BOOTSEL
只能使用 combined `radxa-linkr-debugger-rp2350.uf2`；OTA 只能使用 MCUboot
格式 `radxa-linkr-debugger-rp2350-ota.bin`，不得将 app-only `zephyr.uf2`
用于 ROM BOOTSEL 或 OTA。

#### 危险项 pending 与固件确认

当前x1.2目录必须恰好18项、v1最大84字节：五路power
（含`power/5v_usb_hub`）、`sd`/`tf_wp`两个switch、
GP5/6/7/8/9/12/13/14/15/16/29十一个GPIO。旧`switch/usb`、GP17、GP18请求返回
`unknown_item`；包含旧ID的snapshot必须按malformed拒绝，不能迁移或别名化。

危险项使用已确认无外接负载的 GP5 output HIGH（测试后恢复 input），或由本轮
夹具明确授权的 power-on 值。x1.2 没有 `switch/usb`。host/client 不得自行推断
危险性或跳过确认。
危险提示与拦截只发生在 Save 请求时；写入存储的快照已经获得授权，启动时
全量重放，不存在第二次确认门。

未确认 save 返回 HTTP 409、`error.code=confirmation_required` 必须包含
`dangerous_items`，由真实 HIL 的 `dangerous-auto-restore` flow 验证。真实 HIL 的
`dangerous-auto-restore` flow 只在下面的 `dangerous-auto-restore 与
固件确认` 节中显式断言未确认 save 的 confirmation 错误。

成功 save 的断言字段只能是 `saved_items`、`confirmation_items`、`applied_items`、`snapshot` 和
数值 `pending`；它不使用 `pending_items`。`pending_items` 只属于
`apply_failed` 的部分执行结果。若未确认 save 的错误没有 `dangerous_items`，必须
停止后续危险动作并记录 `[failed: no firmware confirmation]`。

#### dangerous-auto-restore 与固件确认

`dangerous-auto-restore` 是当前固件上唯一在 `all` runner 中验证危险项的
真实硬件 flow，consecutive 2 次重启的 v1 自动重放是它的核心契约。它必须
包含以下断言：

- 未确认的危险 Save 必须返回 HTTP 409、`error.code=confirmation_required`
  并携带 `dangerous_items`，host 不得跳过确认。
- 一次确认的危险 Save（例如已授权的 `gpio/GP5` output HIGH）后，`GET /api/v1/config`
  必须返回 `snapshot.version == 1`、`snapshot.present == true` 且数值
  `pending == 0`；该行的 `current` 与 `saved` 必须完全一致，
  `requires_confirm == true`、`apply_state == applied`、
  `selected == true`。
- 连续两次重启之后再次 GET：上述 v1 字段、值和 `pending == 0` 必须保持
  一致，`apply_state` 仍为 `applied`。v1 自动重放在两次普通启动之间
  不依赖任何外部命令，也不存在独立的 Apply 操作。
- 如果以上任何一次断言失败（例如 `pending != 0`、`apply_state != applied`、
  `snapshot.version != 1`），记为
  `[failed: dangerous auto restore]` 并立即停止后续危险动作。

#### Clear 不改变硬件

`config clear` 必须仅删除持久化快照，不修改 live hardware。HIL 必须在 clear
前后用 HTTP GET 与普通状态端点对同一实时状态作对比，不允许为了验证 clear 而重新上下电。
CDC 只证明快照摘要/删除结果和命令路径一致，不证明实时逐项状态：

- clear 前记录 `snapshot.present=true`、数值 `pending` 和 save 的
  `saved_items`。
- clear 后记录 `snapshot.present=false`、`pending=0`，并确认实时读数不变。

任何不一致记为 `[failed: clear altered live hardware]`，不通过。

#### Capture/OTA busy 排斥

Capture 测试必须占用逻辑分析仪或 sigrok capture arbiter；`adc record` 不是替代品。
在 capture 活跃时，save 和 clear 都必须返回 HTTP 409、`error.code=busy` 与
`activity=capture`。在 OTA upload 活跃时，config write（save 和 clear）都必须
返回 HTTP 409、`error.code=busy` 与 `activity=ota`。

GET 必须在有界时间内完成；GET 从不获取 owner，但不能把所有准备状态都错误地断言为 busy。capture 或 OTA 分别活跃时，save 与 clear 都必须返回 busy，`activity` 分别为 `capture` 与 `ota`。未来 HIL 必须为这些状态记录准确且有界的 HTTP 结果，绝不以超时、无限重试或 ADC record 代替 arbiter 验证。

#### OTA 与 combined-UF2 保留

配置持久化不应改变 OTA 或 combined-UF2 恢复契约。每次 HIL 必须使用上面的
combined UF2 完成 ROM BOOTSEL 恢复，并只将上述 OTA bin 上传到 OTA 路径。任何
缺失合成镜像、OTA 镜像、已识别 CDC device 或物理刷写条件的路径记为 `[blocked]`。
Runner 的 OTA upload 使用 `--limit-rate 64K`，使状态与 busy 仲裁可在有界的活跃窗口中观测。

#### CDC config 与 BOOTSEL fallback

CDC ACM fallback 必须通过已识别的 `--serial <identified-cdc-device>` 访问，禁止
直接写入假定的 tty 路径。CDC 命令集为 `config show`、
`config save [--confirm] <firmware-item-id>...`、`config clear`。当 HTTP 不可用时，必须能用 CDC 触发同样的查询与 mutate，且
HTTP/CDC 两条路径在 BOOTSEL 切换后都能恢复。

#### 最终安全清理

Todo 16 实机 HIL 在结束前必须由 HTTP 与普通端点逐项断言最终状态：没有 snapshot，
`switch/sd=target`、`switch/tf_wp=writable`，
所有测试过的 GPIO 都为 input，所有测试过的 power output 都为
off，包括 `vdd_5v` 和其子轨 `5v_usb_hub`；若保留了预置快照，必须另外记录
snapshot 中的五项电源值以及重启后的实际 replay 结果。还必须解除 NCM DHCP 占用、恢复开始前记录的默认路由/DNS，并释放 terminal、
刷写工具和 CDC device 句柄。清理步骤缺失任意一项即视为本次 HIL `[blocked]`，
不可被记录为 pass。

CDC `config show` 只断言无快照摘要和 fallback 可用性，不作为最终逐项状态的证据。

Runner 根据固件返回的列表枚举所有可控 power output 并逐个关闭，再把 direction 为
output 的 GPIO 逐个切回 input；随后再次 GET 读取并验证两类状态。

#### 证据与报告

每次真实硬件 HIL 必须产出以下产物并归档到 `docs/testing/results/`：

```text
docs/testing/results/<YYYY-MM-DD>-persistent-config-hil.md
docs/testing/results/<YYYY-MM-DD>-persistent-config-hil.raw.jsonl
docs/testing/results/<YYYY-MM-DD>-persistent-config-hil.serial.log
docs/testing/results/<YYYY-MM-DD>-persistent-config-hil.SHA256SUMS
```

报告正文必须列出：

- 板子身份、固件版本、UF2 / OTA bin 的 SHA256 与本次 SHA256SUMS 摘要。
- 每条 HIL 命令的入参、出参与耗时，标记为 pass / `[blocked]` / `[failed]`。
- 所有 `[blocked]` 的原因；不得把 `[blocked]` 记为 pass。

本节既定义 Todo 16 实机 HIL 的验收要求，也保留后续板卡回归的相同边界。
2026-08-05 v1 save-and-apply 真实硬件 HIL 的实际执行结果为 PASS，权威证据见
[日期 v1-save HIL 报告](results/2026-08-05-persistent-config-v1-save-hil.md)；2026-07-30 真实
硬件 HIL 的六个 runner flow 实际执行结果同样为 PASS，权威证据见
[历史六 flow 报告](results/2026-07-30-persistent-config-hil.md)；Todo 14 本地证明
及未来本地测试仍不构成或替代真实硬件 HIL。

### 2b. Rust TUI 键盘、鼠标与确认行为

修改 `cmd-ng` 的 TUI 渲染、导航、鼠标命中或真实硬件控制逻辑时，必须用当前源码
的 release build 启动 TUI，并在 `80x24` 和 `120x32` 两种尺寸中验证。操作期间用
另一终端的 CLI/HTTP 读取实际硬件状态，不得只根据 TUI 本地显示判定通过。

必测项：

- 常态区域不得出现外框或空白分隔行；确认浮层是唯一允许带边框的区域。顶部状态
  固定两行；无 ADC 数据时遥测占零行，有数据时示波器固定七行（一行表头、六行
  图形）。四路电流必须按确定性列宽并排显示：80 列为 20/19/19/19，零基 gutter
  位于 20、40、60；120 列为 30/29/29/29，零基 gutter 位于 30、60、90。每路按可见峰值和
  最新值的 `ceil(max(..., 1) * 5 / 4)` 独立缩放，
  不得使用固定 5000 mA 量程。
- Controls、Saved Config、Status 三页页签和当前页表头各固定一行，Tab/Shift+Tab
  在三页间前后切换，且只有活动页签使用反色/粗体焦点。
- Controls 每个 Power、Switch 对象严格占一行并使用稳定列。状态快照的顶层
  `gpio_layout_groups` 是连接器布局权威：x1.2 必须包含
  `J8/right/order=0` 和 `J13/right/order=1`。known group 先按 `order` 排序，组内
  只按固件给出的 `layoutRow/layoutColumn` 排序；这些坐标是元件面俯视物理网格，
  即使 `edge=right` 也**不得镜像** row/column。
- GPIO 只消费固件返回的
  `layoutGroup/layoutLabel/layoutRow/layoutColumn`：48 列及以上时同一固件物理行
  最多两个独立 cell，低于48列时每针单行，不得跨固件行配对。`edge=right` 仅由
  group metadata驱动右侧/板外方向的`›`标记，不得按`J8`、`J13`或任何GPIO名称
  特判。未提供组级metadata的组保留首次出现顺序；未知edge使用通用外观，不猜测
  出线方向。缺失/非法引脚坐标或坐标冲突的引脚进入通用fallback，并按快照顺序
  保留可达性；host不得硬编码连接器、pin map或镜像规则。
- 固件GPIO数组和布局图只描述可控制safe-GPIO子集，不是完整connector pinout。
  J8/J13的GND、保留脚和空位必须保持inert，不得命中其它引脚。GP17/GP18不属于
  当前safe/control目录，不能出现在通用GPIO组或J8/J13物理图。无ADC数据的密度
  基准fixture须保持`dense_layout_tests.rs`的预算：80x24至少14行、120x32至少20行。
  实板有ADC时保留七行示波器，按实际目录及物理行配对核对可见数量，不能为凑数
  虚构引脚或跨行配对；所有项目必须可滚动到达。Saved Config每个条目占一行，
  Status每个switch或监控字段占一行。
- 当前 Power、Switch 与 Saved Config 行的焦点背景必须从第 0 列铺到终端最后一列；
   GPIO 焦点只覆盖自己的半行 cell，不能覆盖或隐藏兄弟 cell。电源 ON/OFF 分别为
   绿/深灰；switch ready/pending/mismatch 分别为青/黄/红；GPIO 必须显示
   `◌ IN LOW`、`◌ IN HIGH`、`○ OUT LOW`、`● OUT HIGH`，LOW 为无浅色背景的
   深灰，HIGH 为粗体红色。在途动作必须追加 `[LOW…]`、`[HIGH…]` 或 `[INPUT…]`，
   同时继续显示动作前最近一次固件权威方向/电平，直到动作后的状态刷新完成。
- 最后一行固定为 htop 式键位栏；键块与操作标签必须视觉可分，每个键/操作单元
  要么完整显示、要么整段省略，不得在右边界截断半个单元。确认和错误状态必须
  替换常态键位提示。
- 上下键按共享视觉投影移动并在可能时保持 GPIO 左右位置；左右键只能在同一投影行
  的兄弟 GPIO cell 间切换。PgUp/PgDn 与 `g` 必须使用同一投影，每个 GPIO 都可达。
  Controls 与 Saved Config 必须自动滚动并保持当前项可见；Status 直接滚动，各页
  滚动位置彼此独立。
- Power/Switch 的鼠标命中区域覆盖整行；成对 GPIO 的左右半行矩形必须互不重叠、
  分别命中对应引脚，空半行必须 inert。GPIO 的 `l`/`L` 驱动 LOW、`o`/`O` 驱动
  HIGH、`i`/`I` 恢复 input；Enter/Space/`0`/`1` 对 GPIO 均为 inert。鼠标左键在
  600ms 前释放后进入 220ms await-second 窗口，窗口到期才驱动 LOW；按住到 600ms
  一次性驱动 HIGH，同一引脚在窗口到期前第二次按下/释放恢复 input 且无瞬态 LOW；
  中键、右键始终 inert。这些 direct action
  在非 GPIO 行不得产生硬件请求。
- GPIO 手势 HIL 必须验证同一终端 cell 内的 Moved 或左键 Drag 报告保持有效，只有
  跨到不同 pin、终端列或终端行才取消；还必须验证 600ms 到期的 Up 派发原始 pin 的
  HIGH，以及 await-second 到期的第二次 Down 先派发原始 pin 的 LOW 并消费该次
  Down，不能产生 input 瞬态。
- GPIO 使用安全测试脚（默认 GP13）：从 input 开始，依次验证 `l` 与左键单击均
  得到 `output LOW`，`o` 与左键按住 600ms 均得到 `output HIGH`，`i` 与同脚
  左键双击均恢复 input。
  每一步都从 API 回读；LOW 必须显示为无浅色背景的深灰、HIGH 必须显示为粗体红色，
  且颜色不依赖 input/output 方向。动作执行期间还必须验证对应 pending 标签与动作前
  权威状态同时可见，完成或失败后标签消失；左键按住期间必须显示 `[HOLD…]`，
  释放或派发后消失。
- 每个 Power 控件第一次操作只打开红色边框确认框；取消和超时都不得改变实际
  电源，三秒内再次 Enter/Space 或点击 Confirm 只执行一次状态切换。
- TUI 的 switch 激活策略对所有固件广告的 switch 一律使用同一三秒确认框，
  不再区分固件 `requires_confirm`。被点击的 switch 整行焦点在确认框打开后
  仍保留合成 `accent.select` 背景，被模态遮住之外的 cell 不会被取消或替换。
  第一次点击不发送任何 PUT；只有一次有效确认才会路由一次。命名测试
  覆盖：作为动态产品合同的代表，下面两条命名 case 验证新策略下
  `switch/sd` 与 `switch/tf_wp`（两者在本发布版里都不带
  `requires_confirm`）走完整的「点击 → 弹出确认框 → 0 个 PUT →
  取消/超时保持 0 个 PUT → 再点击 → 新确认 → 1 个 PUT」路径：
  - `tui_switch_pointer_sd_first_click_zero_put_then_fresh_confirm_routes_once`
  - `tui_switch_pointer_tf_wp_first_click_zero_put_then_fresh_confirm_routes_once`
  这两条 case 必须以 `switch/sd` 与 `switch/tf_wp` 为唯一产品名锚点；
  任何新增的 firmware-advertised switch 都按同一策略自动进入 TUI 确认门控，
  不再为本策略硬编码任何生产 switch 名称或路由值。x1.2 不再提供 VIN/VIO
  路由，因此不执行旧版 VIO 确认框或电压切换用例。
- 上述命名 TUI switch case 与所有 2b 节其它 mouse / confirm / GPIO HIL 都
  必须在与运行 HIL 的用户无关的环境中执行：runner 不允许假定任何固定
  user PID、`HOME` 路径、shell 启动文件、tmux server 名称或
  `radxa-linkr-debuggerctl` 二进制绝对路径；runner 必须按当前 shell 的实际
  UID/PID 自动隔离临时目录、`adc record` 输出文件、`tmux send-keys` 目标
  session 与任何 mock HTTP 后端。该隔离保证不同用户、不同主机、不同 CI 上
  重跑同一组 case 都不会读到其它用户的历史产物，也不会把结果误算到非本
  用户头上。
- 硬件确认事件 3 秒截止（当前 release）必须独立于 tick：在确认框打开后，
  必须分别验证 (a) 严格早于 `started + CONFIRM_TIMEOUT` 的 Enter/Space 或
  Confirm 点击执行硬件切换；(b) 达到或超过 `started + CONFIRM_TIMEOUT` 才
  到达的确认事件被识别为过期，发出该命令的 `timeout_message`，不执行硬件
  切换。判定在确认事件自身完成，不得依赖下一次渲染或 poll tick 先观察到
  超时；该 case 只引用当前 release 的源码契约，HTTP 回读必须在超时分支后
  显示硬件未变。
- GPIO 直接键修饰键过滤（当前 release）必须覆盖四类输入：(a) 小写
  `l`/`o`/`i` 不带任何修饰键 → 派发对应 DriveLow / DriveHigh / SetInput；
  (b) 大写 `L`/`O`/`I` 且修饰键恰好为 Shift → 同样派发对应意图；
  (c) Ctrl/Alt/Super 任意组合，无论大小写，均返回 `None`，无硬件请求且
  不取消既有手势；(d) 大小写与 Shift 失配的组合（小写配 Shift、大写无 Shift）
  也返回 `None`，同样无硬件请求且不取消既有手势。每一步必须用 HTTP 回读
  验证硬件状态未发生变化，且既有 GPIO 手势（若处于 Down 或 AwaitSecond）
  保持不变。
- 终端 `Resize` 重绘边界（当前 release）必须验证：`Event::Resize` 到达
  时取消任何进行中的 GPIO 手势（同时清掉初始左键 Down 与 AwaitSecond
  状态），整体清空 `hit_map` 让旧命中矩形失效，并在消费下一个排队输入
  事件前完成一次强制重绘。Resize 之后排队的鼠标事件必须按新几何评估，
  不得落在旧 cell 上。该 case 必须包含两次 Resize 之间的 Down→队列→
  Resize→Up 序列，证明旧几何不会派发任何硬件请求。
- 正常退出、连接错误和 Ctrl+C 后，raw mode、alternate screen、鼠标捕获与光标状态
  都必须恢复。
- 安装后的 `rdb` 与 `radxa-linkr-debuggerctl` 对 `--version`、`--help` 和
  `--json status` 返回相同退出状态与输出；Unix 必须是指向主程序的相对符号链接，
  Windows 两个 `.exe` 必须为同一硬链接或具有相同 SHA256。

结束前把所有测试过的 GPIO 恢复为 input、Power 恢复为测试前 HTTP 权威基线，并
按本规范验证 HTTP/WS、CDC fallback、HTTP BOOTSEL 与 CDC BOOTSEL。证据写入
`docs/testing/results/<YYYY-MM-DD>-cmd-ng-tui-controls-hil.md`，并保存 80x24/120x32
三页、带四通道示波器的 Controls、GPIO 双 cell 选择态及确认态终端截图，明确区分
真实板卡 HIL 和 TestBackend/mock-board 本地 QA。

### 2c. 固件目录自动化任务

MASKROM/EDL 来自固件拥有的不可变目录 `GET /api/v1/tasks/catalog`，host 不
保存任何恢复配方，也不能新增固件专用 API、WebSocket 命令、CDC 命令、
开机默认值或 replay 路径。修改固件目录、通用 task runner 或 catalog 解析
后，必须验证：

- `task list` 列出 `GET /api/v1/tasks/catalog` 返回的条目，每行
  `source=builtin`；原始 `GET /api/v1/tasks` 仍只报告 stored entry。两个
  拉取相互独立，catalog 不可用时 `task list` 仍展示 stored task 并带
  `catalog_error`，`task run builtin/...` 直接返回 `catalog_unavailable`。
- 缺少 `--confirm` 的 `task run` 必须在零 transport request 时返回
  `confirmation_required`；所有实际执行使用 `task run builtin/... --confirm`。
- confirmed built-in 不读取或修改 `/api/v1/tasks`，主序列与 cleanup 序列
  完全由固件目录声明的 `PUT` 记录驱动，host 不在客户端硬编码
  `CON_MAS`/`rail`/等待步骤。客户端等待仅在每个 PUT 成功之后按记录的
  `wait_ms` 应用。
- MASKROM 把 `CON_MAS` 拉低、EDL 把它拉高；成功后 `CON_MAS` 回读为 input，
  所选 rail 回读为 on（具体行为以固件目录为准）。首次失败立即停止主序列，
  随后只运行固件目录声明的 cleanup 序列；主错误保持权威，cleanup 结果单独
  记录，不允许 host 推断 rail 回滚。首条请求前取消不 cleanup，stored task
  失败或取消也不推断 cleanup。
- stored task 与固件目录声明的 ID 碰撞时，客户端列表标记 stored entry 被
  遮蔽，执行时 built-in 胜出；`task clear` 只删除 stored entry，目录条目
  继续存在。
- 只有连接了由所选 rail 供电、且可独立确认 USB ROM 枚举的目标夹具，才能
  把对应 MASKROM/EDL 行记为 pass；缺少兼容目标或 rail 接线必须标记
  `[blocked]`。

HIL 前后逐字节保存并恢复 `/api/v1/tasks` blob，记录全部 Power/GPIO/Switch 基线；
结束时把 `CON_MAS` 恢复 input，其余硬件状态恢复基线。证据写入
`docs/testing/results/<YYYY-MM-DD>-builtin-automation-tasks-hil.md`。

### 3. 电源输出 get/set

```sh
timeout 5s radxa-linkr-debuggerctl --json power set 12v_out on
timeout 5s radxa-linkr-debuggerctl --json power set 12v_out off

timeout 5s radxa-linkr-debuggerctl --json power set 5v_out on
timeout 5s radxa-linkr-debuggerctl --json power set 5v_out off

# vdd_5v 是普通可控父轨：出现在列表中，可 get/set。
timeout 5s radxa-linkr-debuggerctl --json power list > /tmp/linkr-power-list.json
grep -q '"name":"vdd_5v"' /tmp/linkr-power-list.json
grep -q '"name":"5v_usb_hub"' /tmp/linkr-power-list.json
timeout 5s radxa-linkr-debuggerctl --json power get vdd_5v
timeout 5s radxa-linkr-debuggerctl --json power set vdd_5v on
timeout 5s radxa-linkr-debuggerctl --json power get vdd_5v
timeout 5s radxa-linkr-debuggerctl --json power set vdd_5v off

# 5v_usb_hub 是 vdd_5v 的子轨，启动关闭，且没有独立 INA226/ADC descriptor。
timeout 5s radxa-linkr-debuggerctl --json power get 5v_usb_hub
timeout 5s radxa-linkr-debuggerctl --json power set 5v_usb_hub on
timeout 5s radxa-linkr-debuggerctl --json power get vdd_5v
timeout 5s radxa-linkr-debuggerctl --json power get 5v_usb_hub
timeout 5s radxa-linkr-debuggerctl --json power set 5v_usb_hub off
timeout 5s radxa-linkr-debuggerctl --json power get vdd_5v

# x1.2 已删除 switch/usb；GET 必须 404，且不得改变独立的 vdd_5v。
timeout 5s curl -sS -o /tmp/linkr-switch-usb.json -w '%{http_code}\n' \
  http://172.29.203.1/api/v1/switch/usb | grep -qx 404

# 注意：vdd_5v 是 USB hub 的辅助 5 V 父轨；关闭会使 hub 及其下游设备断电。

timeout 5s radxa-linkr-debuggerctl --json power set 20v_out on
timeout 5s radxa-linkr-debuggerctl --json power set 20v_out off
```

父轨/子轨 setter 必须验证以下顺序，并以每次 GET 的固件状态为准：

1. `5v_usb_hub=on` 时若 `vdd_5v` 原为 off，固件必须先打开 `vdd_5v`，随后
   `5v_usb_hub` 和 `vdd_5v` 都读回 on。
2. `5v_usb_hub=off` 只关闭子轨，`vdd_5v` 保持原状态。
3. `vdd_5v=off` 必须先把 `5v_usb_hub` 关闭，再关闭父轨；两项都读回 off。
4. 仅 `vdd_5v=on` 不得隐式打开 `5v_usb_hub`。
5. 只保存 `power/5v_usb_hub` 的 v1 child-only snapshot 后重启，重放必须按
   parent-before-child 顺序把 `vdd_5v` 和 `5v_usb_hub` 都恢复为 on；
   `GET /api/v1/config` 的 `pending` 必须为 0。
6. `/api/v1/adc/read` 必须仍为八路；不得出现 hub descriptor，`vdd_5v` 读数是
   包含 hub 支路的父轨总量。
7. Hub 物理供电必须用下游 USB 枚举证明，不得只记录 GPIO/API 状态：hub 关闭时
   有界轮询 `lsusb` 不得出现 `067b:2731`；打开后有界等待出现
   `067b:2731 Prolific Technology, Inc. USB SD Card Reader`；关闭后设备消失；
   再次打开必须重现同一设备。不得挂载、读写该读卡器。该证据证明 U38 /
   `5V_USB_HUB` 实际提供下游 VBUS。
8. 结束前 GP5/GP6 必须为 input，`switch/sd=target`、`switch/tf_wp=writable`，
   `5v_usb_hub=off` 且 `vdd_5v=on`。

### 4. ADC 电流与 pad 电压监测

四路 INA226 和 CH224A 共用 GP20/GP21 I2C 总线。400 kHz 生产默认的合格基线是：
生产 BOM 在 SDA/SCL 各装 4.7 kΩ 到常供电板载 VCC_3V3，J8 空载，固件保持
`bias-disable`、Schmitt 输入和开漏输出。执行本节前必须核对这些条件；任何偏离都
不能沿用基线结论：

- 绝不能启用 RP2350 internal pull-up 来替代或并联 BOM 上拉。其 RPU 为
  32–86 kΩ，会形成未受控的附加 VCC_3V3 通路。
- 任何接入 J8 的器件、线缆、上拉或异域电源都构成新的总线配置。禁止任意上拉和
  跨域上拉；必须重新核算并实测总 `Cb`、30%–70% `tr`、`VOL`/灌电流及所有
  powered/unpowered 电流路径，未完成时外接400 kHz声明记为 `[blocked]`。
- CH224A CFG2/CFG3 的7/10/15 kΩ internal pull-up支持400 kHz，但datasheet未说明
  上拉轨，也未给出VHV=0 V时powered-off Ioff；CFG绝对最大值为6.5 V。
  不能把内部上拉或轮询成功当作独立的合格上拉、掉电隔离或Ioff安全证据。
- INA226 SCL 的绝对最大值是 `VS + 0.3 V`；当 `VS = 0 V` 时，SCL 被拉到 3.3 V
  明确违规。SDA 的绝对最大值虽为 6 V，但没有 powered-off Ioff 规格，仍须测量
  反向电流。板上 ESD5451N 是 5 V TVS，不会把总线钳在 3.3 V，也不能作为掉电
  隔离或反灌保护；这是非频率特定的硬件边界。
- ADC0通过R46/R47（100 kΩ/15 kΩ，full 115 kΩ）观测Q28之后、U10之前的
  `DC20V`源侧分压，不是直接测量CH224A VHV。CH224A节点只通过Device Tree引用
  这个私有标准`voltage-divider` sensor；它不与公共telemetry直接共用sensor实例。
  底层raw ADC访问仍由RP2350 ADC driver的`adc_context`在ADC0及其它通道之间
  串行化；源侧电压换算必须由firmware按Device Tree完成，不能放到host。
- CH224A节点使用标准`zephyr,deferred-init`；自动POST_KERNEL init必须被跳过，
  再由`main`在PIO I2C和voltage-divider已ready后显式`device_init()`一次。该init
  必须bus-silent，完整target build的init-priority checker不得报告依赖倒序。
- 每笔真实CH224A I2C read/write之前必须fresh-sample ADC gate，包括probe每个地址/
  attempt、每次I2C retry、48个capability字节和PPS两次write。absent/unknown要求
  连续3次`>=2800 mV`、间隔5 ms后才可启动第一笔I2C（总稳定时间至少10 ms）；
  present后每笔fresh sample `>=2500 mV`可继续，2500–2800 mV仅保持已present。
  `<2500 mV`、sensor not-ready、fetch/get/换算错误必须立即清presence并阻止剩余
  retry/write。status和capability在最后一字节后还必须进行一次fresh终点检查。
- CH224A `0x09` status的有效性只取决于I2C ACK和成功的一字节读取，不能把寄存器
  值当作器件身份或协议校验。仅解析已命名mask；`0x00`、`0x80`、`0x88`、`0xff`
  均不得仅因unknown/reserved bit而失败、重试、recover或清PD状态。每次启动首次
  观察到未知位时应有一条包含raw `0x%02x`值的warning，后续不得刷屏。该warning
  不属于transport failure，也不能触发5秒probe backoff。
- gate/ADC/I2C错误使PD unavailable，清除旧gear/PPS/request和capabilities并进入
  5秒probe退避；除新source的稳定上升边沿外，退避期不得产生CH224A事务。独立
  低优先级thread约每100 ms只轮询ADC gate，不使用system workqueue，present稳态
  不读status/capability。稳定复插边沿应清旧source的probe backoff并自动执行一次
  probe/current status，使恢复不依赖客户端轮询，但不能复活旧请求。完整读取但无
  有效PDO的`-ENODATA`只进入capability退避；short status不得发布过期的5秒
  capability缓存。5 V fallback可跳过capability读取，但不能绕过ADC gate。
- CH224A probe仅可对`-ETIMEDOUT`/`-EBUSY`执行一次标准`i2c_recover_bus()`；普通
  地址/数据NAK的`-EIO`不得触发bus clear。recovery在发SCL脉冲前和retry前都要
  fresh ADC gate，失败后维持unavailable和5秒退避。PIO recovery必须继续在bus
  lock下fail-close，latch=0，仅以OE/PINDIRS下拉或释放，禁止internal pull和主动
  drive HIGH。`CONFIG_I2C_SHELL=n`保持不变，不增加任意地址/配置入口。
- ADC gate的板级HIL保持`pending`：后续应独立覆盖absent启动/状态/5 V/20 V/PPS
  零I2C、三次稳定、迟滞、ADC错误、retry间掉电、capability第N字节掉电、PPS两写
  之间掉电、约100 ms无人客户端自动拔插恢复、稳态无重复probe/capability、5秒
  backoff、新源快速重入，以及stuck/timeout时单次恢复和普通NAK不恢复。stuck
  recovery必须只观测既有/安全故障条件；未经另行授权不得用外部拉线制造电气短路。
  这些项目不是本次400 kHz数字功能放行前提。
- gate与约100 ms轮询只能在观测到低电压后阻止**下一笔**事务；它不是IRQ，
  ADC采样与I2C START之间仍有
  TOCTOU，正在进行的事务不能撤销。它也不能消除DC20V残压/反灌、外部4.7 kΩ
  静态路径、未供电器件路径或与INA226共享的SCL波形。不得声称VHV无效时总线
  零波形、powered-off Ioff合格或已实现物理隔离。
- 400 kHz 生产默认的原始功能HIL夹具使用与正式BOM等值的SDA/SCL各4.7 kΩ
  飞线上拉到板载VCC_3V3，上拉随板掉电，J8空载，VHV/PD全程在线，且
  **不执行单边掉电**。4.7 kΩ 分别与 CH224A 7/10/15 kΩ internal pull-up
  并联后为 2.812/3.197/3.579 kΩ；按已知 34 pF 估算
  `tr = 81.0–103.1 ns`，3.3 V 低态上拉电流为 0.922–1.174 mA。4.7 kΩ
  单独对应 `tr = 135.4 ns`、`Cbmax = 75.33 pF`；最坏并联值对应
  `Cbmax = 98.94 pF`，扣除已知 34 pF 后余量为 64.94 pF。报告仍须保存
  示波器实测 tr/VOL；这些估算不能替代测量，也不能证明完整UM10204 analog/
  50 ns spike-filter合规或VHV=0/VCC_3V3=0时的Ioff/反灌安全。
- 400 kHz 软件路径 HIL 需覆盖普通 data/ACK、STOP、repeated START 和九脉冲
  recovery。对 STOP、repeated START、每个 recovery high 及 recovery tail STOP，
  必须确认 SCL 释放后等待实际 SCL high，再从实际 high 起满足 setup/high 时间；
  人为拉低 SCL 的测试必须在有界超时后看到控制器 fail-close、两线释放，不能卡死
  CPU。Device Tree 对 SDA/SCL 启用 Schmitt hysteresis 且保持 `bias-disable`；
  Schmitt 并不自动证明 UM10204 的 50 ns spike filter，因此本项不得写成完整
  UM10204 认证。

```sh
timeout 5s radxa-linkr-debuggerctl --json adc read
timeout 5s radxa-linkr-debuggerctl --json adc read 5v_out
timeout 5s radxa-linkr-debuggerctl --json adc read 12v_out
timeout 5s radxa-linkr-debuggerctl --json adc read 20v_out
timeout 5s radxa-linkr-debuggerctl --json adc read vdd_5v
timeout 5s radxa-linkr-debuggerctl --json adc read adc0
timeout 5s radxa-linkr-debuggerctl --json adc read adc1
timeout 5s radxa-linkr-debuggerctl --json adc read adc2
timeout 5s radxa-linkr-debuggerctl --json adc read adc3
```

验证：

- 返回恰好八个 descriptor，顺序固定为 `5v_out`、`12v_out`、`20v_out`、
  `vdd_5v`、`adc0`、`adc1`、`adc2`、`adc3`。第五路电源输出
  `5v_usb_hub` 没有 INA226/ADC descriptor；不得为它伪造电流或电压通道，
  `vdd_5v` 读数是包含 hub 支路的父轨总量。
- 前四个是 INA226 电流通道，后四个是 ADC pad 电压通道；四个电压通道都没有
  `power_enabled`。`adc0` 带固件提供的 `PD input divider; ADC pad voltage` note，
  但公开值仍是 ADC0 pad 电压，不能乘 `115/15` 还原源侧电压。
- `raw / mv / current_ua / sensor_value` 可解析，host 不做校准或零点修正。

### 4b. Eight-channel Compact Telemetry & GP29 Ownership

本节与第 4 节组合形成 ADC 验收表面。完整合约以
[docs/reference/adc-telemetry.md](../reference/adc-telemetry.md) 为准；这里只列出 HIL
要确认的最小集合。当前固件固定上报八个 descriptor，顺序为
`5v_out`、`12v_out`、`20v_out`、`vdd_5v`、`adc0`、`adc1`、`adc2`、`adc3`：
前四个是 INA226 电流通道，后四个是纯监测 ADC pad 电压通道。

2026-10-21 的 x1.2 板卡 HIL 已覆盖 4b.1 HTTP 八 descriptor 固定顺序、4b.2/4b.3
WebSocket 八通道与 18×8 batch、CLI 八通道、内嵌 Web row/pinout，以及 GP29 input-only；
权威证据见
[2026-10-21 ADC/pinout/J4 报告](results/2026-10-21-x1.2-adc-pinout-j4-hil.md)。
该报告没有执行 ADC0 的真实 PD-source 物理拔出/重插自动 monitor，也没有执行 stuck-line
电气故障注入；这两项仍为 `pending`，不得从数字协议/常态读数外推为通过。历史
[2026-07-31 报告](results/2026-07-31-adc3-telemetry-hil.md)仅保留为旧四 descriptor
背景。`capture_arm` / `subscribe` 的 `rate_hz`
范围为 `1..250`，250 Hz 是当前上限；超过 250 必须返回 `invalid_rate`，不得
静默 clamp。rolling nightly 检查（第 12h 节）独立于本节，不在本报告范围内。

**4b.1 HTTP rich read shape (`required`, eight-descriptor digital shape passed 2026-10-21)**

```sh
timeout 5s curl -fsS http://172.29.203.1/api/v1/adc/read > /tmp/linkr-adc-all.json
for channel in 5v_out 12v_out 20v_out vdd_5v adc0 adc1 adc2 adc3; do
  timeout 5s curl -fsS \
    "http://172.29.203.1/api/v1/adc/read?channel=$channel" \
    > "/tmp/linkr-adc-$channel.json"
done
python3 - <<'PY'
import json
from pathlib import Path

names = ['5v_out', '12v_out', '20v_out', 'vdd_5v',
         'adc0', 'adc1', 'adc2', 'adc3']
current_names = set(names[:4])
voltage_names = set(names[4:])
payload = json.loads(Path('/tmp/linkr-adc-all.json').read_text())
readings = payload.get('readings')
assert isinstance(readings, list) and len(readings) == 8, 'HTTP must return 8 readings'
assert [reading.get('name') for reading in readings] == names, 'HTTP descriptor order mismatch'

for reading in readings:
    name = reading['name']
    if name in current_names:
        assert reading.get('sensor_channel') == 'current' and reading.get('unit') == 'A'
        assert isinstance(reading.get('current_ua'), int)
        assert isinstance(reading.get('power_enabled'), bool)
        assert isinstance(reading.get('voltage_valid'), bool)
        if reading['voltage_valid']:
            assert isinstance(reading.get('voltage_uv'), int)
        else:
            assert reading.get('voltage_uv') is None
    else:
        assert name in voltage_names
        assert reading.get('sensor_channel') == 'voltage' and reading.get('unit') == 'V'
        assert isinstance(reading.get('raw'), int) and isinstance(reading.get('mv'), int)
        sensor_value = reading.get('sensor_value', {})
        assert isinstance(sensor_value.get('val1'), int) and isinstance(sensor_value.get('val2'), int)
        pad_uv = sensor_value['val1'] * 1_000_000 + sensor_value['val2']
        assert abs(pad_uv - reading['mv'] * 1000) < 1000, f'{name} is not direct pad voltage'
        for forbidden in ('value', 'current_ua', 'power_enabled', 'voltage_valid', 'voltage_uv'):
            assert forbidden not in reading, f'{name} must not carry {forbidden}'

assert readings[4].get('note') == 'PD input divider; ADC pad voltage'
for index, name in enumerate(names):
    single = json.loads(Path(f'/tmp/linkr-adc-{name}.json').read_text()).get('readings')
    assert isinstance(single, list) and len(single) == 1 and single[0].get('name') == name
print('eight-channel HTTP ADC shape accepted')
PY
```

上面的 `pad_uv` 与 `mv` 一致性断言同时禁止把 ADC0 的公开 `sensor_value` 换算为
分压器源侧电压。host 不做 ADC 校准、零点修正或 `115/15` 倍乘。

**4b.2 WebSocket compact single-sample shape (`required`, passed 2026-10-21)**

```text
timeout 5s node <<'NODE'
const base = 'http://172.29.203.1';
const session = await fetch(`${base}/api/v1/live-sessions`, { method: 'POST' })
  .then((r) => {
    if (!r.ok) throw new Error(`session create failed: HTTP ${r.status}`);
    return r.json();
  });
const rawUrl = session.ws_url ?? session.session?.ws_url;
if (!rawUrl) throw new Error('session response missing ws_url');
const wsUrl = new URL(rawUrl, base);
wsUrl.protocol = wsUrl.protocol === 'https:' ? 'wss:' : 'ws:';

await new Promise((resolve, reject) => {
  const ws = new WebSocket(wsUrl);
  const timer = setTimeout(() => reject(new Error('eight-channel single frame timeout')), 5000);
  ws.addEventListener('open', () => {
    ws.send(JSON.stringify({ type: 'subscribe', topic: 'live', rate_hz: 250, id: 'adc8-single' }));
  });
  ws.addEventListener('message', (event) => {
    const data = JSON.parse(event.data);
    if (data.type !== 'telemetry') return;
    if (!Array.isArray(data.readings) || data.readings.length !== 8) {
      reject(new Error('single frame must carry eight readings'));
      return;
    }
    const names = data.readings.map((r) => r.name);
    if (names.join(',') !== '5v_out,12v_out,20v_out,vdd_5v,adc0,adc1,adc2,adc3') {
      reject(new Error(`unexpected reading order: ${names.join(',')}`));
      return;
    }
    if (data.readings.filter((r) => r.kind === 'current').length !== 4 ||
        data.readings.filter((r) => r.kind === 'voltage').length !== 4) {
      reject(new Error('single frame must carry four current and four voltage readings'));
      return;
    }
    for (const reading of data.readings) {
      if (!['current', 'voltage'].includes(reading.kind)) {
        reject(new Error(`bad kind on ${reading.name}: ${reading.kind}`));
        return;
      }
      if (reading.kind === 'current' && reading.unit !== 'uA') {
        reject(new Error(`current unit mismatch on ${reading.name}: ${reading.unit}`));
        return;
      }
      if (reading.kind === 'voltage' && reading.unit !== 'uV') {
        reject(new Error(`voltage unit mismatch on ${reading.name}: ${reading.unit}`));
        return;
      }
      if (!Number.isInteger(reading.value)) {
        reject(new Error(`value is not signed integer on ${reading.name}`));
        return;
      }
      if (reading.kind === 'current' && typeof reading.power_enabled !== 'boolean') {
        reject(new Error(`current reading missing power_enabled: ${reading.name}`));
        return;
      }
      if (reading.kind === 'voltage' && 'power_enabled' in reading) {
        reject(new Error(`${reading.name} must not carry power_enabled`));
        return;
      }
      if (reading.kind === 'current' &&
          (!Number.isInteger(reading.current_ua) ||
           typeof reading.voltage_valid !== 'boolean' ||
           !Number.isInteger(reading.voltage_uv))) {
        reject(new Error(`current reading missing dual telemetry: ${reading.name}`));
        return;
      }
      for (const forbidden of ['raw', 'mv', 'sensor_value']) {
        if (forbidden in reading) {
          reject(new Error(`compact frame must not carry ${forbidden} on ${reading.name}`));
          return;
        }
      }
    }
    if (data.readings[4].note !== 'PD input divider; ADC pad voltage') {
      reject(new Error('adc0 firmware note missing'));
      return;
    }
    clearTimeout(timer);
    ws.close();
    resolve();
  });
  ws.addEventListener('error', () => reject(new Error('WebSocket error')));
});
console.log('eight-channel compact single-sample frame accepted');
NODE
```

记号含义：紧凑单样本帧没有 `raw`/`mv`/`sensor_value`；电流通道携带
`current_ua`、`voltage_valid`、`voltage_uv` 双遥测。`rate_hz` 的验收上限是
250；251 及以上必须返回 `invalid_rate`，不得 clamp。外部 WS 客户端以前如果按
旧 descriptor 数量或旧字段解析，必须迁移；没有 dual-emission shim。

**4b.3 WebSocket compact batch shape (`required`, 18×8 passed 2026-10-21)**

```text
timeout 5s node <<'NODE'
const base = 'http://172.29.203.1';
const session = await fetch(`${base}/api/v1/live-sessions`, { method: 'POST' })
  .then((r) => {
    if (!r.ok) throw new Error(`session create failed: HTTP ${r.status}`);
    return r.json();
  });
const rawUrl = session.ws_url ?? session.session?.ws_url;
if (!rawUrl) throw new Error('session response missing ws_url');
const wsUrl = new URL(rawUrl, base);
wsUrl.protocol = wsUrl.protocol === 'https:' ? 'wss:' : 'ws:';

await new Promise((resolve, reject) => {
  const ws = new WebSocket(wsUrl);
  const timer = setTimeout(() => reject(new Error('eight-channel batch timeout')), 5000);
  ws.addEventListener('open', () => {
    ws.send(JSON.stringify({
      type: 'subscribe', topic: 'live', rate_hz: 250, batch_size: 18, id: 'adc8-batch18',
    }));
  });
  ws.addEventListener('message', (event) => {
    const data = JSON.parse(event.data);
    if (data.type !== 'telemetry-batch') return;
    if (!Array.isArray(data.channels) || data.channels.length !== 8) {
      reject(new Error('batch channels must be 8'));
      return;
    }
    const names = data.channels.map((channel) => channel.name);
    if (names.join(',') !== '5v_out,12v_out,20v_out,vdd_5v,adc0,adc1,adc2,adc3') {
      reject(new Error(`unexpected batch channel order: ${names.join(',')}`));
      return;
    }
    for (let index = 0; index < data.channels.length; index++) {
      const channel = data.channels[index];
      const current = index < 4;
      if (channel.kind !== (current ? 'current' : 'voltage') ||
          channel.unit !== (current ? 'uA' : 'uV')) {
        reject(new Error(`batch channel metadata wrong: ${channel.name}`));
        return;
      }
    }
    if (data.channels[4].note !== 'PD input divider; ADC pad voltage') {
      reject(new Error('adc0 batch note missing'));
      return;
    }
    if (!Array.isArray(data.samples) || data.samples.length !== 18) {
      reject(new Error('batch must carry exactly 18 samples'));
      return;
    }
    for (const sample of data.samples) {
      if (!Array.isArray(sample.values) || sample.values.length !== 8 ||
          !sample.values.every(Number.isInteger)) {
        reject(new Error('sample values length mismatch'));
        return;
      }
      if (!Array.isArray(sample.voltages_uv) ||
          sample.voltages_uv.length !== 4 ||
          !sample.voltages_uv.every(Number.isInteger) ||
          !Number.isInteger(sample.power_enabled_mask) ||
          !Number.isInteger(sample.voltage_valid_mask)) {
        reject(new Error('batch missing four-channel INA226 voltage telemetry'));
        return;
      }
      if ((sample.power_enabled_mask & ~0x0f) !== 0 ||
          (sample.voltage_valid_mask & ~0x0f) !== 0) {
        reject(new Error('batch masks must use only four current-channel bits'));
        return;
      }
    }
    clearTimeout(timer);
    ws.close();
    resolve();
  });
  ws.addEventListener('error', () => reject(new Error('WebSocket error')));
});
console.log('eight-channel 18-sample compact batch frame accepted');
NODE
```

**4b.4 Web sparkline 10 Hz / 90-sample (`required`; eight-row presence/order and production tests passed 2026-10-21, full manual 90-point observation remains required when changed)**

用 Playwright 打开板载页面 `http://172.29.203.1/`，在 Power & measurements 卡片内
断言：四路电流 row（`5v_out`、`12v_out`、`20v_out`、`vdd_5v`）与 ADC0..ADC3
四个电压区块的 SVG 历史长度都不超过 90 个采样点，且在稳定供电下两个方向各能
保持 ~9 秒（`±1 秒`）的窗口宽度。ADC 区段按固件 descriptor 渲染并使用固定
0..3,300,000 µV Y 轴；ADC0 展示固件 note，但不得擅自 host-side 重新标定或分压
换算。Web 自动化节点使用既有的 `web/` 项目脚手架。

2026-10-21 当前固件与 production Web build 已确认八个 descriptor 的存在和固定
顺序，Web Node/Vitest 回归覆盖八路解析/渲染；历史 2026-07-31 实板直接观测过 ADC3
90-point / responsive 子集。若修改 sparkline 时间窗、八路布局或隐藏页 cadence，仍需
重新执行本节全部人工/浏览器 HIL；不能把协议顺序 PASS 写成所有通道的模拟精度或
独立 90 点视觉认证。

**4b.5 GP29 ADC3 ownership (`required`, direct ownership subcase `[passed]` by 2026-07-31 HIL; v1 snapshot compatibility subcases `pending`)**

```text
timeout 5s curl -fsS http://172.29.203.1/api/v1/gpio
timeout 5s curl -fsS http://172.29.203.1/api/v1/gpio/GP29
timeout 5s curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"direction":"output","value":1}' \
  http://172.29.203.1/api/v1/gpio/GP29   # must fail (firmware-owned input-only)
```

断言：

- `/api/v1/gpio` 列表中允许保留 GP29（在持久化/安全目录里），但写入方向必须是
  `output` 的请求被固件层拒绝（错误码与文本都要清楚说明输入唯一）。
- v1 输入快照继续解码、重放并在 firmware-defaults 之后自动恢复。
- v1 输出快照保留可解码，但重放命中 GP29 之后，整段
  replay 停在第一个硬件失败：GP29 `apply_state="failed"`，后续 GPIO output
  仍 `pending`，绝不能伪装成完整成功。

**4b.6 HIL 边界**

2026-10-21 当前八 descriptor 的 HTTP、WS single/batch、CLI、GP29 input-only 和
production Web 形状已通过，见当日报告。ADC0 实际 PD source 物理拔出/重插后的
约 100 ms 自动 monitor 恢复、stuck-line 安全故障注入和模拟电气测量未执行，仍为
`pending`。4b.5 中 v1 输入
快照继续解码并在 firmware-defaults 之后自动恢复、v1 输出快照在重放命中
GP29 时停在第一个硬件失败、GP29 `apply_state="failed"`
而后续 GPIO output 仍 `pending` 这两个历史 v1 snapshot 兼容子项仍
`pending`，等待未来真实板卡 HIL。rolling nightly 检查（第 12h 节）
与 4b 节无关，独立保持 `pending`。nightly 现仅在推送到 `dev` 分支时
触发，每次实际 dev-push run 都保持 `pending`，直到 GitHub 在新触发
条件下产出真实 Actions 执行 evidence（命名带日期与主题的产物/日志），
该 evidence 在本规范当前修订时仍不存在。宿端单测
（`cmd-ng/src/adc.rs` 与 `web/src/lib/adc.test.ts`）只证明 wire-shape
形状，不替代 4b 节真实硬件验收。actionlint 与仓库内本地结构校验不等价
于 GitHub 实际跑过 `.github/workflows/nightly.yml`；任何把
本地 lint 通过说成"nightly 已跑"的陈述仍属越权。

### 5. switch 路由控制

```sh
timeout 5s radxa-linkr-debuggerctl --json switch get sd
timeout 5s radxa-linkr-debuggerctl --json switch route sd target
timeout 5s radxa-linkr-debuggerctl --json switch get sd

timeout 5s radxa-linkr-debuggerctl --json switch get tf_wp
timeout 5s radxa-linkr-debuggerctl --json switch route tf_wp protected
timeout 5s radxa-linkr-debuggerctl --json switch get tf_wp
timeout 5s radxa-linkr-debuggerctl --json switch route tf_wp writable
timeout 5s radxa-linkr-debuggerctl --json switch get tf_wp
```

注意：

- `set` 后等待硬件稳定再 `get`
- 优先顺序验证，避免并行反向测试干扰
- x1.2只广告`sd`和`tf_wp`。GET `/api/v1/switch/usb`必须返回404，status、config、
  WS snapshot都不得出现`switch/usb`。
- `switch/sd`与`switch/tf_wp`分别切到另一合法值再恢复原值；切换前后回读GP5、
  GP6和`vdd_5v`，必须无副作用。
- `tf_wp` 启动默认必须是 `writable`（switch 关闭，GPIO22 驱动 Q12 将 GL3224 SD_WP 拉低）；`protected` 时 onboard USB 读卡器应表现为只读；GL3224 在插卡时采样 SD_WP，WP 变更需重新插卡或将 `switch sd` 切走再切回后生效

### 6. x1.2 已移除 VIN 与 USB 路由

x1.2 没有可切换的 VIN/VIO 1.8V/3.3V 电路，HTTP、CLI、CDC 和持久化目录
均不再提供 VIN 控制。不得执行旧版 `switch vin` / CDC `vin` 步骤，也不再
把 VIN 回读或恢复 3.3V 作为 HIL 清理条件；其它电源、路由、GPIO 和主机网络
清理要求不变。硬件事实见 [x1.2 BSP](../reference/hardware-x1.2.md)。

x1.2 同样没有 `switch/usb`；旧 ID 不能作为 alias。`/switch/usb` 返回404，旧
config ID使请求返回`unknown_item`，旧snapshot包含它时视为malformed。

### 7. safe GPIO

```sh
timeout 5s radxa-linkr-debuggerctl --json gpio list
timeout 5s radxa-linkr-debuggerctl --json gpio set GP13 1
timeout 5s radxa-linkr-debuggerctl --json gpio input GP13
timeout 5s radxa-linkr-debuggerctl --json gpio set GP13 0
timeout 5s radxa-linkr-debuggerctl --json gpio input GP13

for pin in GP5 GP6; do
  timeout 5s radxa-linkr-debuggerctl --json gpio input "$pin"
  timeout 5s radxa-linkr-debuggerctl --json gpio set "$pin" 1
  timeout 5s radxa-linkr-debuggerctl --json gpio set "$pin" 0
  timeout 5s radxa-linkr-debuggerctl --json gpio input "$pin"
done
```

同时验证以下板级与metadata合同：

- safe-GPIO允许列表为可输出`GP5`、`GP6`、`GP7`-`GP9`、`GP12`-`GP16`和仅输入
  `GP29`，共11项、其中10项可输出；GP17/GP18不得出现。
- GP5=`GPIO5_CON`/J8 pin16、GP6=`GPIO6_CON`/J8 pin15，均为普通RP2354直连
  GPIO，默认input且无pull，不得描述为USBMUX或TF switch。API和embedded Web
  都要执行input→HIGH→LOW→input并逐步回读。
- 完整物理目录为24个cell：11个safe control和13个inert；GP29保持input-only。
- 元件面俯视时J8左列从顶到下为pin 10→1，右列为pin 20→11；已确认控制/监测
  子集为pin1..10依次`GP12/GP13/GP14/GP15/GP16/GP29(ADC3)/GP28(ADC2)/
  GP27(ADC1)/GP20(SCL)/GP21(SDA)`。ADC0/GP26不是J8端子；该子集不是完整pinout。
- J13顶排为pin2 GP9/USER、pin3 GP8/RSET；底排为pin1 GP7/MASKROM、pin4 GND。
  pin4不是3.3 V，也不得出现在GPIO点击目标中。
- `gpio_layout_groups`必须把J8/J13都标为`edge=right`，order分别为0/1；对应
  safe-GPIO metadata为J8：GP29 `(row=4,col=0)`、GP5 `(4,1)`、GP16 `(5,0)`、
  GP6 `(5,1)`、GP15 `(6,0)`、GP14 `(7,0)`、GP13 `(8,0)`、GP12 `(9,0)`；
  J13：GP9 `(0,0)`、GP8 `(0,1)`、
  GP7 `(1,0)`。这些row/column不得因right edge而镜像。

### 8. watchdog 状态

```sh
timeout 5s radxa-linkr-debuggerctl --json watchdog status
```

观察 GPIO25 心跳 LED：在 `/api/v1/watchdog` 返回 healthy 状态后，
目视检查蓝色状态 LED 应在大约 1 秒周期内亮灭交替。视觉观察时间窗口
bound 到 5 秒以内（足够看到 2-3 次完整周期）。

### 8b. Logic Analyzer (Sigrok over WebSocket / TCP)

The logic analyzer uses RP2350 PIO2+DMA for high-speed GPIO capture. The sigrok
binary protocol runs over two transports: Web UI uses `/api/v1/live-sessions` then
`/api/v1/ws/<slot>` binary Sigrok; native sigrok/PulseView uses raw-TCP port 5556.

Pin ownership is part of the HIL contract. FAST8/WIDE11 take over both J4
UART1 pins (TX GP10 and RX GP11) through PIO2; a SINGLE capture on GP10 takes
TX only. Before changing mux ownership, the serial runtime freezes admission,
waits a bounded interval for any active TX to reach physical wire-complete,
cancels queued events that cannot start, advances sink epochs, and enters
PAUSED. While capture owns a pin, CDC ACM4, `serial.j4`, and raw TCP 5555 remain
transport sessions but cannot use that UART path; WS writes receive an explicit
paused error and TCP input is backpressured. After STOP or terminal cleanup the
firmware restores `GPIO_FUNC_UART_AUX` on GP10/GP11, resumes fresh sink epochs,
and J4 works again without reboot. Verify CDC, WS, and raw TCP before and after
capture; do not treat expected mid-capture silence as a bridge failure. The
active worst-case is a 480-byte 8E2 write at 4800 baud. The pause wait is capped
at 1250ms; current board evidence observed physical completion at 1.213s before
capture, while timeout/cancel branches remain host-model covered.

On x1.2, GP20 is the PIO I2C SCL and GP21 is SDA. The LA samples GP20 as a
read-only tap without reconfiguring the pin. Never drive GP20 or hold it low
from the fixture; that contends with the PIO0 I2C master. Verify the tap by
observing SCL activity from the board's own I2C traffic. An idle-high assertion
is valid only with the documented external pull-up and cross-domain electrical
prerequisites from section 4; there is no board-mounted pull-up and an unknown
high source is not a pass. GP19 is the CH224A PD strap and must also remain
high-impedance.

CONFIG v1 pre/post are uint16 for ordinary bounded and stream requests. Web
bounded pre-trigger supports rising, falling, and either only: `pre_samples >= 1`,
`post_samples >= 1`, and `pre_samples + post_samples <= 512`. Requested rates are
1-25 MHz, and the selected physical plan must retain at least
`2 * ceil(actual_rate / 1000)` samples. SINGLE supports through 25 MHz, FAST8
through 10 MHz and rejects 25 MHz, and WIDE11 through 5 MHz and rejects 10 MHz
and 25 MHz. Before first connection the Web UI permits local editing when generic
constraints pass, then uses real per-mode CAPS and rejects or disables old firmware
or a mode without CAPS mode flag bit 5 (`PRE_TRIGGER`, `1 << 5`). HELLO server
flags bit 0 CONFIG_V2 and bit 1 GENERIC_PACKED_BURST are separate. Stream,
trigger NONE, unsupported or high-rate generic packed burst, and ordinary deep
capture remain pre=0. Completion is pre plus post, and `triggerIndex` equals pre.

Firmware reuses the prepared common packed ring/sink lifecycle, treats packed
samples as the sole trigger authority after prefill, scans edges in software, and
freezes and drains `[T-pre,T+post)` without a new IRQ pairing or buffer. The
existing deep post behavior remains when pre=0. See the [2026-07-28 HIL report](results/2026-07-28-logic-analyzer-pre-trigger-uart-hil.md).

Bounded pre=0 and post=1..512 remains the exact finite PIO+DMA path for trigger
NONE and paths that cannot negotiate bounded pre-trigger. Web rising, falling,
and either pre-trigger uses the prepared packed ring/sink lifecycle, software edge
scan, and exact `[T-pre,T+post)` freeze and drain. Larger bounded requests
(post>512) use packed ring streaming. At negotiated high rates with
GENERIC_PACKED_BURST, post=0 uses packed burst, exactly 100000 samples followed
by auto-STOP/drain. At lower non-packed rates post=0 runs until user stops.

State progression after START_RESP: START_RESP with state 2 (ARMED) or state 3
(RUNNING for NONE), then EVENT armed (rising/falling/either only), then EVENT
triggered, then DATA frames, then EVENT stopped. NONE trigger emits no ARMED EVENT
and starts directly in RUNNING state.

Reproduce the current bounded pre-trigger and UART HIL with temporary Nix dependencies:

```sh
nix-shell -p python3Packages.websocket-client python3Packages.pyserial --run 'python3 apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py --matrix ws-bounded --modes SINGLE --tcp-rates-khz 1000 --tcp-pre-samples 64 --tcp-post-samples 448 --trigger-types rising --trigger-channel 0 --uart-stimulus Press --uart-device /dev/ttyACM1 --uart-baud 115200 --timeout 5'
```

#### 8b.1 Web UI Sigrok Session Lifecycle

Create live session and verify sigrok binary protocol over WebSocket:

```sh
# Create live session
timeout 5s curl -fsS -X POST http://172.29.203.1/api/v1/live-sessions
# Response: { "ws_url": "/api/v1/ws/0", ... }
```

Verify WebSocket binary frame exchange with sigrok protocol (HELLO/CAPS/CONFIG/START/STOP).
WS sigrok and raw-TCP 5556 are mutually exclusive; one session at a time across both.

#### 8b.2 Bounded Capture (post_samples=65535)

Verify bounded captures with exactly 65535 samples at 100 kHz for SINGLE, FAST8,
WIDE11 modes. Each should receive exactly 65535 samples with 0 gaps, restart true,
HTTP health true.

#### 8b.3 Stream Mode (post_samples=0)

Verify stream mode behavior. With `GENERIC_PACKED_BURST`, post=0 at the
high rates documented in section 8b.4.1 (100 MHz or 125 MHz SINGLE/FAST8,
100 MHz WIDE11) captures exactly 100000 samples losslessly then auto-STOPs
and drains; WIDE11 at 125 MHz is rejected by START (INVALID_CONFIG).
At lower non-packed rates, post=0 runs until the client stops. Protocol-v2
SINGLE passes the strict five-second contract at 16 MHz over both WS and TCP for
the measured static/low-transition input. Each 16,384-sample chunk uses a
2,048-byte chronological bitstream and compresses to three payload bytes for the
observed repeated value. This does not prove high-transition or incompressible
16 MHz capacity; see section 8b.4.

#### 8b.4 Continuous Operating Points and Sustained Stability

Sustained-duration validation is governed by a strict semantics contract. A
continuous run is `pass=true` only when it meets **all** of the following:

- At least 95% of the requested duration is reached
- At least 95% of negotiated samplerate × requested duration samples are received
- `stop_response.received: true`
- Zero sample-index gaps
- Zero disconnects
- Zero protocol-level decode errors
- Zero overruns
- Immediate restart capability confirmed
- HTTP health after stop

An early server STOPPED/OVERRUN that arrives before the requested duration
is a clean lossless terminal, but it is `pass=false` for sustained-duration
purposes and is recorded as a capacity diagnostic, not a sustained PASS.

Validated operating points and boundary diagnostics under the strict contract:

| Transport | Mode | Rate | Expected |
|-----------|------|------|----------|
| WebSocket | SINGLE continuous | 16 MHz | 10/10 strict 5-second PASS; 78,004,224–79,855,616 samples, 15.600–15.969 Msamples/s, zero gaps/errors/overruns, STOP, restart, HTTP health |
| TCP | SINGLE continuous | 16 MHz | 10/10 strict 5-second PASS; 77,266,944–79,937,536 samples, 15.451–15.986 Msamples/s, zero gaps/errors/overruns, STOP, restart, HTTP health |
| WebSocket | SINGLE continuous | 18.002 MHz actual | Boundary failure: 83,542,016 samples below the 85,509,500 strict minimum; clean STOP/restart/health, zero gaps/overruns |
| TCP | SINGLE continuous | 18.002 MHz actual | Boundary failure: 78,413,824 samples below the 85,509,500 strict minimum; clean STOP/restart/health, zero gaps/overruns |
| WebSocket | FAST8 static/low-transition | 2.600 MHz | 10/10 strict PASS; adjacent 2.800 MHz fails |
| TCP | FAST8 static/low-transition | 2.600 MHz | 10/10 strict PASS; 2.700 MHz passes only 7/10 |
| WebSocket | WIDE11 static/low-transition | 1.050 MHz | 10/10 strict PASS; adjacent 1.100 MHz fails |
| TCP | WIDE11 static/low-transition | 1.175 MHz | 10/10 strict PASS; 1.200 MHz passes only 8/10 |
| WebSocket | FAST8 GP16-active PALETTE2 | 1.200 MHz | 10/10 strict PASS; adjacent 1.225 MHz fails |
| TCP | FAST8 GP16-active PALETTE2 | 1.375 MHz | 10/10 strict PASS; adjacent 1.400 MHz fails |
| WebSocket | WIDE11 GP16-active PALETTE2 | 850 kHz | 10/10 strict PASS; adjacent 875 kHz fails |
| TCP | WIDE11 GP16-active PALETTE2 | 950 kHz | 10/10 strict PASS; adjacent 975 kHz fails |

Both current 16 MHz rows require 95% duration and 95% of the negotiated sample
count, in addition to zero gaps, disconnects, protocol errors, and overruns plus
STOP_RESP, immediate restart, and HTTP health. Duration alone is no longer a
throughput PASS. The adjacent 18.002 MHz rows retain clean transport semantics
but fail because their sample totals miss the negotiated-rate threshold.

The SINGLE input was static or low-transition. SINGLE_BITS_RLE reduced each
16,384-sample chunk to three payload bytes, so high-transition/incompressible 16 MHz SINGLE
remains unverified. Multichannel active-input validation uses a separate GP16 profile:

- Connect /dev/ttyACM1 TX to GP16 and share ground; confirm no user process owns ttyACM1.
- Continuously transmit UART 0x55, 8N1, at 921600 baud for the complete matrix.
- Require activity_analyzed_samples=65536, bit6 present in sample_value_or and absent from
  sample_value_and, and non-zero sample_value_transitions for every PASS row.
- PACKED_PALETTE2 rows must report compression_frame_counts key 6. Raw pre-palette baseline rows
  use UART baud equal to sample rate and require key 1 plus near-per-sample transitions.
- This proves one transition-rich GP16 line in the full FAST8/WIDE11 channel set. It does not
  prove simultaneous independent entropy on all channels or cross-lane high-state mapping.

See docs/testing/results/2026-08-30-logic-analyzer-v2-dense-hil.md and
docs/testing/results/2026-08-31-logic-analyzer-v2-multichannel-hil.md.

Bounded pre=0 and post=1..512 HIL results (exact finite engine). post=513 to 65535 uses packed ring streaming.
post=65536 is not a valid uint16; use CONFIG_V2 with u32LE pre/post for >65535 captures.

| Transport | Mode | Trigger | Rate | post | Result |
|----------|------|---------|------|------|--------|
| WS | SINGLE | NONE | actual 125.081 MHz | 1 | Exactly 1 sample, 0 gaps, restart true, HTTP health true |
| TCP | SINGLE | NONE | actual 125.081 MHz | 1 | Exactly 1 sample, 0 gaps, restart true, HTTP health true |
| WS | SINGLE | rising | 5 MHz | 512 | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS | SINGLE | falling | 5 MHz | 512 | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS | SINGLE | either | 5 MHz | 512 | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS | SINGLE | rising | 25 MHz | 512 | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS | SINGLE | falling | 25 MHz | 512 | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS | SINGLE | rising | 50 MHz | 512 | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS | SINGLE | falling | 50 MHz | 512 | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS | SINGLE | rising | 100 MHz | 512 | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS | SINGLE | falling | 100 MHz | 512 | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| WS | SINGLE | either | 100 MHz | 512 | Exactly 512 samples, 0 gaps, restart true, HTTP health true |
| TCP | SINGLE | rising | 100 MHz | 512 | Exactly 512 samples, 0 gaps, restart true, HTTP health true |

Bounded post=65535 results (packed ring streaming, user-stop):

| Transport | Mode | Trigger | Rate | post | Result |
|----------|------|---------|------|------|--------|
| WS | SINGLE | NONE | 100 kHz | 65535 | Exactly 65535 samples, 0 gaps, restart true, HTTP health true |
| WS | FAST8 | NONE | 100 kHz | 65535 | Exactly 65535 samples, 0 gaps, restart true, HTTP health true |
| WS | WIDE11 | NONE | 100 kHz | 65535 | Exactly 65535 samples, 0 gaps, restart true, HTTP health true |
| TCP | SINGLE | NONE | 100 kHz | 65535 | Exactly 65535 samples, 0 gaps, restart true, HTTP health true |
| TCP | FAST8 | NONE | 100 kHz | 65535 | Exactly 65535 samples, 0 gaps, restart true, HTTP health true |
| TCP | WIDE11 | NONE | 100 kHz | 65535 | Exactly 65535 samples, 0 gaps, restart true, HTTP health true |

Current WIDE11 bounded post=100000 results (CONFIG_V2, dual-SM packed arena,
auto-STOP on completion):

| Transport | Mode | Trigger | Rate | post | Result |
|----------|------|---------|------|------|--------|
| WS | WIDE11 | NONE | 100 MHz | 100000 | Exactly 100000 samples, 0 gaps, zero disconnects, fresh restart after every run |
| WS | WIDE11 | rising | 100 MHz | 100000 | Exactly 100000 samples, trigger index 0 |
| TCP | WIDE11 | rising | 100 MHz | 100000 | Exactly 100000 samples, trigger index 0 |
| WS | WIDE11 | NONE | 100 MHz | 100000 | Exactly 100000 samples, 0 gaps |
| TCP | WIDE11 | NONE | 100 MHz | 100000 | Exactly 100000 samples, 0 gaps |

post=0 at high rate (GENERIC_PACKED_BURST, auto-STOP after exactly 100000 samples):

| Transport | Mode | Trigger | Rate | post | Result |
|----------|------|---------|------|------|--------|
| WS | SINGLE | NONE | 100 MHz | 0 | Exactly 100000 samples, auto-STOP, 0 gaps |
| WS | SINGLE | NONE | 125 MHz | 0 | Exactly 100000 samples, auto-STOP, 0 gaps |
| WS | FAST8 | NONE | 100 MHz | 0 | Exactly 100000 samples, auto-STOP, 0 gaps |
| WS | FAST8 | NONE | 125 MHz | 0 | Exactly 100000 samples, auto-STOP, 0 gaps |
| WS | WIDE11 | NONE | 100 MHz | 0 | Exactly 100000 samples, auto-STOP, 0 gaps |

WIDE11 post=0 at 125 MHz is rejected by START (INVALID_CONFIG); not a pending HIL case.

Adjacent WS SINGLE failure was not measured under the final architecture. Use an
isolated runner invocation per trigger type and rate because a terminal program
holding `/dev/ttyACM1` or case reuse can invalidate the stimulus. Verify
`/dev/ttyACM1` is the WCH serial device and is not held by any terminal program
before starting logic analyzer validation (use `fuser` or similar to confirm).

Verify HTTP and watchdog remain responsive during capture.

#### 8b.4.1 Generic Packed Burst (CONFIG_V2, post=100000 and post=0)

HELLO server_flags bit 0 advertises CONFIG_V2 and bit 1 advertises GENERIC_PACKED_BURST.
Use frame0x0b (CONFIG_V2_REQ, 16B) with u32LE pre/post fields for large-depth captures.
The v1 frame0x05 (12B) remains for bounded captures with post <= 65535 and the post=0
stream sentinel. With CONFIG_V2 and GENERIC_PACKED_BURST, bounded
`post=65536..100000` is valid at every otherwise supported rate and pin plan.

**High-rate `post=0` capacity-burst matrix**:

| Mode | Rate | pre | post | Notes |
|------|------|-----|------|-------|
| SINGLE | 100 MHz or 125 MHz | 0 | 0 | Captures exactly 100000 samples losslessly then auto-STOP/drain |
| FAST8 | 100 MHz or 125 MHz | 0 | 0 | Captures exactly 100000 samples losslessly then auto-STOP/drain |
| WIDE11 | 100 MHz only | 0 | 0 | Captures exactly 100000 samples losslessly then auto-STOP/drain. 125 MHz rejected by START (INVALID_CONFIG) |

The 2026-07-27 WIDE11 HIL verified the then-current target at 100 MHz with
`pre=0` and both `post=100000` and high-rate `post=0`. Accepted deep bursts
delivered exactly 100000 samples in 98 DATA frames with zero sample-index gaps.
WIDE11 uses two capture SMs: SM-A (GP10-GP17, 8-bit autopush32, 100000 B) and SM-B
(GP18-GP20, 3-bit autopush30, 40000 B); two DMA channels; 144184 B shared burst slice (overlays the 161144 B total backing allocation).
GP29 is excluded from WIDE11 LA and remains input-only while used by ADC3. NONE deep burst uses
two capture SMs; triggered deep burst adds a third SM running the 3-instruction trigger
program. Two-phase START prepares ownership and quiesce before the response. NONE sends
START_RESP in RUNNING state with no ARMED event; triggered captures send START_RESP in
ARMED state followed by the ARMED event. GO then synchronously enables the sampler SM(s).

| Transport | Mode | Trigger | Rate | Expected |
|-----------|------|---------|------|----------|
| WS | WIDE11 bounded deep burst | NONE | 100 MHz | PASS — exactly 100000 samples, 98 DATA frames, 0 gaps, restart and HTTP health true |
| WS | WIDE11 bounded deep burst | rising | 100 MHz | PASS — exactly 100000 samples, trigger index 0, 0 gaps |
| TCP | WIDE11 bounded deep burst | rising | 100 MHz | PASS — exactly 100000 samples, trigger index 0, 0 gaps |
| WS | WIDE11 high-rate capacity burst | NONE | 100 MHz | PASS — post=0 produced exactly 100000 samples and auto-STOPPED |
| TCP | WIDE11 high-rate capacity burst | NONE | 100 MHz | PASS — post=0 produced exactly 100000 samples and auto-STOPPED |

After each deep burst run, verify HTTP health and restart capability. The 144184 B shared burst slice (overlaying the 161144 B total backing allocation)
restores ADC telemetry, power capture, and normal Sigrok pool after drain;
confirm these services are functional after the capture completes.

The historical authoritative TCP/WS matrix reported 54/54 cases and the
high-rate matrix passed 62/62. Eighteen historical continuous rows ended with
an explicit capacity OVERRUN; four had `capacity_stop_before_data=true`.
Those rows prove lossless-or-stop terminal behavior, not sustained operation
at the requested rate, and the current strict duration evaluator does not count
an early terminal as a sustained PASS. Historical WIDE12 predecessor evidence remains at
`docs/testing/results/2026-07-26-logic-analyzer-wide12-100k-hil.md`.

#### 8b.4.2 WIDE11 Deep-Burst Pin Mapping HIL

The executable pin-mapping HIL is `--matrix wide11-mapping` in
`apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py`. It must capture the
exact WIDE11 deep-burst mode (`100 MHz`, `pre=0`, `post=100000`, CONFIG_V2), decode
DATA payload as little-endian 11-bit samples (GP10=bit0 through GP20=bit10), and
clearly identify which stimulus profile was used.

On x1.2, GP20 is the PIO I2C **SCL**, not a free GPIO. It is sampled as a
read-only tap: the LA leaves the PIO0 function and pad input enable untouched, so
PIO0 keeps driving the INA226/CH224A bus while PIO2 samples the pad. GP19 is the
CH224A PD strap and must remain high-impedance. The only fixture-driven pins are
therefore GP10, GP11, and GP18:

| Physical pin | DATA bit | Fixture |
|--------------|----------|---------|
| GP10 | bit0 | External generator D0 (also trigger channel 0) |
| GP11 | bit1 | External generator D1 |
| GP18 | bit8 | External generator D2 |
| GP19 | bit9 | No connection; CH224A PD strap, read-only |
| GP20 | bit10 | No connection; PIO I2C SCL, read-only tap |

GP29 / ADC3 is excluded from WIDE11 LA and remains input-only.

Do not use HTTP safe-GPIO outputs as the stimulus source for this case. WIDE11 PIO
preparation takes over the fixture-driven pins as PIO2 inputs, while GP19 stays
high-impedance and GP20 retains its PIO0 I2C configuration. Any same-pin HTTP
safe-GPIO output would be overridden or conflict with the capture ownership model.

Full external-generator prerequisite for a valid mapping pass:

- External 3.3 V-compatible pattern generator D0 -> at least 1 kΩ series
  resistor -> GP10 (DATA bit0 and trigger channel 0)
- External generator D1 -> at least 1 kΩ series resistor -> GP11 (DATA bit1)
- External generator D2 -> at least 1 kΩ series resistor -> GP18 (DATA bit8)
- External generator GND -> Linkr Debugger GND
- Leave GP19 and GP20 unconnected. Do not drive them, pull them low, or attach a
  probe that loads the I2C bus. A run that drives GP20 is invalid and must not be
  reported as a pass.
- Keep all generator outputs high-impedance until the START response confirms capture
  ownership; then emit the declared repeating pattern. Return every output to
  high-impedance before STOP or automatic cleanup can restore UART1 TX/RX on GP10/GP11.
  The series resistors are mandatory contention protection if handoff timing is
  imperfect. Pattern bit0 drives GP10, bit1 drives GP11, bit2
  drives GP18; pattern values must be `0..0x7` (no fourth driven bit). The default
  sequence is `0x1,0x3,0x2,0x6,0x4,0x5,0x7,0x0`, holding each value for
  64 samples at 100 MHz, and is intentionally transition-rich enough to reject a
  one-sample SM-A/SM-B skew.

Run the full external-generator case with a 5-second operation bound only after wiring
and generator setup are in place:

```sh
timeout 5s python3 apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py \
  --matrix wide11-mapping \
  --wide11-map-external-generator \
  --timeout 5
```

The case is blocked, not passed, unless `--wide11-map-external-generator` is provided.
Passing criteria include CONFIG_V2 use, exact mode/rate/pre/post, no DATA decode
errors, no sample-index gaps, at least 100000 received samples, successful STOP_RESP
cleanup, HTTP health after cleanup, and exact expected-pattern matching for bits
0/1/8 (`pattern_mask=0x0103`) across the configured check window. Other lanes
must not be compared against invented fixed levels.

GP20 is accepted only as a read-only SCL tap. While the capture runs, generate
board-owned I2C traffic (for example repeated `GET /api/v1/adc/read` requests or
normal INA226 telemetry) and verify that the GP20 lane shows SCL transitions and
returns to the externally pulled idle-high level established by section 4's
documented fixture. The LA must not change the GP20/GP21 pin mux or direction
and must not disturb I2C transactions. Without that external pull-up, or if the
capture window contains no I2C traffic, record GP20 as blocked/not exercised
rather than passing it. Never substitute the RP2350 internal pull-up.
GP19 must read at its strap level (low through the board's 120 kOhm Rset) and
must never be driven by the fixture.

The runner reports passive GP19/GP20 level/transition observations only. It cannot
correlate GP20 transitions with board-owned I2C transactions or inspect the pin mux,
and therefore always leaves `dynamic_scl_mapping_validated` and
`read_only_no_contention_verified` false. A three-driven-bit mapping pass alone is
not full WIDE11 acceptance: separately record the read-only/no-contention checks
above, or mark them `[blocked]` / not exercised. Static high or low SCL is not
evidence of a dynamic SCL mapping pass.

Reduced single-wire setup for remote validation is explicitly separate and must not be
reported as the full external mapping test. Use it only when `/dev/ttyACM1` TX is
wired through at least a 1 kΩ series resistor to GP10 and all other selected pins are
left unconnected:

```sh
timeout 5s python3 apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py \
  --matrix wide11-mapping \
  --wide11-map-gp10-uart-low-others \
  --timeout 5
```

The reduced case sends the UART stimulus only after START_RESP, which is the
trigger-safe barrier. It verifies GP10 DATA bit0 sees both low and high runs. It
does **not** validate independent high-state mapping for GP11, GP18, GP19, or
GP20, and it must not assert that GP20 stays low: GP20 is SCL and its level is
owned by the I2C bus. Report unconnected lanes as unverified, not as zero-mask
passes (`zero_mask=0x0000`; the retained CLI flag name does not require low levels).
GP29 is excluded from WIDE11 LA and remains input-only while used by ADC3.

The older 2026-07-27 reduced profile that held GP11-GP20 low is x1.1 historical
evidence. It is not an x1.2 acceptance procedure, and its fixed-low zero masks must
not be reused on x1.2. Likewise, an `external_generator` run that still drives
D3 into GP20 is not acceptable x1.2 evidence; the runner must implement the
three driven bits plus the read-only GP20 tap described above.

#### 8b.4.3 WIDE11 Shared-Arena Telemetry Isolation HIL

The executable two-client regression is `--matrix wide11-telemetry-isolation` in
`apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py`. It validates that a
normal JSON WebSocket ADC telemetry client stays connected but pauses while a second
client holds the WIDE11 shared burst-slice lease through raw-TCP sigrok deep burst.
WIDE11 uses two packed capture SMs and two DMA channels in the 144184-byte
shared burst slice (overlays the 161144 B total backing allocation); triggered capture adds a third trigger-only SM.

Run with 5-second bounded operations:

```sh
timeout 5s python3 apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py \
  --matrix wide11-telemetry-isolation \
  --timeout 5
```

Required behavior:

- Client A creates `/api/v1/live-sessions`, connects to the returned JSON WebSocket,
  subscribes to ADC telemetry (`type=subscribe`, `topic=live`), and records baseline
  telemetry sequence/timestamps before the capture starts.
- Client B opens raw-TCP sigrok port 5556 and performs the exact WIDE11 deep burst:
  WIDE11, 100 MHz, `pre=0`, `post=100000`, CONFIG_V2.
- START_RESP from client B is the evidence that the capture backend has entered the
  trigger-safe prepared/running path and the shared burst-slice pause/lease interval has
  started. The runner records this monotonic timestamp.
- During the overlap lease/pause interval, client A's WebSocket must remain connected
  and must not receive malformed JSON or binary sigrok contamination. A receive
  timeout in this expected pause is no-data while still connected, not a disconnect
  or runner error. A short grace window may record telemetry already queued before
  quiesce; it must be reported as `pre_pause_delivery_grace_records`, not hidden.
  After that grace window, no baseline-epoch telemetry sample from the overlapped
  ADC ring may be emitted before an observable sequence-epoch reset/resume. The
  runner reports such contamination as `old_epoch_pause_records` /
  `old_epoch_pause_record_count`.
- A valid sequence regression/reset with advancing `device_t_mono_us` or `uptime_us`
  marks observable firmware release/resume and begins the post-release epoch, even
  when the raw-TCP helper has not yet returned its final STOP response. For example,
  if baseline ended at sequence 12, grace drains sequence 13..25, and the client then
  observes sequence 1..9 with device time greater than the baseline/old epoch, those
  reset-epoch records are retained as post-release evidence. Do not classify arbitrary
  sequence gaps as reset without device-time advance.
- Client B must receive exactly the WIDE11 deep-burst contract: at least 100000 samples,
  98 DATA frames, zero sample-index gaps, no DATA decode/mask/budget errors, and
  STOP_RESP. The raw-TCP helper return timestamp is an upper bound on release, not the
  lease boundary when telemetry already exposed a valid reset epoch earlier.
- After drain/release, fresh ADC telemetry must continue on client A without reconnecting.
  Resume may reconstruct the ADC ring and start a new telemetry sequence epoch, so the
  runner must not require the post-release numeric sequence to exceed the baseline
  sequence. It must require post-release records with a valid sequence and advancing
  `device_t_mono_us` or `uptime_us`, and it must explicitly report the inferred
  release/resume timestamp plus whether the post-release sequence continued or reset.
- `GET /api/v1/adc/read` must pass after release.

Use `pause`/`resume` terminology for this test. Do not describe the expected pause as
a disconnect or reconnect, and do not treat `WebSocketTimeoutException` during the
expected pause as a failure when the socket remains open. A same-epoch telemetry
message emitted during the post-grace overlap window before reset/resume is a failure;
a genuine WebSocket close/error, malformed JSON, binary contamination, raw TCP WIDE11
short read, missing 98 DATA frames, invalid post-release epoch/timing, or failed ADC
HTTP health is also a failure.

#### 8b.5 GP10 UART Trigger Validation

GP10 trigger validation requires an isolated runner invocation per trigger type
and rate. A terminal program holding `/dev/ttyACM1` or case reuse can invalidate
the stimulus. Confirm `/dev/ttyACM1` is the WCH serial device and is not held
by any terminal program before starting.

Prerequisites:
- `/dev/ttyACM1` is the external WCH CH347 stimulus-fixture serial port; verify
  with `fuser` or `ls -la /dev/ttyACM*`
- `/dev/ttyACM2` is the firmware CDC ACM port
- No terminal program is attached to `/dev/ttyACM1`
- `/dev/ttyACM1` TX reaches GP10 through at least a 1 kΩ series resistor. Direct
  output-to-output wiring is forbidden because firmware restores UART1 TX immediately
  after capture, possibly before a short bounded UART stimulus has finished.

The stimulus UART and the J4 UART are different paths. `/dev/ttyACM1` drives
GP10 externally as the test fixture, while J4 is the firmware UART1 bridge on
GP10/GP11. During FAST8/WIDE11 (and SINGLE on GP10) the LA owns those pins, so
J4 must be silent; after cleanup the firmware restores `GPIO_FUNC_UART_AUX` and
J4 works again without a reboot. Do not use J4 as the stimulus source for this
case and do not leave a terminal attached to J4 during the capture.

UART stimulus may be sent immediately after START_RESP is received and must trigger:
because START_RESP is the trigger-safe barrier — it is emitted only after the firmware has
acquired capture ownership and the PIO/DMA backend is successfully armed and running.
A host that receives START_RESP may immediately transmit trigger stimulus on the UART;
no false ARMED or RUNNING EVENT precedes this acknowledgment. A failed start returns
FRAME_ERROR synchronously and emits no ARMED or RUNNING EVENT.

Use the isolated WS HIL runner with UART stimulus injection. Repeat once per rate
and per trigger type; do not combine multiple rates or trigger types in one run.

1 MHz rising edge — isolated invocation:

```sh
timeout 5s nix-shell --run \
  'python3 apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py \
    --matrix ws-bounded \
    --modes SINGLE \
    --tcp-rates-khz 1000 \
    --tcp-pre-samples 0 \
    --tcp-post-samples 512 \
    --trigger-types rising \
    --trigger-channel 0 \
    --uart-stimulus UUUUUUUU \
    --uart-device /dev/ttyACM1 \
    --uart-baud 115200 \
    --timeout 5'
```

1 MHz falling edge — isolated invocation:

```sh
timeout 5s nix-shell --run \
  'python3 apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py \
    --matrix ws-bounded \
    --modes SINGLE \
    --tcp-rates-khz 1000 \
    --tcp-pre-samples 0 \
    --tcp-post-samples 512 \
    --trigger-types falling \
    --trigger-channel 0 \
    --uart-stimulus UUUUUUUU \
    --uart-device /dev/ttyACM1 \
    --uart-baud 115200 \
    --timeout 5'
```

1 MHz either edge — isolated invocation:

```sh
timeout 5s nix-shell --run \
  'python3 apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py \
    --matrix ws-bounded \
    --modes SINGLE \
    --tcp-rates-khz 1000 \
    --tcp-pre-samples 0 \
    --tcp-post-samples 512 \
    --trigger-types either \
    --trigger-channel 0 \
    --uart-stimulus UUUUUUUU \
    --uart-device /dev/ttyACM1 \
    --uart-baud 115200 \
    --timeout 5'
```

2 MHz rising edge — isolated invocation:

```sh
timeout 5s nix-shell --run \
  'python3 apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py \
    --matrix ws-bounded \
    --modes SINGLE \
    --tcp-rates-khz 2000 \
    --tcp-pre-samples 0 \
    --tcp-post-samples 512 \
    --trigger-types rising \
    --trigger-channel 0 \
    --uart-stimulus UUUUUUUU \
    --uart-device /dev/ttyACM1 \
    --uart-baud 115200 \
    --timeout 5'
```

2 MHz falling edge — isolated invocation:

```sh
timeout 5s nix-shell --run \
  'python3 apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py \
    --matrix ws-bounded \
    --modes SINGLE \
    --tcp-rates-khz 2000 \
    --tcp-pre-samples 0 \
    --tcp-post-samples 512 \
    --trigger-types falling \
    --trigger-channel 0 \
    --uart-stimulus UUUUUUUU \
    --uart-device /dev/ttyACM1 \
    --uart-baud 115200 \
    --timeout 5'
```

2 MHz either edge — isolated invocation:

```sh
timeout 5s nix-shell --run \
  'python3 apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py \
    --matrix ws-bounded \
    --modes SINGLE \
    --tcp-rates-khz 2000 \
    --tcp-pre-samples 0 \
    --tcp-post-samples 512 \
    --trigger-types either \
    --trigger-channel 0 \
    --uart-stimulus UUUUUUUU \
    --uart-device /dev/ttyACM1 \
    --uart-baud 115200 \
    --timeout 5'
```

For each rate and trigger-type combination:
1. Verify JSON `overall_pass: true`
2. Verify exactly 512 samples captured
3. Verify zero sample-index gaps
4. Verify zero disconnects
5. Verify `stop_response.received: true`
6. Verify trigger offset is valid (trigger index present and reasonable, offset 0 for idle-biased stimulus)
7. Verify immediate restart capability with no artificial delay: after a 100 MHz
   bounded `post=513` capture and STOP/close, a fresh-session `post=1` START must
   succeed without Sigrok ERROR code 5/detail 116
8. Verify HTTP health after stop

Verify trigger offset validation: with the UART idle-biased stimulus at the
trigger channel, the reported `triggerIndex` must be within the expected range and
`triggerOffset` must be 0. Re-arm and re-trigger to confirm offset remains
consistent.

CDC BOOTSEL tests must discover the control-shell interface from the current
USB topology instead of assuming a tty number. The 2026-10-21 topology exposed
control on ACM3 and J4 on ACM4; no ACM2 interface existed. This paragraph is
separate from `/dev/ttyACM1` references in 8b.5, where ACM1 is an external WCH
logic-analyzer stimulus fixture rather than a board control or J4 endpoint.

#### 8b.6 Owner/Mutual-Exclusion

Verify only one sigrok session at a time across WebSocket and raw-TCP 5556.
When one transport holds the session, the other should return BUSY error.
After session closes, new connections should be immediately available.

#### 8b.7 HTTP/WS Health During Capture

During active capture, verify HTTP API endpoints remain responsive:
- `GET /api/v1/status`
- `GET /api/v1/watchdog`

Both should return normal JSON, not timeouts or errors.

#### 8b.8 Native Sigrok (TCP 5556)

Verify sigrok-cli connects via raw-TCP port 5556:

```sh
timeout 5s sigrok-cli -d linkr-debugger:conn=tcp-raw/172.29.203.1/5556 --scan
```

Verify bounded capture with exactly 65535 samples at 100 kHz, 0 gaps, restart
healthy, HTTP healthy.

#### 8b.9 Browser WASM Decoder

Verify WASM decoder resources load correctly:

```sh
timeout 5s curl -fsS --compressed -D /tmp/decoder-js.headers \
  -o /tmp/logic-decoder.js \
  http://172.29.203.1/assets/decoder/logic-decoder.js
timeout 5s curl -fsS --compressed -D /tmp/decoder-wasm.headers \
  -o /tmp/logic-decoder.wasm \
  http://172.29.203.1/assets/decoder/logic-decoder_bg.wasm

python3 - <<'PY'
from pathlib import Path

js_headers = Path("/tmp/decoder-js.headers").read_text().lower()
wasm_headers = Path("/tmp/decoder-wasm.headers").read_text().lower()
assert " 200 " in js_headers.splitlines()[0]
assert "content-type: text/javascript" in js_headers
assert "content-encoding: gzip" in js_headers
assert " 200 " in wasm_headers.splitlines()[0]
assert "content-type: application/wasm" in wasm_headers
assert "content-encoding: gzip" in wasm_headers
assert Path("/tmp/logic-decoder.wasm").read_bytes()[:4] == b"\x00asm"
assert b"logic-decoder_bg.wasm" in Path("/tmp/logic-decoder.js").read_bytes()
print("Decoder JS/WASM headers, gzip decoding, glue reference, and WASM magic are valid")
PY
```

### 8c. Reboot 与物理 BOOTSEL 按钮

`reboot` 是正常冷重启，`bootloader` 是 ROM BOOTSEL。二者不是别名，任何
reboot 通过都不能替代第 9、10 节的 ROM BOOTSEL 证据，反之亦然。

#### 每次启动后的电源前置状态

冷启动后 `5v_out`、`12v_out`、`20v_out`、`vdd_5v`、`5v_usb_hub` 全部回到
各自 Device Tree 的未使能启动状态。如果存在持久化
v1 配置快照，启动重放可能在 control transport ready 前再次打开其中保存的
`5v_out`、`vdd_5v` 或 `5v_usb_hub`。必须记录这个 replay 结果，但不能把重放后的
`on` 当作 Device Tree boot-off 证据，也不能因此跳过 host 前置动作。只有专门验证
无 snapshot boot default 的用例才要求先清除 snapshot；普通功能 HIL 可保留并明确
记录 replay。

每次上电或启动（包括初次烧录后的启动）、reboot、ROM BOOTSEL 返回或
watchdog 复位后，HIL 都必须先重新建立测试前置电源并验证，再继续任何依赖
目标供电的步骤。transport ready 后，下面第一个 PUT 必须是本轮**第一条 host
hardware mutation**，第二个 PUT 必须是本轮第二条；二者之间不得插入 route、GPIO、
其它 rail 或配置写操作：

```sh
BOARD_URL=http://172.29.203.1
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"state":"on"}' "$BOARD_URL/api/v1/power/5v_out"
curl -fsS -X PUT -H 'Content-Type: application/json' \
  --data '{"mode":0,"voltage_mv":20000}' "$BOARD_URL/api/v1/pd"
curl -fsS "$BOARD_URL/api/v1/power/5v_out"
curl -fsS "$BOARD_URL/api/v1/pd"
```

要求 `5v_out` 为 `on`，PD 返回 `available=true`、`mode=0`（fixed）且
`voltage_mv=20000`。PD 源必须实际广告 20 V；源不提供 20 V 时该步骤记为
`[blocked]`，不得用 5 V 结果冒充通过。每次 reboot 测试迭代和每次 BOOTSEL
恢复后都重复这条前置检查。如果后续步骤还需要 `12v_out` 或 `20v_out`，也必须
重新设置并逐项验证；不得复用复位前的电源状态。

即使启动 replay 已显示 `5v_out=on`，第一条 host mutation 仍发送/确认
`5v_out=on`，第二条仍请求 PD 20 V；随后必须读取 INA226，确认 enabled 5 V rail
具有有效电压和电流数据。报告同时记录 replay 状态与 host mutation 顺序。

#### 四个客户端的正常 reboot

在同一固件上分别验证以下四个协议客户端（CDC ACM、HTTP、WebSocket、CLI）：

| 客户端 | 操作 |
| --- | --- |
| CDC ACM shell | `reboot` |
| HTTP | `curl -fsS -X POST "$BOARD_URL/api/v1/reboot"` |
| WebSocket | `{"type":"command","command":"reboot","id":"reboot-ws"}` |
| CLI | `radxa-linkr-debuggerctl reboot` |

WebSocket 使用 `/api/v1/live-sessions` 返回的 slot；预期先收到
`{"type":"result","schema":"radxa-linkr-debugger.v1","command":"reboot","id":"reboot-ws","ok":true,"status":"ok"}`，
再等待 250 ms 重启。每次迭代都要检查：

- 请求在复位前返回成功；HTTP 为 2xx，CLI/WS 为 `ok:true`。
- 250 ms 后 USB NCM 断开并重新枚举，板子在既有 bounded retry 内恢复 HTTP。
- 10 秒内 `lsblk` 不出现 VENDOR 严格等于 `RPI` 的设备，即没有进入
  `RPI-RP2`，也没有 bootloader 枚举。
- 恢复后先执行上一节的 `5v_out=on` + PD 20 V 前置恢复与验证。
- `GET /api/v1/watchdog` 返回 `supported=true`、`automatic=true`、
  `healthy=true`、`armed=true`、`bootloader_on_timeout=true`，且
  `failing_service` 为空。

TUI 与 Web UI 复用同一个 HTTP 端点，不另算第五个传输：TUI 的
**System / Reboot** 行和 Web 的 **Restart** 各执行一次，二者都必须只触发正常
重启，且不出现 `RPI-RP2`。TUI 不提供 BOOTSEL 行；另需确认 CLI `bootloader`
与 Web 的 **Enter BOOTSEL** 仍走独立 bootloader 动作。正常重启测试不得从这两个
入口发起，它们的 ROM BOOTSEL 路径按第 9 节验证。

#### 非 POST 请求

`/api/v1/reboot` 受支持的变更方法是 POST。依次发送 GET、PUT、DELETE：

```sh
for method in GET PUT DELETE; do
  code=$(timeout 5s curl -sS -o "/tmp/reboot-$method.json" -w '%{http_code}' \
    -X "$method" "$BOARD_URL/api/v1/reboot")
  [ "$code" = 405 ] || { echo "$method expected 405, got $code"; exit 1; }
  grep -q 'method_not_allowed' "/tmp/reboot-$method.json" || {
    echo "$method missing method_not_allowed"; exit 1;
  }
done
```

每个响应必须是 HTTP 405 且 `error.code=method_not_allowed`。等待至少 1 秒后
`GET /api/v1/status` 仍应成功，`board_monitoring.uptime_ms` 继续增长，证明
这些请求没有调度复位。

#### 重复 pending

在同一个 250 ms 窗口内连续发送两次 `POST /api/v1/reboot`。第一次成功响应后，
第二次应返回 HTTP 409、`error.code=reboot_pending`，不得再调度第二次或更早的复位。只允许观察到一次
USB 断开/重新枚举；恢复后仍不得出现 `RPI-RP2`。若第二次恰好在设备已复位后
发出，其 transport error 只能记为时序边界，不能算作幂等通过，必须重跑本项。

#### 无 RPI-RP2

正常 reboot 测试期间不得按住物理按键。持续 10 秒轮询，任何时刻出现 `RPI`
vendor 磁盘都判失败；若按住按键跨越复位，ROM 进入 BOOTSEL 属于物理按键
用例的预期，不能记到本项。

```sh
for _ in $(seq 1 10); do
  RPI_DISK=$(timeout 5s lsblk -dpno NAME,VENDOR | awk '$2 == "RPI" { print $1; exit }')
  [ -z "$RPI_DISK" ] || { echo "unexpected ROM BOOTSEL device: $RPI_DISK"; exit 1; }
  sleep 1
done
```

#### 物理 BOOTSEL 按钮

物理按键测试需要人工计时。每一项开始前记录 `uptime_ms`，结束后再次读取，
以区分“没有复位”和“已经重启”：

| 手势 | 预期 |
| --- | --- |
| 短按后立即释放（小于 3 秒） | 不动作；`uptime_ms` 连续增长，USB/HTTP 不中断，不出现 `RPI-RP2` |
| 运行中按住 3 秒，再继续按住至少 2 秒 | 按住期间不复位；`uptime_ms` 继续增长，HTTP 仍可响应 |
| 上一步按住 3 秒后释放 | 执行正常冷重启；不出现 `RPI-RP2`，恢复后先做 5 V + PD 20 V 前置检查 |
| 一直按住 10 秒不释放 | 按住期间不复位；释放后按长按处理，执行正常冷重启 |
| 按住按键的同时上电或触发复位 | 由 RP2350 ROM 采样进入 ROM BOOTSEL；释放后出现 `RPI-RP2`，仅用合成完整 UF2 恢复 |

最后一项是破坏性恢复测试，必须使用合成完整
`radxa-linkr-debugger-rp2350.uf2`；绝不能把应用产物
`build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.uf2`
写入 RPI-RP2。

物理按键由固件线程轮询，不运行在 ISR 中，也不由硬件直接复位。核心完全卡死
时线程无法执行长按动作；这是设计边界，不要把长按当作卡死恢复路径。该故障
模式仍由硬件 watchdog 兜底：普通 watchdog 超时复位通过 recovery marker 进入
ROM BOOTSEL；如果存在 OTA test marker，同一次复位走 MCUboot 回滚路径，按
第 11g 节单独验证，二者不能混用。正常启动后确认 `healthy` 恢复为 `true`。

以上 reboot 与物理按键项在没有对应日期报告和真实返回值摘要之前只能记为
`pending`，不能标为 `[passed]`。

### 9. BOOTSEL Entry

> 本节只验证显式 ROM BOOTSEL。正常 `reboot` 的证据见第 8c 节，不能拿来
> 替代本节。

```sh
timeout 5s curl -fsS -X POST http://172.29.203.1/api/v1/bootloader || true
```

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
FLASH_UF2=build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2
cp "$FLASH_UF2" "$RPI_MOUNT/"
```

烧录完成后，使用有界重试（最多 15 次，每次间隔 2 秒）轮询 HTTP 端点，确认板子已重新枚举并恢复响应：

```sh
BOARD_READY=
attempts=15
while [ "$attempts" -gt 0 ]; do
  if timeout 5s curl -fsS http://172.29.203.1/api/v1/status >/dev/null; then
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

随后重新烧录（使用当前 MCU 对应的 canonical/combined UF2）：

```sh
picotool load -v -x "$FLASH_UF2"
```

**Combined-UF2 dual BOOTSEL guard (`required`, `[passed]` by 2026-07-31 HIL)**: 本节 BOOTSEL
路径必须**同时**校验 HTTP `POST /api/v1/bootloader` 与 CDC ACM shell
`bootloader` 命令都能进入 ROM BOOTSEL，且两个路径都通过同一份合成
`radxa-linkr-debugger-rp2350.uf2` 恢复 HTTP。绝不允许把应用产物
`build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.uf2`
当作 ROM BOOTSEL 烧录源：那份 UF2 不含 MCUboot，会让板子变砖，必须
用合成的 combined UF2 重新刷回。2026-07-31 真实板卡 HIL 已分别给出
HTTP BOOTSEL 与 CDC BOOTSEL 两条路径的成功证据，恢复均仅使用合成
`radxa-linkr-debugger-rp2350.uf2`（本次环境 `picotool` 缺权限，
实际通过 `udisksctl` 挂载 RPI-RP2 分区并复制 combined UF2 恢复，
未刷写应用 UF2），详见
[日期报告](results/2026-07-31-adc3-telemetry-hil.md)。


### 10. USB CDC ACM fallback

验证：

- HTTP/WS 不可用时，CDC ACM 串口仍可访问
- fallback shell 仍可执行基本命令
- fallback 路径仍可进入 BOOTSEL
- 当 HTTP 不可恢复时，CDC `bootloader` shell 命令走的是与 HTTP API 同一份
  合成 `radxa-linkr-debugger-rp2350.uf2` 的刷写路径；永远不要用应用产物
  `build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.uf2`
  去试 ROM BOOTSEL 恢复（该文件不含 MCUboot，会让板子变砖）。

CDC ACM shell 验证（G3 固件）：

```text
linkr-debugger:~$ config show
linkr-debugger:~$ bootloader
```

CDC fallback 用 `config show` 验证只读状态路径；x1.2 不提供旧版 `vin` 命令。

**CDC + combined-UF2 dual BOOTSEL (`required`, `[passed]` by 2026-07-31 HIL)**: 与第 9 节配套，
CDC ACM shell 的 `bootloader` 命令必须经过与 HTTP `POST /api/v1/bootloader`
同一个测试矩阵，并通过同一份合成 `radxa-linkr-debugger-rp2350.uf2` 恢复。
2026-07-31 真实板卡 HIL 已分别在报告里留下两条路径的成功证据，详见
[日期报告](results/2026-07-31-adc3-telemetry-hil.md)。

### 11. MCUboot OTA

> 注意：以下测试用例描述 HIL 验证步骤，不声称已完成这些测试。

OTA 路径无签名、无认证、无安全启动、无防回滚保护。SHA256 仅校验上传负载的完整性。
任何持有 USB NCM 访问权限的主机均可提交固件镜像。

初始安装需要 ROM BOOTSEL 刷写合并型可启动镜像；OTA 仅适用于后续更新。
OTA 只能发送 MCUboot 格式的应用 `.bin` 文件，不能发送 `.uf2` 或 `.elf`。

#### 11a. OTA 状态查询

```sh
timeout 5s radxa-linkr-debuggerctl --json ota status
```

验证返回 JSON 包含 `schema`、`ok`、`command`、`state`，且 `state` 应为 `idle`
或当前实际状态。

#### 11b. 正常上传 / 测试 / 自动确认流程

```sh
# 准备好测试文件（实际 HIL 中使用构建产物路径）
TEST_BIN="build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin"

# 上传 MCUboot 格式应用 bin
timeout 90s radxa-linkr-debuggerctl --json ota upload "$TEST_BIN"

# 请求测试启动
timeout 5s radxa-linkr-debuggerctl --json ota test
```

板子应在短延迟后重启。进入测试镜像后，等待 16 秒 watchdog 健康门槛自动确认。
在此期间可观察：

```sh
timeout 5s radxa-linkr-debuggerctl --json ota status
```

`state` 应依次经历 `pending_test`（测试启动后）、`idle`（自动确认后），
且 `current_image_confirmed` 应为 `true`。

若需手动确认：

```sh
timeout 5s radxa-linkr-debuggerctl --json ota confirm
```

确认后 `state` 应回到 `idle`。

#### 11c. 手动确认（绕过自动确认）

```sh
timeout 90s radxa-linkr-debuggerctl --json ota upload "$TEST_BIN"
timeout 5s radxa-linkr-debuggerctl --json ota test
# 板子重启进入测试镜像后，不等待 16 秒门槛，直接：
timeout 5s radxa-linkr-debuggerctl --json ota confirm
timeout 5s radxa-linkr-debuggerctl --json ota status
# 验证 state=idle 且 current_image_confirmed=true
```

#### 11d. SHA256 不匹配

上传一个文件，但提供错误的 SHA256 header（故意篡改其中一位）：

```sh
# 先获取正确文件的 SHA256
CORRECT_SHA=$(sha256sum "$TEST_BIN" | awk '{print $1}')
# 用错误 SHA256 上传
timeout 5s curl -fsS -X POST \
  -H 'Content-Type: application/octet-stream' \
  -H 'X-Linkr-Ota-Size: <byte_size>' \
  -H "X-Linkr-Ota-Sha256: 0000000000000000000000000000000000000000000000000000000000000000" \
  --data-binary @"$TEST_BIN" \
  http://172.29.203.1/api/v1/ota/upload
# 验证返回 ok=false 且 error.code 包含 "sha256" 相关错误
```

#### 11e. 上传中断

上传过程中断开连接（例如在上传进行到一半时按 Ctrl+C）：

```sh
timeout 3s curl -fsS -X POST \
  -H 'Content-Type: application/octet-stream' \
  -H 'X-Linkr-Ota-Size: <byte_size>' \
  -H "X-Linkr-Ota-Sha256: $CORRECT_SHA" \
  --data-binary @"$TEST_BIN" \
  http://172.29.203.1/api/v1/ota/upload || true
# 立即检查状态：
timeout 5s radxa-linkr-debuggerctl --json ota status
# state 应为 idle 或 failed（不是 uploading）
```

#### 11f. 大请求首错保持

使用完整 OTA 镜像制造首个 fragment 的 Content-Type 错误：

```sh
CORRECT_SHA=$(sha256sum "$TEST_BIN" | cut -d' ' -f1)
timeout 30s curl -sS -o /tmp/linkr-ota-error.json -w '%{http_code}\n' -X POST \
  -H 'Content-Type: text/plain' \
  -H "X-Linkr-Ota-Size: $(stat -c %s "$TEST_BIN")" \
  -H "X-Linkr-Ota-Sha256: $CORRECT_SHA" \
  --data-binary @"$TEST_BIN" \
  http://172.29.203.1/api/v1/ota/upload
```

请求必须在服务端排空所有 body fragment 后返回 HTTP 415 和 JSON
`error.code=unsupported_content_type`。客户端不得收到 transport error；随后查询 OTA 状态时，
`last_error.code` 仍必须为 `unsupported_content_type`，不能被后续 fragment 改写为
`upload_not_started`。

#### 11g. watchdog 复位导致未确认回滚（无自动确认）

此测试验证：当测试镜像未经确认且 watchdog 复位发生时，板子执行 MCUboot
回滚而非进入 ROM BOOTSEL。

步骤：

1. 构建 HIL 基线镜像（`0.0.1+0`）与候选 OTA 镜像（`0.0.2+0`）：
   ```sh
   mkdir -p docs/testing/results/2026-08-28-watchdog-rollback-fault-injection-hil/fault-baseline \
     docs/testing/results/2026-08-28-watchdog-rollback-fault-injection-hil/fault-candidate
   west build -p always -b rpi_pico2/rp2350a/m33/mcuboot \
     --sysbuild apps/radxa_linkr_debugger \
     -d build/radxa_linkr_debugger \
     -- -DEXTRA_CONF_FILE="$PWD/apps/radxa_linkr_debugger/prj-fault.conf"
   cp build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2 \
     docs/testing/results/2026-08-28-watchdog-rollback-fault-injection-hil/fault-baseline/
   west build -p always -b rpi_pico2/rp2350a/m33/mcuboot \
     --sysbuild apps/radxa_linkr_debugger \
     -d build/radxa_linkr_debugger \
     -- -DEXTRA_CONF_FILE="$PWD/apps/radxa_linkr_debugger/prj-fault-candidate.conf"
   cp build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2 \
     docs/testing/results/2026-08-28-watchdog-rollback-fault-injection-hil/fault-candidate/
   make firmware
   ```

2. 先刷写 `0.0.1+0` HIL 基线 combined UF2（`prj-fault.conf`），确认
   `/api/v1/watchdog/fault` 可用且无 OTA marker，再用
   `web-ota-hil.sh --flow watchdog-rollback` 上传 `0.0.2+0` 候选镜像并触发测试
   启动。不同的 MCUboot 版本确保真实 swap；canonical 生产镜像不包含 fault
   hook，其版本（例如本源码树的 `0.3.0+0`）由 `VERSION`/版本同步流程决定。

3. 板子重启进入测试镜像、OTA marker 存在后，runner 调用
   `POST /api/v1/watchdog/fault`。该 hook 禁止 production 构建暴露，并要求
   marker 存在；armed 后 supervisor 停止喂狗，在约 5 秒后触发与真实 liveness
   故障相同的硬件 watchdog reset，10 秒窗口到期会自动 disarmed。

4. 复位后先观察回滚中间态：`state=idle`、`current_image_confirmed=true`、
   `test_marker_present=true`，证明 marker 尚未被 auto-confirm 清掉而是由
   MCUboot 回滚保留。随后 runner 等待 marker 清除，并校验回滚后的
   `build.image_version` 与基线一致、uptime 低于“基线 uptime+经过时间”的无复位
   预期值。若出现了 RPI-RP2 磁盘枚举，则测试失败。

> 此路径的自动化入口是 `web-ota-hil.sh --flow watchdog-rollback`，它同时验证
> marker 不存在时 arm 被拒绝、arm 后发生运输中断、恢复后无 BOOTSEL 枚举。
> 2026-08-28 真实板级结果是
> [Watchdog Rollback Fault-Injection HIL](results/2026-08-28-watchdog-rollback-fault-injection-hil.md)。

#### 11h. OTA 路径拒绝非 bin 文件

```sh
# 仅使用空的临时 .uf2 验证客户端拒绝扩展名；绝不把 combined UF2 发送到 OTA。
INVALID_UF2=$(mktemp --suffix=.uf2)
trap 'rm -f "$INVALID_UF2"' EXIT
timeout 5s radxa-linkr-debuggerctl --json ota upload "$INVALID_UF2"
# 验证返回 ok=false；不应开始上传或改变设备 OTA state。
```

#### 11i. 显式 HTTP BOOTSEL 与 CDC ACM BOOTSEL（已在第 9、10 节覆盖）

OTA 测试完成后，通过以下方式重新进入 BOOTSEL 并刷写，确认回退路径未被破坏：

```sh
timeout 5s curl -fsS -X POST http://172.29.203.1/api/v1/bootloader || true
# 使用第 9 节有界重试循环确认 BOOTSEL 枚举
```

CDC ACM shell 回退：

```text
linkr-debugger:~$ bootloader
```

### 12. Web OTA HIL Automation

This section covers the automated Web OTA HIL runners: the POSIX shell API runner
(`docs/testing/scripts/web-ota-hil.sh`) and the Node.js browser
runner (`web/scripts/ota-hil.mjs`). Both runners are HIL tooling, not production
OTA delivery mechanisms.

#### 12a. Runner types

**API-level runner** (`web-ota-hil.sh`): Issues raw HTTP requests against the
board OTA endpoints. Operates at the API layer without a browser. Useful for
rapid headless validation of the OTA state machine, error codes, and gate logic.

USB NCM re-enumeration can outlast the firmware's watchdog auto-confirm window.
The API runner uses `test_marker_present` to distinguish a missed auto-confirm
from rollback: `idle` plus `current_image_confirmed:true` plus a cleared marker
is success. Immediately after an MCUboot rollback the marker may remain set
briefly while firmware observes that the running image is already confirmed;
the runner waits until firmware clears it. Missing marker evidence is
inconclusive. In the manual flow, `/confirm` is sent only after `pending_test` is
observable, and the terminal state must also show the marker cleared. This
classification assumes no external power-cycle or BOOTSEL action interrupts the
controlled HIL run.

**Browser runner** (`ota-hil.mjs`): Drives a real Chromium/Chromium-browser
instance via Playwright against the board-hosted Web UI at `http://172.29.203.1/`.
Exercises the full front-end OTA card including local SHA-256 computation,
upload, confirmation dialogs, and state polling. Distinguishes two confirmation
flows: `auto` (waits for firmware watchdog auto-confirm) and `manual` (clicks
Confirm image after pending_test).

Both runners default to dry-run/read-only behavior and require explicit
`--execute` to run side-effectful operations. The `--flow all` mode is
dry-run-only; it cannot be combined with `--execute`.

#### 12b. Shared gate model

| Gate flag | Required for |
|---|---|
| `--allow-upload-test-reboot` | OTA upload, test boot, manual confirm |
| `--allow-watchdog-fault` | Arm the HIL-only watchdog fault hook during rollback |
| `--allow-bootsel` | HTTP or CDC ACM BOOTSEL entry |
| `--allow-flash` | UF2 copy to RPI-RP2 mount point |

Gates are checked only when `DRY_RUN=0` (i.e., after `--execute`). Dry-run
plans print the commands that would run without executing them.

#### 12c. Shell API runner dry-run examples

Default preflight (read-only, no gate required):

```sh
./docs/testing/scripts/web-ota-hil.sh --flow preflight
# DRY-RUN: run_timeout 5s curl -fsS http://172.29.203.1/api/v1/status
# DRY-RUN: run_timeout 5s curl -fsS http://172.29.203.1/api/v1/watchdog
# DRY-RUN: run_timeout 5s curl -fsS http://172.29.203.1/api/v1/ota
```

OTA status only:

```sh
./docs/testing/scripts/web-ota-hil.sh --flow status
```

Negative upload plan (dry-run, no gate required):

```sh
./docs/testing/scripts/web-ota-hil.sh --flow negative-upload
```

Full dry-run plan (all flows, dry-run only):

```sh
./docs/testing/scripts/web-ota-hil.sh --flow all
# Fails if combined with --execute
```

#### 12d. Shell API runner executable examples

Upload, auto-confirm flow (requires gate):

```sh
./docs/testing/scripts/web-ota-hil.sh \
  --flow api-auto-confirm \
  --image build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin \
  --execute --allow-upload-test-reboot
```

Upload, manual-confirm flow (requires gate):

```sh
./docs/testing/scripts/web-ota-hil.sh \
  --flow api-manual-confirm \
  --image build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin \
  --execute --allow-upload-test-reboot
```

Negative upload validation (requires gate):

```sh
./docs/testing/scripts/web-ota-hil.sh \
  --flow negative-upload \
  --execute --allow-upload-test-reboot
```

Watchdog rollback validation (requires fault HIL image and both gates):

```sh
./docs/testing/scripts/web-ota-hil.sh \
  --flow watchdog-rollback \
  --image docs/testing/results/2026-08-28-watchdog-rollback-fault-injection-hil/fault-candidate/zephyr.signed.bin \
  --execute --allow-upload-test-reboot --allow-watchdog-fault
```

HTTP BOOTSEL entry (requires gate):

```sh
./docs/testing/scripts/web-ota-hil.sh \
  --flow bootsel-http \
  --execute --allow-bootsel
```

CDC ACM BOOTSEL entry (requires gate and tty):

```sh
./docs/testing/scripts/web-ota-hil.sh \
  --flow bootsel-cdc \
  --tty /dev/ttyACM0 \
  --execute --allow-bootsel
```

UF2 copy to RPI-RP2 mount (requires gate):

```sh
./docs/testing/scripts/web-ota-hil.sh \
  --flow flash-uf2 \
  --uf2 build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2 \
  --execute --allow-flash
```

The API runner requires `CONFIG_LINKR_DEBUGGER_FAULT_INJECTION=y` for rollback;
production firmware rejects the flow when the fault endpoint is unavailable.

#### 12e. Browser runner dry-run example

```sh
cd web
node scripts/ota-hil.mjs --dry-run
# Prints a JSON plan describing the browser actions
```

Browser runner accepts `--playwright-module` and `--chromium-executable` to
control Playwright loading and the browser binary path. It does not require
global Playwright installation; Playwright-core is loaded dynamically from the
module resolution path. For Nix users, a temporary nix environment or explicit
Nix store paths can supply the Chromium dependency without claiming an exact
unverified package attribute.

#### 12f. Browser runner executable examples

```sh
cd web
node scripts/ota-hil.mjs --execute --flow both
node scripts/ota-hil.mjs --execute --flow auto
node scripts/ota-hil.mjs --execute --flow manual \
  --chromium-executable /nix/store/...-chromium-.../bin/chromium
```

The runner uses bounded timeouts (5-second short operations, 45-second reboot
wait, 120-second upload timeout) and polling with diagnostics truncation. It
validates `.bin` extension and non-empty file before upload. This browser runner
does not exercise watchdog rollback; it reports that implementation separately
and defers to the shell API runner for fault injection.

#### 12g. Port and URL

Both runners use `http://172.29.203.1` as the default board URL. The API runner
default is port 80. The browser runner connects to the board-hosted Web UI on the
same NCM-assigned address.

### 12h. Rolling Nightly Prerelease Isolation

`.github/workflows/nightly.yml` 仅在推送到 `dev` 分支时产出
mutable `nightly` Git tag 与一个标记为 `not latest` 的 prerelease，发布一个
固定的 9-asset 子集，并显式省略正式 Release 才包含的
`radxa-linkr-debuggerctl-rust_linux_arm64.tar.gz` Linux ARM64 CLI 归档。本节描述 nightly 通道与正式 HIL 的隔离约定，
本身不要求任何具体实物跑分；它属于发布链路约束，避免污染既有 artifact 与
HIL 报告。

期望检查项（每条 `required`、`pending`，未跑真实 evidence 之前不能算 pass）：

- nightly workflow 是**仅**修改 GitHub Release 与 tag 的发布链路，不应驱动
  任何专用 `[passed]` 状态写入 `docs/testing/results/`。任何 `[passed]`
  标签只能来自真实板卡 HIL 流程。
- 正式 `v*` release 的 SHA256SUMS、`radxa-linkr-debugger-rp2350.uf2`、
  `radxa-linkr-debugger-rp2350-ota.bin` 等产物在 nightly run 与正式
  release run 之间应保持各自独立。即便两类产物在同一天出现，也不应
  复用同一份产物摘要；CI 必须分别为两条通道跑出 SHA256。
- 任何由 nightly 渠道驱动的烧写（例如 in-house developer 使用
  `nightly` tag 验证 bleeding-edge 改动）必须使用合成
  `radxa-linkr-debugger-rp2350.uf2` 而**不是**应用产物
  `build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.uf2`；
  应用 UF2 没有 MCUboot，线刷会变砖。
- nightly 渠道的 bleed 改动**不**覆盖正式 `v*` release 的下载链接；
  HIL 验收仍以正式 `Release` workflow 的资产为权威来源。
- 任何"nightly 已通过 HIL"的说法都不被允许。nightly 通道是滚动
  测试产物，不构成 formal validation evidence；正式 HIL 证据必须独立
  写到命名带日期和测试主题的 `docs/testing/results/<date>-<topic>-hil.md`。

## 最短 HIL smoke test

时间紧张时，至少验证以下 7 项：

1. `--json doctor`
2. `--json status`
3. `power set/get`
4. `adc read`
5. `switch route/get`
6. `bootloader`
7. `reboot`（正常冷重启；恢复后先做 5 V + PD 20 V 前置检查，且不得出现
   `RPI-RP2`）

额外验证：8. `ota status`（确认 state=idle，不上传不测试）

x1.2 不执行已删除的 VIN 路由用例。OTA 上传/测试/确认流程属于可选的延长 HIL 项，不在最短 smoke test 范围内。

## 验证结果记录建议

建议记录：

- 提交 hash
- 固件版本/tag
- 构建目录与 UF2 路径
- 烧录方式
- 实测命令与返回值摘要
- 是否通过 BOOTSEL 与 CDC ACM fallback
- reboot 是否保持正常启动且未出现 RPI-RP2
- 物理 BOOTSEL 长按/短按/按住复位的结果
- 每次启动后的 `5v_out` + PD 20 V 前置检查结果
- 失败项与风险说明

## Final Post-Fix HIL Evidence

The current combined ADC, connector-layout, five-output power catalog, J4 serial,
automatic-responder matrix, raw-TCP 5555, embedded Web terminal,
persistent-startup and post-capture evidence is recorded in
[2026-10-21 x1.2 ADC/pinout/J4 HIL](results/2026-10-21-x1.2-adc-pinout-j4-hil.md).
Its PASS scope is digital/functional and explicitly excludes the unperformed
ADC0 physical source unplug/replug monitor, stuck-line electrical injection,
analog 50 ns/tr/VOL/Ioff certification, and the active worst-case
480-byte-at-4800 8E2 timeout/cancel branch beyond the observed 1.213s physical
completion. It also does not certify hub current, because `5v_usb_hub` has no
INA226/ADC channel. The J4 product baud ceiling is 115200; the report's
230400/921600/1500000 captures are high-rate failure diagnostics and never
product PASS rows. The report also records the final CDC line-coding
staging/promotion behavior: DTR=false defaults remain staged, HTTP/WS low-state
changes are immediate, and LOW→HIGH publishes DTR/ingress only after staged work
completes or rolls back.

The current bounded pre-trigger and UART sample-0 decoder results are recorded in
`docs/testing/results/2026-07-28-logic-analyzer-pre-trigger-uart-hil.md`, including
canonical footprint, HTTP and CDC BOOTSEL recovery, WebSocket protocol evidence,
browser decode evidence, and visual QA.

The current SINGLE sustained-duration evidence under the strict duration and
negotiated-sample-count contract is recorded in
docs/testing/results/2026-08-30-logic-analyzer-v2-dense-hil.md. It is the
authoritative source for protocol version 2, SINGLE_BITS/SINGLE_BITS_RLE,
WS/TCP 16 MHz ten-run operating points, 18.002 MHz adjacent failures, bounded
post=65535 regression, host A/B benchmarks, UART limitation, dual BOOTSEL use,
and final production restoration.

The current multichannel sustained-duration, activity-qualified, DMA RXSTALL, palette
compression, chunk-size, queue-depth, and resource evidence is recorded in
docs/testing/results/2026-08-31-logic-analyzer-v2-multichannel-hil.md. It is authoritative for
FAST8/WIDE11 protocol-v2 operating points, raw active-input baselines, adjacent failures, and
final production restoration. It does not supersede the separate 16 MHz SINGLE report.

The 2026-08-30 v1 fast-path report remains authoritative historical evidence for
the byte-per-sample 300 kHz through 1 MHz matrix and its restoration. The
2026-08-24 report remains historical evidence for the earlier 300/350 kHz
operating points, overrun diagnostics, high-rate matrix, stack high-water values,
and clean detached-HEAD A/B. Their dated observations remain unchanged.

The following historical results were obtained from earlier post-fix HIL runs.
They are not rewritten as current sustained PASS claims and they are not
projections or estimates of the current architecture.

Full logic analyzer finite HIL evidence report (2026-07-25):
`docs/testing/results/2026-07-25-logic-analyzer-finite-hil.md`

### Historical WS SINGLE 1MHz Continuous (`/tmp/linkr-final-ws-1mhz.json`)
Historical, pre-transport-cleanup evidence. The then-current pipeline received
approximately 4.995 million samples near 999 ksample/s with zero gaps and clean
STOP/restart/health. A later v1 pipeline regressed, then the 2026-08-30 v1 fast
path restored 1 MHz for compressible input. Protocol v2 now supersedes all of
those runtime contracts and passes the current strict rule at 16 MHz. This row
is retained only as historical context.

### GP10 UART Trigger Validation (`/tmp/linkr-final-gp10-*.json`)
Six isolated bounded captures at 1 MHz and 2 MHz, rising/falling/either trigger types,
each with 4096 samples and trigger offset 0. All six passes:
`/tmp/linkr-final-gp10-1mhz-rising.json`, `/tmp/linkr-final-gp10-1mhz-falling.json`,
`/tmp/linkr-final-gp10-1mhz-either.json`, `/tmp/linkr-final-gp10-2mhz-rising.json`,
`/tmp/linkr-final-gp10-2mhz-falling.json`, `/tmp/linkr-final-gp10-2mhz-either.json`.
These bounded pre-trigger rows continue to use 1 MHz / 2 MHz intentionally
because bounded pre-trigger is the contract that historically needed 1 MHz
validation, and the trigger tests remain passing on the current architecture.

### TCP Bounded 100kHz (`/tmp/linkr-final-tcp-bounded.json`)
All three modes (SINGLE, FAST8, WIDE11) received exactly 65535 samples with zero
sample-index gaps, stop response received, immediate restart and HTTP health confirmed.

These historical results demonstrate the bounded/triggered implementation
against the reference smoke test criteria. They are not projections or
estimates. The historical WS SINGLE 1 MHz row is context only; the authoritative
current SINGLE sustained-duration evidence is the protocol-v2 dense HIL report
cited above.

## 参考来源

- `README.md`
- `AGENTS.md`
- `skills/radxa-linkr-debugger/SKILL.md`
