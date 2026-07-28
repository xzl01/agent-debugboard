# HIL 功能测试规范

本文档定义 `radxa-linkr-debugger` 固件与主机控制逻辑改动后的板级 HIL（Hardware-in-the-Loop）功能测试规范。

## 适用范围

以下改动不能只依赖编译/静态检查/unit test，必须完成 HIL 实机功能验证：

- RP2350 固件逻辑改动
- 主机 CLI/TUI 真实控制逻辑改动
- 电源输出、switch 路由、ADC 电流监测、safe GPIO、watchdog、BOOTSEL 相关改动
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
5. 验证 BOOTSEL 进入能力（BOOTSEL entry path）
6. OTA 路径相关改动必须验证上传、测试启动、手动确认基本流程
7. 新增功能补对应 functional test（可行时）

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

验证 HTTP 响应 JSON 长度低于 4096 字节（协议限制检查）：

```sh
LEN=$(timeout 5s curl -fsS http://172.29.203.1/api/v1/status | wc -c)
echo "status response size: $LEN bytes"
[ "$LEN" -lt 4096 ] || { echo "status response must be below 4096 bytes"; exit 1; }
```

验证 WebSocket `snapshot/status` 包含相同的 `memory` 形状和 Phase 2 对象：

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

串口卡片必须按以下两个分支之一完成验证：

**分支 A（override 已启用，直连 Web Serial）**：

1. 在 `chrome://flags/#unsafely-treat-insecure-origin-as-secure` 中加入精确来源
   `http://172.29.203.1`（将地址复制后粘贴到浏览器地址栏，普通网页无法直接导航到
   `chrome://` 页面）。Edge 同样接受该 Chromium 地址。
2. 重启浏览器，重新打开板载页面 `http://172.29.203.1/`。
3. 确认卡片不再显示红色设置状态，每个可见 UART pane 只在 **Bridge** 旁显示一个
   **Web Serial** 按钮；该按钮可发起浏览器设备选择器。手动选择 CH347 设备，验证可以收发
   串口数据。chooser 是浏览器的强制安全机制，测试中不可绕过。

该 override 属于实验性配置，会降低该来源安全性；它不会移除用户手势或设备选择器要求。

**分支 B（override 未启用，bridge 回退）**：

1. 确认板载页面每个可见 UART pane 只在 **Bridge** 旁显示一个红色 **Web Serial** 按钮，
   卡片标题区不显示该按钮，原串口说明控件不再渲染；点击 pane 内按钮后弹出三步设置说明。
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
rm -f /tmp/linkr-hil-1000.ndjson
timeout 5s radxa-linkr-debuggerctl adc record /tmp/linkr-hil-1000.ndjson 1000 --rate-hz 1000
python3 - <<'PY'
import json
from pathlib import Path

rows = [json.loads(line) for line in Path('/tmp/linkr-hil-1000.ndjson').read_text().splitlines()]
assert len(rows) == 1000, f'expected 1000 telemetry records, got {len(rows)}'
for index, row in enumerate(rows):
    timing = row.get('metadata', {}).get('device_timing', {})
    for field in ('sample_sequence', 'device_t_mono_us', 'uptime_us'):
        assert isinstance(timing.get(field), int), f'row {index} missing {field}'
    dropped = row.get('metadata', {}).get('dropped_samples')
    if dropped is not None:
        assert isinstance(dropped, int) and dropped > 0, f'row {index} invalid dropped_samples: {dropped}'
print('batch telemetry timing aliases passed')
PY
```

用连续记录的 `sequence`、`metadata.device_timing.sample_sequence` 和
`metadata.device_timing.device_t_mono_us` 计算实际采样间隔；紧凑 batch 在线路上只提供
`sequence` 与 `uptime_us` 时，Rust recorder 必须归一化出前述别名。不得用 WebSocket 帧数代替样本数。固件支持最多四个并发客户端，四路并发测试必须确认每个
输出文件都持续增长，且任一客户端退出不会中断其他客户端。

### 2c. 强制门户发现

验证 DHCP option 114（Captive Portal URI）已正确通告。利用 tcpdump 在 NCM 接口
捕获 DHCP DORA 过程，解析 option 3（router）、option 6（DNS）和 option 114
（Captive Portal URI）。以下为 Linux 示例；macOS/Windows 使用对应平台 tcpdump
或 DHCP 工具：

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
# 包含 option 3、6、114 的精确值。
timeout 5s tshark -r "$DHCP_CAPTURE" -Y 'dhcp.option.dhcp == 3' -V \
  > /tmp/linkr-dhcp-request.txt
timeout 5s tshark -r "$DHCP_CAPTURE" -Y 'dhcp.option.dhcp == 5' -V \
  > /tmp/linkr-dhcp-ack.txt
grep -E 'Parameter Request List|Captive-Portal|Option.*114' \
  /tmp/linkr-dhcp-request.txt
grep -E 'Router|Domain Name Server|Captive-Portal|Option.*(3|6|114)' \
  /tmp/linkr-dhcp-ack.txt

if [ "$NCM_WAS_MANAGED" = yes ]; then
  sudo timeout 5s dhclient -r "$NCM_IFACE" || true
  sudo nmcli device set "$NCM_IFACE" managed yes
  sudo nmcli device connect "$NCM_IFACE"
fi
```

客户端请求的 Parameter Request List 必须包含 option 114。ACK 必须包含 router
`172.29.203.1`、DNS `172.29.203.1` 和 option 114 URI
`http://172.29.203.1/captive-portal/api`；把 `/tmp/linkr-dhcp-ack.txt` 留作
HIL 记录。上述命令会恢复原 NetworkManager 所有权；不要同时运行两个 DHCP
client。

验证 DNS 泛解析 A 记录（对任意查询名返回 `172.29.203.1`）：

```sh
timeout 5s dig +short @172.29.203.1 anything.example A
```

期望 `172.29.203.1`；若输出为空或包含非 IP 字符串则失败。

验证 DNS AAAA 查询返回 NOERROR 且 answers 为空（NODATA）：

```sh
timeout 5s dig @172.29.203.1 anything.example AAAA +notcp +tries=1 +time=2 +noedns
```

通过检查输出中是否存在 `NOERROR` 且 `ANSWER: 0` 来区分 NODATA 与
timeout/NXDOMAIN：

```sh
timeout 5s dig @172.29.203.1 anything.example AAAA +notcp +tries=1 +time=2 +noedns | python3 -c "
import sys
import re
text = sys.stdin.read()
m = re.search(r'status:\s*([A-Z]+).*?ANSWER:\s*(\d+)', text, re.S)
assert m is not None, f'Cannot parse dig header: {text[:300]}'
status, answers = m.group(1), int(m.group(2))
assert status == 'NOERROR' and answers == 0, f'Expected NOERROR + 0 answers, got {status} + {answers}'
print('AAAA NOERROR/NODATA OK')
"
```

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
  echo "External DNS resolution is unavailable or captured by the board DNS"
  exit 1
}
echo "Existing Internet route and DNS remain usable"
```

**多宿主路由说明**：若 host 同时有其他网络接口且默认路由指向其他适配器，
操作系统可能不会向 NCM 接口发起强制门户检测请求，自动打开不保证发生。
此时用户可直接打开 `http://172.29.203.1/` 或使用 `curl`、CLI/TUI。

### 3. 电源输出 get/set

```sh
timeout 5s radxa-linkr-debuggerctl --json power set 12v_out on
timeout 5s radxa-linkr-debuggerctl --json power set 12v_out off

timeout 5s radxa-linkr-debuggerctl --json power set 5v_out on
timeout 5s radxa-linkr-debuggerctl --json power set 5v_out off

# vdd_5v 是普通可控电源轨：出现在列表中，可 get/set；默认路由 target 下开机为 off。
timeout 5s radxa-linkr-debuggerctl --json power list > /tmp/linkr-power-list.json
grep -q '"name":"vdd_5v"' /tmp/linkr-power-list.json
timeout 5s radxa-linkr-debuggerctl --json power get vdd_5v
timeout 5s radxa-linkr-debuggerctl --json power set vdd_5v on
timeout 5s radxa-linkr-debuggerctl --json power get vdd_5v
timeout 5s radxa-linkr-debuggerctl --json power set vdd_5v off

# vdd_5v 随 switch usb 路由联动：切 pc 强制开，切 target 强制关（覆盖手动状态）。
timeout 5s radxa-linkr-debuggerctl --json switch route usb pc --confirm
timeout 5s radxa-linkr-debuggerctl --json power get vdd_5v   # 期望 on
timeout 5s radxa-linkr-debuggerctl --json power set vdd_5v off   # 路由不变时允许手动
timeout 5s radxa-linkr-debuggerctl --json power get vdd_5v   # 期望 off
timeout 5s radxa-linkr-debuggerctl --json switch route usb target --confirm
timeout 5s radxa-linkr-debuggerctl --json power get vdd_5v   # 期望 off（联动强制）

# 注意：vdd_5v 关闭期间 CH347 1.8V VIN（VDD_1V8 子电源轨）会断电。

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

观察 GPIO25 心跳 LED：在 `/api/v1/watchdog` 返回 healthy 状态后，
目视检查蓝色状态 LED 应在大约 1 秒周期内亮灭交替。视觉观察时间窗口
bound 到 5 秒以内（足够看到 2-3 次完整周期）。

### 8b. Logic Analyzer (Sigrok over WebSocket / TCP)

The logic analyzer uses RP2350 PIO2+DMA for high-speed GPIO capture. The sigrok
binary protocol runs over two transports: Web UI uses `/api/v1/live-sessions` then
`/api/v1/ws/<slot>` binary Sigrok; native sigrok/PulseView uses raw-TCP port 5556.

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

Verify stream mode runs until stopped. Stream at 1 MHz for 5 seconds, verify
continuous sample delivery without gaps, then stop cleanly.

#### 8b.4 Continuous Ceilings and 1MHz Stability

Canonical 1MHz validation: stream WS SINGLE at 1MHz for 5 seconds, repeated for
10 consecutive runs. Each run must satisfy:

- JSON `overall_pass: true`
- Effective rate >= 950 ksps
- Zero sample-index gaps
- Zero disconnects
- Zero protocol-level decode errors
- `stop_response.received: true`
- Immediate restart capability confirmed
- HTTP health after stop

| Transport | Mode | Rate | Expected |
|-----------|------|------|----------|
| WebSocket | SINGLE continuous | 1 MHz | 10 consecutive 5-second runs, zero gaps/disconnects, >= 950 ksps |
| WebSocket | FAST8 continuous | 240 kHz | 5-second no-gap |
| WebSocket | FAST8 continuous | 241 kHz | Adjacent failure, explicit terminal, restart and HTTP health retained |
| WebSocket | WIDE11 continuous | 149 kHz | 5-second no-gap |
| WebSocket | WIDE11 continuous | 150 kHz | Adjacent failure, explicit terminal, restart and HTTP health retained |
| TCP | SINGLE continuous | 443 kHz | 5-second no-gap (historical/representative) |
| TCP | SINGLE continuous | 444 kHz | Adjacent failure, explicit terminal, restart and HTTP health retained |
| TCP | FAST8 continuous | 241 kHz | 5-second no-gap |
| TCP | FAST8 continuous | 242 kHz | Adjacent failure, explicit terminal, restart and HTTP health retained |
| TCP | WIDE11 continuous | 147 kHz | 5-second no-gap |
| TCP | WIDE11 continuous | 148 kHz | Adjacent failure, explicit terminal, restart and HTTP health retained |

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
| WS | SINGLE | continuous | 1 MHz | 0 | 4,997,120 samples, 999,340.8 samples/s, zero sample-index gaps, zero disconnects |

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
(GP18-GP20, 3-bit autopush30, 40000 B); two DMA channels; 144184 B shared arena.
GP29 is excluded from WIDE11 LA (available as ordinary GPIO/ADC3). NONE deep burst uses
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

After each deep burst run, verify HTTP health and restart capability. The 144184 B
dual-SM packed arena restores ADC telemetry, power capture, and normal Sigrok pool after drain;
confirm these services are functional after the capture completes.

The final authoritative TCP/WS matrix passed 54/54 cases and the high-rate
matrix passed 62/62. Eighteen continuous matrix rows ended with an explicit
capacity OVERRUN; four had `capacity_stop_before_data=true`. Those rows prove
lossless-or-stop terminal behavior, not sustained operation at the requested
rate. Historical WIDE12 predecessor evidence remains at
`doc/testing/results/2026-07-26-logic-analyzer-wide12-100k-hil.md`.

#### 8b.4.2 WIDE11 Deep-Burst Pin Mapping HIL

The executable pin-mapping HIL is `--matrix wide11-mapping` in
`apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py`. It must capture the
exact WIDE11 deep-burst mode (`100 MHz`, `pre=0`, `post=100000`, CONFIG_V2), decode
DATA payload as little-endian 11-bit samples (GP10=bit0 through GP20=bit10), and
clearly identify which stimulus profile was used. The full external-generator profile
verifies these independent bit mappings:

| Physical pin | DATA bit |
|--------------|----------|
| GP10 | bit0 |
| GP11 | bit1 |
| ... | ... |
| GP20 | bit10 |

GP29 / ADC3 is excluded from WIDE11 LA (available as ordinary GPIO/ADC3).

Do not use HTTP safe-GPIO outputs as the stimulus source for this case. WIDE11 PIO
preparation configures GP10-GP20 as inputs (`gpio_pin_configure(...,
GPIO_INPUT)`, `pio_gpio_init(...)`, and PIO input pindirs), so any same-pin HTTP
safe-GPIO output would be overridden or conflict with the capture ownership model.

Full external 4-bit prerequisite for a valid `external_4bit` pass:

- External 3.3 V-compatible pattern generator D0 -> GP10 (DATA bit0 and trigger
  channel 0)
- External generator D1 -> GP11 (DATA bit1)
- External generator D2 -> GP20 (DATA bit10)
- External generator GND -> Linkr Debugger GND
- Hold the idle state with GP10 low before arming; after START/armed state, emit the
  declared repeating nibble pattern. Nibble bit0 drives GP10, bit1 drives GP11,
  bit2 drives GP20. The default pattern holds each nibble for
  64 samples at 100 MHz and is intentionally transition-rich enough to reject a
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
0/1/10/11 across the configured check window.

Reduced single-wire setup for remote validation is explicitly separate and must not be
reported as the full external 4-bit mapping test. Use it only when `/dev/ttyACM1` TX is
wired to GP10 and GP11, GP20, and GP29 remain externally low:

```sh
timeout 5s python3 apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py \
  --matrix wide11-mapping \
  --wide11-map-gp10-uart-low-others \
  --timeout 5
```

The reduced case uses `stimulus_profile: "gp10_uart_low_others"` and sends the UART
stimulus only after START_RESP, which is the trigger-safe barrier. It verifies GP10
DATA bit0 sees both low and high runs, and that the zero mask `0x0c02` remains clear
for every checked sample: GP11(bit1) and GP20(bit10) must stay low.
Any high sample on bits1/10 is a failure and should be reported as unexpected
high/crosstalk. This reduced case does **not** validate independent high-state mapping
for GP11 or GP20. GP29 is excluded from WIDE11 LA (available as ordinary GPIO/ADC3).

#### 8b.4.3 WIDE11 Shared-Arena Telemetry Isolation HIL

The executable two-client regression is `--matrix wide11-telemetry-isolation` in
`apps/radxa_linkr_debugger/tests/logic_analyzer_hil_perf.py`. It validates that a
normal JSON WebSocket ADC telemetry client stays connected but pauses while a second
client holds the WIDE11 shared-arena lease through raw-TCP sigrok deep burst.
WIDE11 uses two packed capture SMs and two DMA channels in the 144184-byte
shared arena; triggered capture adds a third trigger-only SM.

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
  trigger-safe prepared/running path and the shared-arena pause/lease interval has
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
- `/dev/ttyACM1` is the WCH CH347 serial port; verify with `fuser` or `ls -la /dev/ttyACM*`
- `/dev/ttyACM2` is the firmware CDC ACM port
- No terminal program is attached to `/dev/ttyACM1`

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

CDC `/dev/ttyACM2` BOOTSEL and combined-UF2 restore: the final HIL confirmed
that issuing `bootloader` from the CDC ACM shell on `/dev/ttyACM2` entered ROM
BOOTSEL as the RPI vendor disk, and copying the combined UF2 restored HTTP
after the board re-enumerated and the flash health check passed.

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

### 9. BOOTSEL Entry

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

1. 上传并触发测试启动，不等待 16 秒确认窗口：
   ```sh
   timeout 90s radxa-linkr-debuggerctl --json ota upload "$TEST_BIN"
   timeout 5s radxa-linkr-debuggerctl --json ota test
   # 板子重启进入测试镜像后，立即切断 core liveness（模拟故障）
   ```

2. 在 16 秒门槛到期前，通过 HTTP/WS/cmdline 制造 watchdog 复位（例如
   使 core 线程停止喂狗，使 API 服务无响应，或断开 CDC ACM liveness）。

3. 复位后检查：板子应通过 MCUboot 回滚到上一个已确认镜像，`state` 回到 `idle`，
   而非进入 ROM BOOTSEL。若进入了 ROM BOOTSEL（表现为 RPI-RP2 磁盘枚举），
   则测试失败。

> 此测试用例需要制造受控的 liveness 故障，目前固件暂无安全的 fault injection
> 路径，详见固件 README 的 FIXME 说明。此处记录预期行为，待 fault injection
> 路径实现后补充自动化验证。

#### 11h. OTA 路径拒绝非 bin 文件

```sh
# 尝试上传 .uf2 文件（应被拒绝或写入后验证失败）
timeout 90s radxa-linkr-debuggerctl --json ota upload radxa-linkr-debugger-rp2350.uf2
# 验证返回 ok=false 或 state=failed
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
(`skills/radxa-linkr-debugger/scripts/web-ota-hil.sh`) and the Node.js browser
runner (`web/scripts/ota-hil.mjs`). Both runners are HIL tooling, not production
OTA delivery mechanisms.

#### 12a. Runner types

**API-level runner** (`web-ota-hil.sh`): Issues raw HTTP requests against the
board OTA endpoints. Operates at the API layer without a browser. Useful for
rapid headless validation of the OTA state machine, error codes, and gate logic.

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
| `--allow-bootsel` | HTTP or CDC ACM BOOTSEL entry |
| `--allow-flash` | UF2 copy to RPI-RP2 mount point |

Gates are checked only when `DRY_RUN=0` (i.e., after `--execute`). Dry-run
plans print the commands that would run without executing them.

#### 12c. Shell API runner dry-run examples

Default preflight (read-only, no gate required):

```sh
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh --flow preflight
# DRY-RUN: run_timeout 5s curl -fsS http://172.29.203.1/api/v1/status
# DRY-RUN: run_timeout 5s curl -fsS http://172.29.203.1/api/v1/watchdog
# DRY-RUN: run_timeout 5s curl -fsS http://172.29.203.1/api/v1/ota
```

OTA status only:

```sh
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh --flow status
```

Negative upload plan (dry-run, no gate required):

```sh
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh --flow negative-upload
```

Full dry-run plan (all flows, dry-run only):

```sh
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh --flow all
# Fails if combined with --execute
```

#### 12d. Shell API runner executable examples

Upload, auto-confirm flow (requires gate):

```sh
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh \
  --flow api-auto-confirm \
  --image build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin \
  --execute --allow-upload-test-reboot
```

Upload, manual-confirm flow (requires gate):

```sh
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh \
  --flow api-manual-confirm \
  --image build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/zephyr.signed.bin \
  --execute --allow-upload-test-reboot
```

Negative upload validation (requires gate):

```sh
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh \
  --flow negative-upload \
  --execute --allow-upload-test-reboot
```

HTTP BOOTSEL entry (requires gate):

```sh
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh \
  --flow bootsel-http \
  --execute --allow-bootsel
```

CDC ACM BOOTSEL entry (requires gate and tty):

```sh
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh \
  --flow bootsel-cdc \
  --tty /dev/ttyACM0 \
  --execute --allow-bootsel
```

UF2 copy to RPI-RP2 mount (requires gate):

```sh
./skills/radxa-linkr-debugger/scripts/web-ota-hil.sh \
  --flow flash-uf2 \
  --uf2 build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2 \
  --execute --allow-flash
```

Watchdog rollback is always reported as BLOCKED because no safe
fault-injection path exists.

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
validates `.bin` extension and non-empty file before upload. Watchdog rollback
is BLOCKED and reported as such in the result object.

#### 12g. Port and URL

Both runners use `http://172.29.203.1` as the default board URL. The API runner
default is port 80. The browser runner connects to the board-hosted Web UI on
the same NCM-assigned address.

## 最短 HIL smoke test

时间紧张时，至少验证以下 6 项：

1. `--json doctor`
2. `--json status`
3. `power set/get`
4. `adc read`
5. `switch route/get`
6. `bootloader`

额外验证：7. `ota status`（确认 state=idle，不上传不测试）

VIN 1.8V 切换不属于默认 smoke test 范围；它需要目标板电压兼容确认和物理测量验证，属于条件限定的可选步骤，详见上方第 6 节。OTA 上传/测试/确认流程属于可选的延长 HIL 项，不在最短 smoke test 范围内。

## 验证结果记录建议

建议记录：

- 提交 hash
- 固件版本/tag
- 构建目录与 UF2 路径
- 烧录方式
- 实测命令与返回值摘要
- 是否通过 BOOTSEL 与 CDC ACM fallback
- 失败项与风险说明

## Final Post-Fix HIL Evidence

The current bounded pre-trigger and UART sample-0 decoder results are recorded in
`doc/testing/results/2026-07-28-logic-analyzer-pre-trigger-uart-hil.md`, including
canonical footprint, HTTP and CDC BOOTSEL recovery, WebSocket protocol evidence,
browser decode evidence, and visual QA.

The following results were obtained from actual post-fix HIL runs against the final
firmware build. The dated repository report at `doc/testing/results/2026-07-25-logic-analyzer-finite-hil.md`
preserves pass summaries, SHA-256 identities, and the post-patch transport-cleanup regression
results; the original JSON outputs named under `/tmp` are not checked in.

Full logic analyzer finite HIL evidence report (2026-07-25):
`doc/testing/results/2026-07-25-logic-analyzer-finite-hil.md`

### WS SINGLE 1MHz Continuous (`/tmp/linkr-final-ws-1mhz.json`)
Overall pass, 4,995,072 samples, 998,963.5 samples/s effective rate,
zero sample-index gaps, zero disconnects, STOP response received, immediate restart
and HTTP health confirmed.

### GP10 UART Trigger Validation (`/tmp/linkr-final-gp10-*.json`)
Six isolated bounded captures at 1 MHz and 2 MHz, rising/falling/either trigger types,
each with 4096 samples and trigger offset 0. All six passes:
`/tmp/linkr-final-gp10-1mhz-rising.json`, `/tmp/linkr-final-gp10-1mhz-falling.json`,
`/tmp/linkr-final-gp10-1mhz-either.json`, `/tmp/linkr-final-gp10-2mhz-rising.json`,
`/tmp/linkr-final-gp10-2mhz-falling.json`, `/tmp/linkr-final-gp10-2mhz-either.json`.

### TCP Bounded 100kHz (`/tmp/linkr-final-tcp-bounded.json`)
All three modes (SINGLE, FAST8, WIDE11) received exactly 65535 samples with zero
sample-index gaps, stop response received, immediate restart and HTTP health confirmed.

These results demonstrate the final implementation against the reference smoke test
criteria. They are not projections or estimates.

## 参考来源

- `README.md`
- `AGENTS.md`
- `skills/radxa-linkr-debugger/SKILL.md`
