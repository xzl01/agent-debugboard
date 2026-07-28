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
scripts/build-firmware.sh
```

### 烧录

RP2350 初次安装或恢复必须使用由 MCUboot 和应用 `zephyr.signed.hex` 合并得到的
`radxa-linkr-debugger-rp2350.uf2`。构建系统自动生成此文件到
`build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2`（同时也会把
`zephyr.uf2` 替换为 signed hex 版本，使其可被 MCUboot 启动）。

**警告**：直接使用未签名的 `zephyr.hex` 生成的 UF2（无 MCUboot 头）会导致 MCUboot
拒绝启动，板子陷入无法响应 USB 的状态，需要物理 BOOTSEL 恢复。构建系统已自动将
`zephyr.uf2` 替换为 signed hex 版本以防止此问题。

```sh
picotool load -v -x radxa-linkr-debugger-rp2350.uf2
```

或使用 RPI-RP2 拖拽方式：

```text
radxa-linkr-debugger-rp2350.uf2
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

# 5v_ws is board-internal: list/status must omit it and direct CLI access must fail locally.
timeout 5s radxa-linkr-debuggerctl --json power list > /tmp/linkr-power-list.json
! grep -q '"name":"5v_ws"' /tmp/linkr-power-list.json
! timeout 5s radxa-linkr-debuggerctl --json power get 5v_ws
! timeout 5s radxa-linkr-debuggerctl --json power set 5v_ws off

# The raw API compatibility entry remains available for low-level diagnostics.
timeout 5s curl -fsS http://172.29.203.1/api/v1/power/5v_ws

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

### 8b. Logic Analyzer (PIO2+DMA)

Logic analyzer uses RP2350 PIO2+DMA for high-speed single-shot capture.
50MHz and 125MHz are very short single-shot bursts; the firmware does not
claim sustained streaming at those rates. Safe pins: GP7, GP8, GP9,
GP10-GP20, GP29. Sample rate range: 1,000,000 - 125,000,000 Hz.

#### 8b.1 Free-run at 1MHz

```sh
timeout 5s curl -fsS -X POST -H 'Content-Type: application/json' \
  --data '{"selected_pins":[13,15],"sample_rate_hz":1000000,"pre_samples":0,"post_samples":512,"trigger":"none"}' \
  http://172.29.203.1/api/v1/logic-analyzer
sleep 1
timeout 5s curl -fsS http://172.29.203.1/api/v1/logic-analyzer
```

验证 state 转换：`idle` -> `armed` -> `capturing` -> `done`，且 arm response 包含
`requestedSampleRateHz`、`actualSampleRateHz`、`samplePeriodPs`、`backend`。

#### 8b.2 Free-run at 25MHz

```sh
timeout 5s curl -fsS -X POST -H 'Content-Type: application/json' \
  --data '{"selected_pins":[13,15],"sample_rate_hz":25000000,"pre_samples":0,"post_samples":512,"trigger":"none"}' \
  http://172.29.203.1/api/v1/logic-analyzer
sleep 1
timeout 5s curl -fsS http://172.29.203.1/api/v1/logic-analyzer/capture
```

验证 `actualSampleRateHz` 接近 25MHz（25000000），`samplePeriodPs` 约为 40000ps / 40ns，
实际值因 PIO clock divider 量化可能有小幅偏差。

#### 8b.3 Free-run at 50MHz

```sh
timeout 5s curl -fsS -X POST -H 'Content-Type: application/json' \
  --data '{"selected_pins":[13,15],"sample_rate_hz":50000000,"pre_samples":0,"post_samples":512,"trigger":"none"}' \
  http://172.29.203.1/api/v1/logic-analyzer
sleep 1
timeout 5s curl -fsS http://172.29.203.1/api/v1/logic-analyzer/capture
```

50MHz 是短时单次 burst；确认 capture 完成且 `actualSampleRateHz` 接近 50MHz。

#### 8b.4 Free-run at 125MHz

```sh
timeout 5s curl -fsS -X POST -H 'Content-Type: application/json' \
  --data '{"selected_pins":[13,15],"sample_rate_hz":125000000,"pre_samples":0,"post_samples":512,"trigger":"none"}' \
  http://172.29.203.1/api/v1/logic-analyzer
sleep 1
timeout 5s curl -fsS http://172.29.203.1/api/v1/logic-analyzer/capture
```

125MHz 是短时单次 burst；确认 capture 完成且 `actualSampleRateHz` 接近 125MHz。

#### 8b.5 Edge trigger (rising)

连接信号源到引脚后执行：

```sh
timeout 5s curl -fsS -X POST -H 'Content-Type: application/json' \
  --data '{"selected_pins":[13],"sample_rate_hz":1000000,"pre_samples":0,"post_samples":256,"trigger":"rising"}' \
  http://172.29.203.1/api/v1/logic-analyzer
```

验证 capture response 的 `config.triggerType` 为 `rising`。

#### 8b.5a Either trigger relocation regression

使用 `either` 触发模式时，PIO 程序可能被装载到非零 offset。HIL 中至少执行一次
`trigger:"either"` arm/capture 流程，确认信号源任一边沿可触发并返回完整 capture；host
unit test 同时覆盖非零 offset 下的 absolute jump target 编码。

#### 8b.5b Capture snapshot consistency regression

读取 `/api/v1/logic-analyzer/capture` 时，固件必须返回同一代 capture 的 metadata 和
样本数据。HIL 中对同一 completed capture 连续 GET 两次，确认 `sampleCount`、
`triggerIndex`、rate metadata 和样本数组长度一致，且没有 500 `capture_too_large` 或截断 JSON。

#### 8b.6 Cancel/arm-busy

```sh
# 先 arm
timeout 5s curl -fsS -X POST -H 'Content-Type: application/json' \
  --data '{"selected_pins":[13],"sample_rate_hz":1000000,"pre_samples":0,"post_samples":512,"trigger":"none"}' \
  http://172.29.203.1/api/v1/logic-analyzer
# 重复 arm 应返回 409 already_armed
timeout 5s curl -fsS -X POST -H 'Content-Type: application/json' \
  --data '{"selected_pins":[13],"sample_rate_hz":1000000,"pre_samples":0,"post_samples":512,"trigger":"none"}' \
  http://172.29.203.1/api/v1/logic-analyzer
# 验证返回 HTTP 409 和 error.code: "already_armed"
# 取消 capture
timeout 5s curl -fsS -X DELETE http://172.29.203.1/api/v1/logic-analyzer
# 再次 arm 应成功
timeout 5s curl -fsS -X POST -H 'Content-Type: application/json' \
  --data '{"selected_pins":[13],"sample_rate_hz":1000000,"pre_samples":0,"post_samples":512,"trigger":"none"}' \
  http://172.29.203.1/api/v1/logic-analyzer
```

#### 8b.7 Pre-trigger arm/release cycle

pre_samples > 0 且 edge trigger（≤25 MHz）应成功 arm。由于 PIO `in pins`
指令需要 pad function 为 PIO peripheral（GPIO API PUT 切换到 SIO 会断开
PIO 输入），HIL 中无法通过 GPIO 环回验证真实触发；只验证 arm/release 状态转换。

```sh
# pre_samples > 0 + rising trigger 应 arm 成功
timeout 5s curl -fsS -X POST -H 'Content-Type: application/json' \
  --data '{"selected_pins":[13],"sample_rate_hz":1000000,"pre_samples":32,"post_samples":32,"trigger":"rising"}' \
  http://172.29.203.1/api/v1/logic-analyzer
# 验证返回 ok:true, action:armed
# release
timeout 5s curl -fsS -X DELETE http://172.29.203.1/api/v1/logic-analyzer
# 再次 arm 应成功（无 EBUSY 卡死）
timeout 5s curl -fsS -X POST -H 'Content-Type: application/json' \
  --data '{"selected_pins":[13],"sample_rate_hz":1000000,"pre_samples":32,"post_samples":32,"trigger":"rising"}' \
  http://172.29.203.1/api/v1/logic-analyzer
timeout 5s curl -fsS -X DELETE http://172.29.203.1/api/v1/logic-analyzer
```

#### 8b.7a Pre-trigger rate cap

pre_samples > 0 且 rate > 25 MHz 应返回 HTTP 400 invalid_config。

```sh
timeout 5s curl -fsS -X POST -H 'Content-Type: application/json' \
  --data '{"selected_pins":[13],"sample_rate_hz":25000001,"pre_samples":10,"post_samples":512,"trigger":"rising"}' \
  http://172.29.203.1/api/v1/logic-analyzer
# 验证返回 HTTP 400 和 error.code: "invalid_config"
```

#### 8b.8 SCPI-over-WebSocket 桥

`/api/v1/logic-analyzer/stream` REST 端点与 `logic-chunk` WebSocket 消息已
移除，浏览器统一走与 PulseView 相同的示波器协议：`ws://<board>/api/v1/scpi`
（文本或二进制帧发命令，二进制帧收响应，流式语义拼接后按行与 IEEE488 块
解析）。TCP（tcp-raw 端口 80）与 WebSocket 两种传输互斥，同一时间只允许
一个 SCPI 会话。

WS 桥功能回归：用 WebSocket 客户端连接 `/api/v1/scpi`，发送 `*IDN?` 应收
到 DS1102D 身份串；依次发送 `:TIM:SCAL 0.00002`、`:TRIG:EDGE:SOUR D0`、
`:TRIG:EDGE:SLOP NEG`、`:RUN` 后 `:WAV:DATA? DIG`，应收到 `#41200` 头加
1200 字节帧，D0 下降沿位于样本 300（GP10 注入 115200 'U'）；连续请求 6
帧全部成功，关闭后 GET /api/v1/logic-analyzer 显示 `state: "idle"`（会话
收尾不得泄漏 LA 所有权）。

单会话互斥回归：一个 SCPI-over-WS 会话存活期间，第二个 WS 或 TCP SCPI 连
接应被拒绝/关闭，首个会话结束后新连接应立即可用。

浏览器端到端回归（Playwright 或手动）：右侧工作区 **Logic Analyzer** 页
签选择引脚后点 **Arm capture**，应显示 "Captured 600 samples"（scope 帧
固定 600 样本，触发时 300 前 + 300 后）与波形；点 **Stream** 应出现
"streaming"徽章、持续增长的样本计数与实时波形（svg path）；点
**Stop stream** 徽章消失，控制台不应出现 `Close received after close`。
连续 3 次启停循环均应正常。

速率边界回归：流式帧循环（trigger none）在 1 MHz 下连续运行 30 秒板子
不应复位；UI 在 >25 MHz 时禁用流式按钮（帧循环在上位机侧节流，固件不再
有独立的流式速率上限）。

#### 8b.9 HTTP/watchdog responsiveness during capture

 在 capture 期间（armed 或 capturing 状态）轮询 HTTP 和 watchdog：

 ```sh
 timeout 5s curl -fsS -X POST -H 'Content-Type: application/json' \
   --data '{"selected_pins":[13],"sample_rate_hz":1000000,"pre_samples":0,"post_samples":512,"trigger":"none"}' \
   http://172.29.203.1/api/v1/logic-analyzer
 # capture 期间验证 HTTP API 仍响应
 timeout 5s curl -fsS http://172.29.203.1/api/v1/status
 timeout 5s curl -fsS http://172.29.203.1/api/v1/watchdog
 # 验证返回正常 JSON（不是超时或错误）
 ```

#### 8b.10 HTTP fragment POST for logic analyzer (TCP fragmentation regression)

 验证逻辑分析仪 POST 正确处理 TCP 分片请求体，使用 Python `http.client` 强制将
 Content-Length 请求体分成多个 sendall 调用发送（模拟浏览器行为）：

 ```python
 import http.client
 import json
 import time

 BOARD = "172.29.203.1"
 PORT = 80
 PATH = "/api/v1/logic-analyzer"

 BODY = (
     '{"selected_pins":[13,15],'
     '"sample_rate_hz":1000000,'
     '"pre_samples":0,'
     '"post_samples":512,'
     '"trigger":"none"}'
 )
 CL = len(BODY)
 def release():
     conn = http.client.HTTPConnection(BOARD, PORT, timeout=5)
     try:
         conn.request("DELETE", PATH)
         response = conn.getresponse()
         response.read()
         assert response.status == 200, f"DELETE cleanup returned {response.status}"
     finally:
         conn.close()

 # Clear stale state first so repeated runs cannot fail with already_armed.
 release()

 conn = http.client.HTTPConnection(BOARD, PORT, timeout=5)
 try:
     conn.putrequest("POST", PATH)
     conn.putheader("Host", BOARD)
     conn.putheader("Content-Type", "application/json")
     conn.putheader("Content-Length", str(CL))
     conn.putheader("Connection", "close")
     conn.endheaders()

     # Force the request body through three separate TCP writes.
     chunk_size = (CL + 2) // 3
     for i in range(3):
         chunk = BODY[i * chunk_size : (i + 1) * chunk_size]
         if chunk:
             conn.sock.sendall(chunk.encode())
         time.sleep(0.05)
     response = conn.getresponse()
     payload = json.loads(response.read())
     assert response.status == 200, f"Expected HTTP 200, got {response.status}: {payload}"
     assert payload.get("ok") is True, f"Expected ok:true, got: {payload}"
     print(f"Fragmented POST succeeded: {payload}")
 finally:
     conn.close()
     release()
 ```

 所有网络操作设置 5 秒超时，`http.client` 会正确处理 Content-Length 和 chunked
 响应。
 清理步骤释放 capture 状态，使重复运行安全。

#### 8b.11 Browser WASM decoder network and annotation checks

 验证浏览器可以加载 WASM decoder 资源。使用 GET（不用 HEAD）直接请求，
 捕获响应头并验证 Content-Type 和 Content-Encoding：

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

 验证浏览器控制台没有 WASM 加载错误，且 annotation 可以正确渲染在 waveform view 中。
 此测试需要在板载页面的 Terminal workspace 中完成实际的 capture 和 decode 流程。

### 8c. PulseView 原生接入（rigol-ds 仿真）

固件在端口 80 上以首字节分流复用 HTTP 与 Rigol DS1102D SCPI 仿真
（rigol-ds 驱动，`tcp-raw`）。验证使用 sigrok-cli，配合 GP10（D0，J16_PIN1）上的
115200 波特 'U' 连续注入（start bit 提供周期性下降沿）。

前提：`nix-shell -p sigrok-cli`，注入脚本写 `/dev/ttyACM1`（115200 连续
'U'）。下述命令中 `$SIG` 为 sigrok-cli 路径。

#### 8c.1 扫描识别

```sh
$SIG -d rigol-ds:conn=tcp-raw/172.29.203.1/80 --scan
# 应识别出 "Rigol Technologies DS1102D"，18 通道（CH1/CH2 + D0-D15）
```

#### 8c.2 快时基硬件预触发（≤25 MHz）

```sh
$SIG -d rigol-ds:conn=tcp-raw/172.29.203.1/80 \
  --config timebase='20 us' --config triggersource=D0 --config triggerslope=f \
  --frames 1 --channels D0 -o /tmp/cap.sr
$SIG -i /tmp/cap.sr -O csv
# 验证：600 样本，D0 下降沿精确位于样本 300（pre=300/post=212），
# 波形呈现 115200 'U' 的周期位型（约 43 样本一对同向边沿）
```

#### 8c.3 慢时基 :TRIG:STAT? 轮询路径（≥50 ms/div）

```sh
$SIG -d rigol-ds:conn=tcp-raw/172.29.203.1/80 \
  --config timebase='50 ms' --config triggersource=D0 --config triggerslope=f \
  --frames 3 --channels D0 -o /tmp/cap.sr
# 验证：3 帧均返回（驱动两阶段等待 RUN->TD 不超时），每帧边沿位于样本 300
```

#### 8c.4 AUTO 回退（无有效触发源）

```sh
$SIG -d rigol-ds:conn=tcp-raw/172.29.203.1/80 \
  --config timebase='20 us' --frames 1 --channels D0 -o /tmp/cap.sr
# 验证：默认触发源 D0 无边沿时约 100ms 后仍返回一帧非触发数据，
# 不应出现采集超时或连接断开
```

#### 8c.5 >25 MHz 突发触发

```sh
$SIG -d rigol-ds:conn=tcp-raw/172.29.203.1/80 \
  --config timebase='1 us' --config triggersource=D0 --config triggerslope=f \
  --frames 1 --channels D0 -o /tmp/cap.sr
# 验证：返回 600 样本（单次 burst 512 + 软件边沿对齐到 300 并填充）；
# 无边沿窗口时同样返回数据（AUTO），不得报错断开
```

#### 8c.6 GP29 模拟通道（CH1）

```sh
$SIG -d rigol-ds:conn=tcp-raw/172.29.203.1/80 \
  --config timebase='1 ms' --frames 1 --channels CH1 -o /tmp/cap.sr
$SIG -i /tmp/cap.sr -O csv
# 验证：600 个模拟电压样本（浮空引脚应为缓变中电平），
# 板端日志不得出现 "GP29 ADC setup failed"
```

#### 8c.7 端口 80 复用回归

SCPI 会话期间与之后，浏览器/CLI 路径（HTTP 经泵转发到 8080）必须正常：
`curl http://172.29.203.1/api/v1/status` 返回 200；SCPI-over-WebSocket
（`/api/v1/scpi`）与 TCP SCPI 会话在各自关闭后均照常工作；全部 sigrok
测试结束后 GET /api/v1/logic-analyzer 应显示 `state: "idle"`（rigol 会话
不得泄漏 LA 所有权）。

### 8d. 深采集（SPI flash，厂商 SCPI）

深采集把采样写入 2 MB storage flash 分区（数字 ≤25 kHz、模拟 ≤10 kHz，
GP10 注入 115200 'U' 用于触发验证）。

#### 8d.1 触发深采集全流程

用 WS 或 TCP SCPI 客户端执行：

```text
:TRIG:EDGE:SOUR D0
:TRIG:EDGE:SLOP NEG
:LINKR:DEEP:START 25000 2
# 轮询 :LINKR:DEEP:STATUS? 直到 DONE（PREPARING 阶段约 1-10 秒）
:LINKR:DEEP:STATUS?
# 期望：DONE <written> <trig_idx> 25000 0，触发时 written ≈ trig_idx + 25000
:LINKR:DEEP:DATA? 0 8192
# 期望：8192×2 字节块，D0 位呈现 115200 'U' 周期边沿
```

验证：同一窗口重复 DATA? 返回完全相同（flash 持久化）；`written` 之后的
区域返回统一的末样本填充；STATUS 中 `dropped` 为 0；LA 状态保持 idle
（深采集不占用 LA）。

#### 8d.2 无信号满窗口

不给触发源信号时 START 应在 `rate × duration` 满窗口后 DONE
（如 25000 Hz × 2 s = 50000 样本）。

#### 8d.3 浏览器 Deep 按钮 E2E

Logic Analyzer 页签选择 GP10 并设 falling 触发后点 **Deep**：应依次显示
PREPARING/CAPTURING/download 进度，完成后徽标显示 "deep <rate> <count>
samples" 与 trigger@ 位置，实时波形出现且可窗口解码，CSV/.sr 导出可用
（导出文件应包含与徽标一致的样本数）。

### 8e. BeagleLogic 仿真（TCP 5555，无限连续视图）

固件在 TCP 5555 端口仿真 BeagleLogic TCP 协议（文本命令 + 裸样本流，
`-d beaglelogic:conn=tcp-raw/172.29.203.1/5555`）。GP10（P8_45，通道 0）
注入 115200 'U'。

#### 8e.1 扫描与协商

```sh
sigrok-cli -d beaglelogic:conn=tcp-raw/172.29.203.1/5555 --scan
# 期望：beaglelogic - BeagleLogic 1.0 with 14 channels
```

#### 8e.2 ONE_SHOT 采集

```sh
sigrok-cli -d beaglelogic:conn=tcp-raw/172.29.203.1/5555 \
  --config samplerate=100000 --samples 200000 --channels P8_45 -o /tmp/bl.sr
sigrok-cli -i /tmp/bl.sr -O csv
# 期望：200000 样本，P8_45 呈现 115200 'U' 的 2-3 样本交替边沿模式
```

#### 8e.3 连续流式持续速率（原始 socket）

手工连接 5555：`version`→`samplerate <N>`→`get`，计 5 秒接收字节数：
- 8-bit @100kHz：应 ≥95 kHz
- 16-bit @100kHz：应 ≥95 kHz；@150kHz：应 ≥140 kHz
- @204800：best-effort（≥100 kHz，允许 dropped）
关闭应干净（`close` 后 LA 状态 idle，下一次连接立即可用）。

#### 8e.4 单会话互斥与清理

一路流式会话存活期间第二连接应被拒绝/不可用；首连接关闭后（即使客户端
异常断开无 `close`），新连接应在秒级内可用，且不得遗留 LA 占用
（GET /api/v1/logic-analyzer 显示 idle）。

### 9. BOOTSEL 进入

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
FLASH_UF2=radxa-linkr-debugger-rp2350.uf2
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
  --uf2 radxa-linkr-debugger-rp2350.uf2 \
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

## 参考来源

- `README.md`
- `AGENTS.md`
- `skills/radxa-linkr-debugger/SKILL.md`
