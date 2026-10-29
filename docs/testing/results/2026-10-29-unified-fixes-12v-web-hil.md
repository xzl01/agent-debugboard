# 2026-10-29 统一修复：12V 受限板载 Web HIL

## 范围与身份

本报告只记录第二阶段板载 Web UI 的真实 Chromium / MCU HTTP / WS 验证，
不替代核心阶段报告，也**不表示完整规范 HIL、全部 canonical 矩阵或可发布**。
执行前重新读取了 `AGENTS.md` 和
[`hil-functional-test-spec.md`](../hil-functional-test-spec.md) 的本日单次 12V 例外。
本次硬件步骤结束后该例外已撤销，后续运行恢复原 20V 前置；执行时规则的副本
与哈希保存在核心证据根的 `authorization/`，不构成长期放宽。

- 唯一目标：USB serial `C0575D0BB2F6A330`，`http://10.123.5.149/`；
  主机 NCM `eth0=10.123.5.150/30`，未改网络设置。
- 本阶段未刷机、未 reboot；沿用核心阶段已完成的
  `5v_out=on → fixed 12V → INA226` 前置检查。开始时只读重新确认 PD 12V、
  5V 输出有效、watchdog healthy，uptime 高于核心阶段末值；后续未观察到重启。
- 核心阶段交接的 combined UF2 SHA-256：
  `3ae4d32ccd80dbff836b3dc1c414f8d8174e5d7e1252e931baef4d1a5693d93c`；
  本阶段没有重新刷写此文件。
- Chromium 实际加载板端 `/assets/app.js`；通过 CDP 读取该次 HTTP 响应正文，
  SHA-256 为
  `6983482f34da72c8244c775bdb0e940fff94c4bdecfdfe783593dcef78d0ff49`。
  无本地 dev server、无 mock 响应、无伪造 WS 帧、无 React state 注入。
- PD/高率主阶段独立浏览器：`Chrome/154.0.8037.97`，PID `1868353`，随机 CDP 端口
  `35689`，page ID `F28F194083235DB99016F4993D20427A`；新 profile 为
  `/tmp/opencode/uncommitted-integrated-hil-20261029/browser/profile`。
  未复用旧 QA profile，未查询、关闭或导航其他浏览器的页面。

外部证据根目录（下文路径相对此目录）：

```text
/tmp/opencode/uncommitted-integrated-hil-20261029/browser/
```

脚本、原始 CDP 事件、解析帧、独立解压样本、截图和逐文件 SHA-256 清单分别位于
`scripts/`、`logs/`、`screenshots/`、`SHA256SUMS`。这些是本次 live HIL 的外部
证据脚本，不是新增仓库离线测试入口；本阶段没有修改生产源码。

## 逐项结果

| 项目 | 结果 | 实机证据与边界 |
| --- | --- | --- |
| 板载 bundle 身份 | PASS | `logs/bundle-identity.json`，正文另存 `hashes/board-app-loaded.js` |
| PD 能力与请求/实测分离 | PASS | 实际 UI 显示 fixed 5V/3A、9V/3A、12V/2.5A，PPS 3.3–11V/3A；请求 12.0V 与 ADC 源侧实测 11.656V 在独立区域显示 |
| 真实 UI fixed 12V Apply 一次 | PASS | `logs/pd-apply-network.json`：恰好一个 `PUT /api/v1/pd`，body `{"mode":0,"voltage_mv":12000}`，HTTP 200 / `ok:true`；回读仍为 12V，5V INA226 正常 |
| F：旧自动请求挂起、手动成功及自动轮询恢复 | PASS：浏览器 fault-injection + real-board 恢复 | 主阶段完成后单独追加；仅暂停 owned 页面一次真实 `/status` 响应，10.000215s timeout、12.000559s 自动恢复，同 document 无 reload；非设备自然故障，详情见末节 |
| GPIO allowlist / J4 警示 | PASS | GET catalog 中 GP12–16 全部 input；UI 只允许这五个 LA 输入，准确提示 FAST8 即使只选 GP12–16 也会接管 GP10/11、暂停 J4；没有写 GPIO |
| 100MHz FAST8 capacity Stream | PASS | GP12+GP13、NONE、pre=0、post=0；100,000 点，49 DATA 帧，0 gaps / ERROR / OVERRUN，STOPPED index=100000 |
| 125MHz FAST8 capacity Stream | PASS | 独立显式用例，非失败重放；同为 100,000 点 / 49 DATA 帧 / 0 gaps / ERROR / OVERRUN；实际率 125.081MHz |
| 自动停止最终只读流快照 | PASS | 两个高率用例都保留 GP12/13 waveform、100.0K 总数、原请求/实际率及“缓冲区满自动停止，并非错误”提示；不是 live badge |
| 快照隔离与浏览/Clear | PASS | 改下一次配置为 GP13+GP14 / 100kHz，不改变旧快照 channels、rate 或 notice；1024 点窗口、取消 Follow、slider 到首段、恢复 Follow 到末段均可用；Clear 清除图、rate、notice |
| 防止误入普通 capture 操作 | PASS | 流快照没有 CSV / PulseView `.sr` 入口；Decode UART disabled，旧 annotations 隐藏，说明文字明确快照不作为普通采集解码/导出 |
| GP13 100kHz 短流手动 Stop | PASS，有限范围 | 仅启动一次；WS START_REQ→STOP_REQ 为 1.774084s；手动只读快照、disabled Decode、无普通导出、Clear 均通过；样本边界见下文 |
| J4 与设备健康恢复 | PASS（主阶段） | 每个高率采集后 J4 ready / admission unfrozen；主阶段结束为 115200 8N1、generation=1，UART TX bytes=0；F 补充时另有外部客户端，不能沿用全局零 sink/TX 结论 |
| 资源释放及 HUB 运行态恢复 | PASS，基线范围有限 | 只清理本次浏览器与连接；单次将 `5v_usb_hub` 恢复 off；GPIO direction 只确认本 Web 阶段前后不变，不代表整轮运行态恢复。GP7 相对最初状态的 output→input 差异保留，见下文 |
| unknown PD / `voltage_mv=0` 冷启动 | NOT TESTED（本阶段） | 仅有协调阶段所述离线 29 项覆盖；本阶段 PD 已知为 12V，未伪造 unknown，未为此重启 |
| fixed 5/9V、15/20V、PPS 设置及 20V 输出 | NOT TESTED | 只展示已公布能力；未切 5/9/PPS，未请求 15/20V，未启用 20V 输出 |
| 小型 bounded / CSV / `.sr` / 有效协议解码 | NOT TESTED | 为控制本次范围未追加采集；静态输入不能证明 UART 协议正确 |
| 五路 100kHz 持续、WIDE11、SINGLE 高率、TCP/performance 矩阵 | NOT TESTED | 未重跑已知不稳定的五路持续场景，也未扩展矩阵或进行外部高熵/引脚映射验收 |

本轮限定产品用例未观察到功能失败；下文明确保留了 harness 的失败与修正，
不把它们隐去或称为硬件重试通过。

## 原始 WS 与样本核验

控件操作使用真实 DOM select/input/change/contextmenu/click 事件。
所有 Sigrok 帧来自 Chromium `Network.webSocketFrameSent/Received`，
保存完整 `raw_hex` / `payload_hex`，没有向接收路径注入数据。

| 用例 | 请求 / CONFIG_RESP 实际率 | mask | HELLO flags | DATA / 样本 | 压缩 | 终态 |
| --- | --- | --- | --- | --- | --- | --- |
| FAST8 GP12+13 | 100,000,000 / 100,000,000 Hz | `0x000c` | `3`（CONFIG_V2 + GENERIC_PACKED_BURST） | 49 / 100000 | BIT_PACK_RLE (`3`) | RUNNING → STOPPED，index=100000，清理收到 STOP_RESP |
| FAST8 GP12+13 | 125,000,000 / 125,081,000 Hz | `0x000c` | `3` | 49 / 100000 | BIT_PACK_RLE (`3`) | RUNNING → STOPPED，index=100000，清理收到 STOP_RESP |
| 单 GP13 短流 | 100,000 / 100,000 Hz | `0x0008` | `3` | 10 / 163840（包含 Stop 后 drain） | SINGLE_BITS_RLE (`5`) | 手动 STOP_REQ、STOP_RESP、STOPPED |

高率两例均记录完整 HELLO/CAPS/CONFIG/CONFIG_RESP/START/DATA/EVENT/STOP 交换；
wire CONFIG mode=1（FAST8）、trigger=NONE、pre=0、post=0。每例只发送一次 START，
DATA 索引从 0 连续至 100000，没有 ERROR frame 或 OVERRUN/ERROR event。
`post=0` 使用协议 CONFIG v1 的 stream sentinel 是正常路径，不能误称发了 CONFIG_V2。

独立离线解压实际 WS payload 后，两例各精确得到 100000 个 dense 样本，观测值全部为 0，
样本文件 SHA-256 均为
`9192c25b734fcbadbe32dadc28089c60db0e39f90cc20ce2e5733f57261acc0c`。
这只证明本次静态/低变化输入的容量 burst、传输与 UI 最终快照；
不证明高熵持续吞吐、外部信号映射、采样时序精度或有效 UART 数据。

### 手动 Stop 的精确边界

WS 时间戳：START_REQ `2111802.200357`，START_RESP `2111802.299388`，
STOP_REQ `2111803.974441`，STOP_RESP `2111804.356331`。
实际请求流区间 1.774084 秒；不是五路持续测试。

- 停止前到达的 5 帧共 **81920** 点，UI 保留为 **81.9K** 的只读快照。
- STOP_REQ 后又有 5 个 drain DATA 帧，共 81920 点；网络记录总计 **163840** 点，
  索引连续、无协议错误。取消后的流快照不再吸收这些尾部帧。
- 因此本报告**不宣称手动快照包含全部 wire drain 样本**。UI 81920 与网络 163840
  是两个明确不同的统计边界；两个自动停止用例没有这个差异。
- 独立解压网络 163840 点均为 0；文件 hash 为
  `6cdd259c8ecbe61fbc369f3293c1961541386954a223b17a37899d7fd9ad42da`。

主证据：`logs/fast8-{100,125}mhz-{result,all-frames,cdp-events}.json`、
`logs/manual-stream-{all-frames,cdp-events,independent-result}.json`、
`logs/independent-ws-sample-analysis.json` 和 `logs/*-decoded-dense-samples.bin`。

## 截图与视觉核查

直接查看了以下完整路径的真实截图：

- `/tmp/opencode/uncommitted-integrated-hil-20261029/browser/screenshots/pd-fixed12-applied.png`
- `/tmp/opencode/uncommitted-integrated-hil-20261029/browser/screenshots/fast8-100mhz-final-snapshot.png`
- `/tmp/opencode/uncommitted-integrated-hil-20261029/browser/screenshots/fast8-125mhz-final-snapshot.png`
- `/tmp/opencode/uncommitted-integrated-hil-20261029/browser/screenshots/manual-stream-final-snapshot.png`

PD 请求/实测区分清楚；100/125MHz 快照标题、只读状态、100.0K、GP12/13、rate
及自动停止提示可读；125MHz 实际率显示舍入为 125.1MHz，请求仍为 125.0MHz。
手动截图中 disabled Decode 与“不作为普通采集结果解码或导出”的说明完整可见。
100/125MHz 的 full-page 截图保留了滚动时 sticky 页头穿插的截图现象，
不能据此声称所有视口均无覆盖；手动截图用 scroll=0 / 1600px 高视口补充完整视图。
未做移动端/全尺寸响应式矩阵。截图本身不承担样本连续性证明，样本以原始 WS 为准。

独立视觉复核还发现一项非阻断的文案可用性限制：波形下方的
`logic-analyzer-config-preview` 显示下一次 bounded capture 的配置草稿（65.5K
及其预计时长），没有明确标注“计划配置”，容易与上方实际 Stream 快照的
100.0K／81.9K 混淆。该摘要来自未来配置，不是对真实流样本的统计；本轮不将
其可读性记为通过，也没有为此继续修改 UI 或重刷固件。

## Harness 失败与诚实边界

- 只读探测过已废弃的 `/api/v1/adc`、`/capture`、`/logic-analyzer` 路径，返回 404；
  对无 ID 的 `GET /live-sessions` 返回 400。后续使用实际 `/adc/read` 和 Sigrok WS。
  不把这些废弃 REST 路径当作采集状态证明。开始时资源空闲依据为核心 lane 明确释放，
  辅以 J4 ready / admission unfrozen / 零 sinks；随后真实 START 正常接受。
- 100MHz 在真正 START 前，临时 harness 的 select selector 和“尚未握手时能力提示”
  断言各失败一次；没有发送 CONFIG/START，原记录保存在
  `logs/prestart-selector-*`、`logs/prestart-capability-*`。修正后实机 100MHz 只采集一次。
- 手动流的第一份 harness 把 HELLO/CAPS/CONFIG 建连时间也计入了 `<=2s` 断言，
  得到 2.194124s 并报错；Stop 已正常发出。保留 `logs/manual-stream-result.json`
  原失败，**没有重新启动流**；随后只读取原始 WS 时间戳与保留 UI，
  在 `logs/manual-stream-independent-result.json` 记录实际 START→STOP 1.774084s。
- 清理脚本首次因 Nix Chromium 的单字符串 argv 格式在 profile 身份断言处停止；
  修正参数解析后只完成同一 owned 浏览器清理。此前的 HUB restore 没有重放。

## HUB 偏差修正、GP7 运行态差异与最终状态

核心阶段原始 `initial-current-status.json` 的 `5v_usb_hub=off`，而 saved config
保存值为 on；刷机后 replay 导致运行态变为 on。因此“原 baseline 完全不变”并不准确。
本阶段按精确授权先 GET 当前状态，再**只发送一次**
`PUT /api/v1/power/5v_usb_hub {"state":"off"}`，HTTP 200 / `ok:true`，
随后 GET 回读 off；没有写 saved config。

另一个必须保留的整轮运行态差异是 **GP7 output→input**：
原始 `initial-current-status.json` 为 GP7 `direction=output,value=1`；
`initial-current-config.json` 中 `gpio/GP7.saved=null`、`selected=false`，未保存该 GPIO。
核心阶段 boot 后 GP7 为 input；本 Web 阶段开始与结束也均为 input，未写该 GPIO。
因此 saved values 保持不变**不等于最初 runtime 全部不变或全部已恢复**。
本阶段收尾授权只包括 HUB off；没有、也不会为了恢复基线而自动驱动 GP7 HIGH/LOW。
GP7 继续保持 input，其浮动输入读值不作为输出恢复证明。

PD/高率主阶段结束时状态（F 补充的全局连接观察另列于末节）：

```text
fixed PD 12V，last_error=0
5v_out=on，vdd_5v=on，5v_otg=on
5v_usb_hub=off（恢复最初运行态）
12v_out=off，20v_out=off
sd=target，tf_wp=writable
GP7=input（相对整轮最初 output 有差异，未恢复输出）；GP12–16 均 input
GPIO direction 仅与本 Web 阶段 preflight 相同，不宣称相对整轮最初全部不变
J4 ready，115200 8N1，generation=1，sink total=0，target UART TX bytes=0
```

restore 后 5V INA226 为 **5.0625V / 0.57975A**；最终清理后为
**5.0625V / 0.57625A**，均 `power_enabled=true`、`voltage_valid=true`。
OTG 的 on 是固件状态回读，不冒称为专用 INA226 电压测量；HUB 同样没有专用 INA226。
saved snapshot/selected/saved values 保持不变；本 Web 阶段 tasks revision/digests、
network、switches 前后相同。这些结论不抹去整轮 GP7 runtime 差异；
saved HUB 仍为 on，所以未来 reboot 仍可能按保存值重放，本阶段未改它。

三次采集都已 STOP/close；Clear 后仅导航自己的 page 到 `about:blank`，
再对独立 Chromium 执行 `Browser.close`。确认 owned PID 已退出、CDP 35689 端口关闭，
本阶段 session 1–4 的只读 GET 均为 404。没有清全局 sessions、没有操作其他客户端。
主阶段结束时 uptime 为 2454609ms。证据：`logs/restore-*`、
`logs/browser-cleanup.json`、`logs/owned-session-readback.json`、
`logs/cdp-port-closed.json`、`logs/final-state-validation.json`。

本阶段仅新增此报告与外部证据，未 stage / commit / push。20V 与未列出的 canonical
项目均不因本报告获得通过结论。

## F 补充：页面挂起请求后的真实板轮询恢复

高率核心完成并清理后，按补充授权执行**一次**页面 transport 故障注入。
这是 **browser fault-injection + real-board 恢复证据**，不是板卡自然断连或固件故障。
没有为 F 重跑 capture，没有硬件写入，没有重放 HUB restore，没有驱动 GP7。

### 独立实例与注入边界

- 新 profile：`browser/fault-polling/profile`；Chromium PID `2026707`，CDP `38409`，
  page ID `2A59B33F7B6052489DAEA50A15A5111E`。未重开主阶段 profile 或操作其他页面。
- 再次确认实际板载 app.js SHA-256 与上文一致。
- 实际 DOM 控件为 `自动=true`、`实时=false`，观测正常 `/status → /adc/read` HTTP
  周期后才启用拦截；J4 自身的 serial WS 不能作为 board telemetry 恢复的替代证据。
- CDP Fetch 只匹配本页面 `http://10.123.5.149/api/v1/status` 的 **Response stage**。
  第一个真实 HTTP 200 响应暂停在页面取得 body 之前；后续同类响应只用
  `Fetch.continueResponse {requestId}` 原样放行。
- 没有 `Fetch.fulfillRequest`、没有改 status/header/body、没有 fetch monkeypatch、
  没有 React state 修改、没有改应用 timer/时钟、没有设备或其他客户端网络故障注入。
  这是一例 response-before-body 挂起，不扩大为全部 status/ADC、headers/body 组合 HIL。

### 实际时间线（相对被挂起自动 status 请求）

| 时间 | 观测 |
| --- | --- |
| 0s | 自动 status 请求 `2026862.17`，板端已返回 HTTP 200，被本页面 Fetch 暂停 |
| +0.837521s | 只点击一次真实 Refresh，新 `/status` 请求开始 |
| +0.940309s | 手动刷新推进到真实 `/adc/read`，两者均成功且正文 `ok:true` |
| +10.000215s | 原挂起请求由应用 timeout 取消：`net::ERR_ABORTED`、`canceled=true` |
| +12.000559s | 下一次自动 `/status` 成功恢复（timeout 后约 2s） |
| +12.103157s | 对应自动 ADC 请求 |
| +14.183561s / +14.322481s | 再一轮真实自动 status / ADC 成功，无需人工操作 |

手动刷新成功后直到恢复观察结束，UI 始终 online，控制未变 stale/disabled，
PD 实测区仍显示 fresh 数值；旧 timeout 没有覆盖较新的成功。
`performance.timeOrigin` 不变，case 内无 page reload；测试结束后的 `about:blank`
导航仅为释放 owned 页面，不计作恢复手段。
截图 `fault-polling/screenshots/automatic-polling-recovered.png` 已直接视觉核查，
可见 online、请求 12V 与实测 11.686V；模式控件被抽屉遮挡，不能单凭此图证明
自动 HTTP 模式、freshness 或恢复时序。这些判断来自上述 DOM/CDP 原始记录；
本项未通过点击 Apply 验证。

主证据（相对证据根目录）：

- `fault-polling/logs/result.json`、`held-response.json`、`manual-refresh-success.json`
- `fault-polling/logs/real-board-success-bodies.json`、`observation-timeline.json`
- `fault-polling/logs/cdp-events.json`、`cdp-commands.json`
- `fault-polling/logs/cleanup.json`、`owned-scope-audit.json`
- `scripts/fault_polling.py`、`scripts/fault_polling_audit.py`

### F 收尾：保留外部客户端，不要求全局归零

owned PID 已退出，CDP 38409 已关闭，Fetch 已禁用；本页面只建立过 `/api/v1/ws/0`。
该连接只发送 `{"type":"subscribe","topic":"serial.j4"}`，没有 UART payload 或 binary TX；
板端 slot0 ingress `rx_bytes=0`、累计 connected=disconnected=2，确认 owned 连接已释放。
导航时 CDP 未保存旧 document 的 WebSocketClosed 事件，清理证明使用 owned 进程/端口
以及板端每 slot 断开计数，不伪称具有该事件。

F 原收尾脚本误要求**全局** sink total=0、历史 UART TX=0，因此在功能 PASS 后断言失败。
实际 GET 显示另一个 slot2 Web 客户端仍连接、该 slot ingress=4、全局 UART TX=4。
未保存该客户端的起始计数，不能断言其 4 bytes 发生在 F 前还是期间；不能算作本页面 TX，
也不能宣称 F 后全局零 TX。只读离线复核 own 请求/消息和每 slot 计数通过，
**没有重跑故障注入、没有断开/导航外部 slot2、没有全局 session 清理**。

F 前后 power、switch 和 GPIO direction 的已有只读响应一致：HUB 继续 off，
GP7 继续 input，5v_out/vdd_5v/OTG 保持 on，PD 12V；J4 ready。
此补充不改变前述 GP7 整轮 runtime 差异，也不扩大 saved-preserved 的含义。
