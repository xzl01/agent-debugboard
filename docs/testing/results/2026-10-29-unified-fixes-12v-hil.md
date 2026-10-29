# 2026-10-29 统一修复：12V 受限 core HIL

## 结论与范围

**Core limited HIL：PASS（带明确记录的运行态 delta）。完整 canonical HIL：NOT TESTED。发布就绪：不声明。**

核正边界：core 结束时 `5v_usb_hub` 仍为 `on`，与首轮 `initial-current-status.json` 的 `off` 不同；本报告不称运行态 baseline 已完全恢复。后续 browser lane 负责有界 `PUT hub=off` 收尾，本文不提前记录为已恢复。

本文只记录独占硬件写入 lane 的真实板卡核心结果。此次运行严格使用单次授权例外：每次启动、正常 reboot、BOOTSEL 恢复后，第一条 host hardware mutation 都是 `5v_out=on`，第二条都是 fixed PD **12V**，随后核验已启用 5V rail 的 INA226 电压、电流。未请求 20V、15V、PPS 或 `20v_out`。

本次所有硬件步骤结束后，临时 12V 例外已从 `AGENTS.md` 和规范撤销，后续运行
恢复原 20V 前置要求。执行时加载的例外规则及哈希保存在原始证据根的
`authorization/`，本报告不是后续测试的例外授权。

Browser/高频 LA 下一阶段由协调者安排，不在本文 PASS 范围内。原始证据根为：

```text
/tmp/opencode/uncommitted-integrated-hil-20261029
```

汇总结果见 `core-results.json`；最终只读核验见 `final-state-validation.json`。

## 目标、源码与产物身份

| 项目 | 实际值 |
| --- | --- |
| 唯一目标 serial | `C0575D0BB2F6A330` |
| USB 拓扑 | `1-1.4.2` |
| 应用 USB | `2fe3:db01`，Radxa Linkr Debugger |
| ROM BOOTSEL USB | `2e8a:000f`，每次均匹配同一 serial 后才刷写 |
| 板卡 URL / host 源地址 | `http://10.123.5.149` / `10.123.5.150/30`，NCM `eth0` |
| 控制 shell / J4 | by-id `...-if00` / `...-if02`；仅打开 if00，未向 if02 写字节 |
| 源码基线 | HEAD `364923dd505dad79f27d0628153b6c869e946a1b`，工作树 dirty |
| 固件运行时身份 | `production`，`0.3.0+0` |
| 固件 build-id | `v0.2.1-262-g364923dd505d-dirty`（`uname -a`）；`app build-version` 为 `v0.2.1-262-g364923dd505d` |
| combined UF2 | `build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2` |
| combined 大小 / SHA256 | `1720320` bytes / `3ae4d32ccd80dbff836b3dc1c414f8d8174e5d7e1252e931baef4d1a5693d93c` |
| 源码 CLI binary SHA256 | `149360bd4be87db59c60b2849549df4a62b7c6f9ad5425dd3ff7c444b40f3cd9` |

本 lane 未 rebuild firmware，使用协调者提供的上述 combined 产物。仅执行了源码 CLI 的：

```sh
cargo build --manifest-path cmd-ng/Cargo.toml
```

构建时间为 `2026-10-29T12:22:54.429879Z` 至 `12:23:03.858942Z`，exit 0。

Embedded Web 的实际 authoritative generated 源是：

```text
build/radxa_linkr_debugger/radxa_linkr_debugger/zephyr/web-dist/assets/app.js
SHA256=6983482f34da72c8244c775bdb0e940fff94c4bdecfdfe783593dcef78d0ff49
size=1351102
```

HTTP BOOTSEL 恢复后及最终 CDC 恢复后，板载 `/assets/app.js` 均与该 generated 源逐字节一致。普通 `web/dist/assets/app.js` 的 SHA256 为 `4d7fc5bf909970292fa308934599b12b7448bb2a889ab322ccbc62786c40abfd`，未用它代替 firmware production build 产物。

## 强制 12V preflight 时序

下表时间均为 UTC；`V5` / `I5` 来自已启用 `5v_out` 的 INA226，`ADC0` 是固件给出的 PD 源侧值。

| 阶段 | mutation 1：`5v_out=on` | mutation 2：fixed 12000mV | V5 / I5 | ADC0 | 结果 |
| --- | --- | --- | --- | --- | --- |
| 初始 | `11:38:51.877290`，HTTP 200 | `12:21:06.350522`，HTTP 200 | 5.061250V / 0.587000A | 11.674071V | PASS |
| HTTP combined 恢复后 | `12:22:25.807012`，HTTP 200 | `12:22:25.842235`，HTTP 200 | 5.081250V / 0.369750A | 11.667892V | PASS |
| CDC 正常 reboot 后 | `12:30:35.754150`，HTTP 200 | `12:30:35.769276`，HTTP 200 | 5.061250V / 0.650750A | 11.556710V | PASS |
| CDC combined 恢复后 | `12:31:36.402553`，HTTP 200 | `12:31:36.446055`，HTTP 200 | 5.082500V / 0.366000A | 11.680243V | PASS |

每次均先确认 source 当前 fixed PDO 包含 5000/9000/12000mV，再发出第二次 mutation；回读均为 `available=true`、`mode=0`、`voltage_mv=12000`。启动 replay 已使 `5v_out=on` 的情况没有被当作 host 首次动作，仍显式发送了第一 PUT。

初始阶段的本地校验器曾误查顶层 `state`，而实际 HTTP 200 响应在 `power_output.state=on`。脚本立即停止，未发送 PD；之后没有重放第一 PUT，只从第二 mutation 继续。原始 false-negative 保留在 `initial-preflight-summary.json`，完整续接证据在 `initial-continuation-preflight-summary.json`。

## Core 项目结果

### 1. HTTP BOOTSEL、combined 刷写、正常启动与身份

**PASS。**

- `POST /api/v1/bootloader` 返回 HTTP 200：`entering rp2350 BOOTSEL in 250 ms`。
- 同 serial ROM 在 `1-1.4.2` 枚举为 `2e8a:000f`，bus 1 / address 25，product `RP2350 Boot`。
- 仅执行一次：

```sh
picotool load -v -x build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2 \
  --bus 1 --address 25 --ser C0575D0BB2F6A330
```

- 时间 `12:21:58.122775Z` 至 `12:22:10.946459Z`，exit 0；两段 flash verify 均 `OK`，随后 picotool 报告应用启动。
- 应用同 serial 以 bus 1 / address 26 重新枚举，无残留 ROM；网络保持 `10.123.5.148/30`、板卡 `.149`、MAC `d6:1a:42:5c:5d:b0`。
- 首轮 post-flash preflight 通过，embedded app hash 与 generated 源一致。

证据：`http-bootsel-*`、`http-flash-*`、`after-http-flash-*`。

### 2. 刷后 5V / fixed 12V / INA226

**PASS。** 见上方 preflight 矩阵；没有以 replay 状态代替 host mutation，也没有请求任何其他电压档位。

### 3. 源码 CLI discovery/status/doctor、真实 NCM WS ADC 与清理

**PASS。**

所有普通命令均使用 `LINKR_SKIP_DESKTOP=1` 并移除 `LINKR_BOARD_URL`，让当前源码执行真实自动发现；未启动真实 Tray。

- `discover --json --timeout 500ms`：exit 0，恰好一台已验证设备，`eth0`、`10.123.5.150/30` 到 `http://10.123.5.149`。另一个 `/30` 候选 `Meta/198.18.0.2` 为 `transport_error`，不是第二台 Linkr。
- 自动发现后的 `--json status`、`--json doctor`：均 exit 0，`schema/ok/command` 正确，doctor `probe_ok=true`。
- 真实 recorder：

```sh
radxa-linkr-debuggerctl adc record \
  /tmp/opencode/uncommitted-integrated-hil-20261029/source-cli-adc-20-100.ndjson \
  20 --rate-hz 100
```

- exit 0，恰好 20 条 `telemetry-record`，每条八个 descriptor；`sample_sequence=1..20`，`device_t_mono_us=80925600..81116000`，`requested_rate_hz=100`，三个设备时间别名均为整数。
- socket 采样直接看到 `10.123.5.150 -> 10.123.5.149:80` 的 recorder 连接；结束后无 recorder 进程或其 owned established socket。CLI 的 close+DELETE 清理路径返回成功，POST 后 HTTP/watchdog 健康。
- `GET /api/v1/live-sessions` 不是列表 API，返回 400；没有把这个不支持的探测当成 session-list 证据，也没有 DELETE 其他客户端 session。

证据：`source-cli-build.log`、`source-cli-discover.*`、`source-cli-status.*`、`source-cli-doctor.*`、`source-cli-adc-*`。

### 4. HTTP body/连接生命周期 P1 外部回归

**PASS（仅限下列可观察外部行为）。内部 context 地址复用：NOT TESTED。**

受控 raw TCP 均绑定 `10.123.5.150`，每连接有界，所有 power body 只含 no-op `on`，未发送 power-off 反例：

- 4 轮：声明 15B、实际发送完整外观的 14B `{"state":"on"}`，形成 partial MORE 后断连；每个新连接的合法 `5v_out=on` 都返回 HTTP 200 和明确 `power_output.state=on`，随后 current/config 签名不变。
- 一次 1025B body（超过 1024B cap）返回 HTTP 413、`error.code=body_too_large`；新连接 no-op ON 恢复成功。
- 5 个 malformed bracket 用例均返回 HTTP 400、`ok=false`、`error.code=invalid_state`；每次电源、switch、GPIO 方向/output 值、PD 请求状态和持久配置签名均不变。
- 最终 INA226 为 5.063750V / 0.599250A，`voltage_valid=true`。

Zephyr client pool 的内部地址无法由外部 socket 观察，因此不宣称证明“同一 context address 被复用”；这里只声明上述有限新连接/断连/413/poison 恢复行为。

证据：`http-lifecycle-summary.json`（49 个事件）及 `http-lifecycle-run2.console.log`。早期本地长度断言失败发生在任何 partial 之前，原件另存为 `http-lifecycle-harness-length-failure-summary.json`，不是固件失败。

### 5. WS status/control、ADC、GPIO no-op 与 HTTP 一致性

**PASS。Capture ownership / bounded LA：NOT TESTED。**

一个受控 live session 内完成：

- `subscribe topic=live rate_hz=20` 成功，收到 status snapshot 和 3 个有效 ADC telemetry；每帧八路顺序正确，5V 电压/电流有效，ADC0 `role=pd_input` 且源侧约 12V。
- `power_set output=5v_out state=on` 返回 `ok=true`，后续 WS snapshot 与 HTTP power/switch 状态一致。
- 目录确认 GP5 原本为 input 后，仅发送 `gpio_set GP5 direction=input`；返回 `ok=true`，未驱动任何引脚 HIGH/LOW。
- HTTP before/after 的 power、switch、GPIO 方向/output 值和 fixed 12V 一致；status 中的配置摘要（`saved_count=9`、`pending_count=0`）未变。本项未读取逐项配置；全程 saved 值一致性另由 `initial-current-config.json` 与 `final-config.json` 的独立比较证明。
- WebSocket close 后固件自动释放 session；对同一 session 的后续 DELETE 返回 404 `unknown_session_id`，证明该 ID 已不存在，不宣称 DELETE 200。无 owned WS 进程/socket 遗留。

证据：`ws-control-summary.json`、`ws-control-before/after-*.json`、`ws-control-independent-validation.json`。

### 6. CDC 控制 shell 与正常 reboot

**PASS。强制 HTTP 断网场景：NOT TESTED；CDC 通路本身已在真实 ACM 上验证。**

仅打开 by-id if00，未使用 if02/J4。真实 root help 未广告独立 `status` 命令，因此未发送该猜测命令；使用 `pd status`。只读结果：

- `uname -a`：Zephyr、`0.3.0`、上述 build-id、`rp2350a`、`rpi_pico2/rp2350a/m33/mcuboot`。
- `pd status`：`available=1 mode=fixed voltage_mv=12000 gear=2 error=0`。
- `task show`：`available=true task_count=1`。原始证据是正常 reboot 前的 `cdc-readonly2-03.bin`；该采样在首次 HTTP flash 之后，不能充当首次 flash 前的 task 基线。
- `config show`：`available=true reason=ready saved_count=9 pending_count=0`。
- `ip`：原 MAC、`10.123.5.148/30`、板卡 `.149`、DHCP base `.150` 均保持。
- `app build-version` 与固件身份一致。

正常 reboot：

- CDC `reboot` 返回 `normal cold reboot in 250 ms (not BOOTSEL)`。
- 监测到应用 USB 断开/重枚举；12 秒内无 `2e8a:000f`、无 VENDOR=`RPI` 磁盘。
- 新 epoch 的只读 GET 显示 `uptime_ms=32495`，低于 reboot 前 `408053`；随后首轮 12V preflight 通过。未执行第二次 reboot。

原监测器曾在 250ms grace 内接收一个复位前 HTTP 响应并误判为恢复响应；保留原 `cdc-reboot-summary.json`。`cdc-reboot-postcheck-*.json` 与 post-reboot preflight 独立证明实际复位，见 `cdc-reboot-independent-validation.json`。

### 7. CDC BOOTSEL 与同一 combined 恢复

**PASS。物理 BOOTSEL 按钮：NOT TESTED。**

- 控制 shell `bootloader` 返回 `Entering rp2350 BOOTSEL in 250 ms...`。
- 同 serial ROM 为 bus 1 / address 28、`2e8a:000f`。
- 仅执行一次：

```sh
picotool load -v -x build/radxa_linkr_debugger/radxa-linkr-debugger-rp2350.uf2 \
  --bus 1 --address 28 --ser C0575D0BB2F6A330
```

- 时间 `12:31:12.297206Z` 至 `12:31:25.218385Z`，exit 0；两段 verify 均 `OK`。
- 应用同 serial 以 bus 1 / address 29 恢复，无 ROM 残留；网络不变，首轮 12V preflight 通过，最终 embedded app hash 仍一致。

证据：`cdc-bootloader-*`、`cdc-flash-*`、`after-cdc-flash-*`。

## 最终观测状态、运行态 delta 与本 lane 清理

**最终观测检查：PASS_WITH_DOCUMENTED_RUNTIME_DELTAS。完整运行态 baseline equality：false，不声明。** 最终只读结果仍保留 fixed 12V：

| 项目 | core 结束时的观测值 |
| --- | --- |
| Power | `12v_out=off`、`5v_out=on`、`vdd_5v=on`、`20v_out=off`、`5v_usb_hub=on`（首轮为 `off`，尚未恢复）、`5v_otg=on` |
| Switch | `sd=target`、`tf_wp=writable`，与首轮运行态一致 |
| GPIO | 固件目录全部 `input`；这不等于与首轮运行态完全一致，GP7 差异见下 |
| Saved snapshot | v1 snapshot present，`saved_count=9`、`pending=0`；逐项 saved 值与 `initial-current-config.json` 一致，未 erase NVS、未 save/clear |
| Tasks | 当前 `task_count=1`，本 lane 未写 task；可证明的基线范围见下，不声称首轮 flash 前 baseline equality |
| PD | fixed 12000mV，source 仍广告 5/9/12V |
| 最终 INA226 | 5.061250V / 0.679500A，`voltage_valid=true`；ADC0=11.637011V |
| Watchdog | supported/automatic/healthy/armed 均 true，`failing_service=""` |
| 网络 | 原 MAC 与 `10.123.5.148/30` 身份保持 |
| 本 lane 句柄 | 无遗留 preflight/raw/WS/CLI/picotool/CDC 进程；if00/if02 均无 fuser owner；无 owned capture |
| 外部客户端 | 既有外部 tray socket 保留，未断开 |

### 运行态 baseline delta 与后续收尾

首轮 BOOTSEL 前的 `initial-current-status.json` 是本次核正使用的运行态基线；它与 `after-http-flash-current-status.json`、`final-status.json` 对比得到：

- `power/5v_usb_hub`：`off → on → on`。首轮和最终的 saved snapshot 都保存 `power/5v_usb_hub=on`，所以这是 **boot replay 改变 live 运行态**，不是 saved snapshot 被改写。core 结束时该支路仍为 `on`，**尚未恢复为首轮 `off`**；后续 browser lane 负责有界 `PUT hub=off` 收尾，本报告不提前声称已完成。
- `gpio/GP7`：方向 `output → input`，core 结束时保持 `input`。因此“最终全部 input”只是最终观测，不能外推为运行态 baseline 完全恢复。

`initial-current-config.json` 与 `final-config.json` 的逐项 saved 值完全一致、`pending=0`。这个结论只证明 **saved snapshot 未变**，不用于掩盖上述 live runtime delta。派生核验以 `result=PASS_WITH_DOCUMENTED_RUNTIME_DELTAS`、`live_runtime_baseline.equal=false` 记录在 `final-state-validation.json`。

### Task 证据范围

本 lane 未执行 task store/import/delete/clear/run 等写入。正常 reboot 前的 `cdc-readonly2-03.bin` 明确包含 `task show available=true task_count=1`；最终 `final-tasks.json` 仍只有 slot 0 的 `edl-5v_otg` 一项。因此可以声明 **正常 reboot 与 CDC BOOTSEL 恢复前后 count 均为 1**。首次 HTTP flash 前没有 task-count raw 基线，所以不声称整个运行过程的 task baseline equality，也不以“当前为 1”替代该缺失证据。

## NOT TESTED / 不可外推项

- **20V fixed、20v_out、15V、PPS matrix：NOT TESTED。** 本次受限授权明确禁止请求。
- **物理 BOOTSEL 按键及人工手势：NOT TESTED。** 无人工操作，不伪造。
- **Browser QA、高频 LA、capture ownership：NOT TESTED。** 留待协调者后续阶段。
- **真实 target UART stimulus、J4 双向字节、目标 UART 命令：NOT TESTED。** if02 未写入。
- **确切 Zephyr client context 地址复用：NOT TESTED。** 仅有上述外部生命周期行为证据。
- **强制 HTTP unavailable 的 fallback 场景：NOT TESTED。** 本次证明的是实际 CDC 控制通路及 reboot/BOOTSEL 功能。
- **模拟电气认证、DMM、stuck-line 注入、外部 stimulus、示波器参数：NOT TESTED。** 数字遥测有效不等于这些项目通过。
- **完整 canonical HIL、完整 firmware rebuild、release readiness：不声明。** 本 lane 按范围只使用提供的 combined artifact。

## 证据索引

关键原始文件均位于 `/tmp/opencode/uncommitted-integrated-hil-20261029/`：

```text
core-results.json
initial-current-status.json
initial-current-config.json
initial-preflight-summary.json
initial-continuation-preflight-summary.json
http-bootsel-rom-identity.json
http-flash-picotool.log
after-http-flash-preflight-summary.json
after-http-flash-current-status.json
after-http-flash-current-config.json
source-cli-discover.stdout
source-cli-adc-20-100.ndjson
source-cli-adc-validation.json
http-lifecycle-summary.json
ws-control-summary.json
ws-control-independent-validation.json
cdc-readonly2-metadata.json
cdc-readonly2-03.bin
cdc-reboot-response.bin
cdc-reboot-independent-validation.json
after-cdc-reboot-preflight-summary.json
cdc-bootloader-summary.json
cdc-flash-picotool.log
after-cdc-flash-preflight-summary.json
final-status.json
final-config.json
final-tasks.json
final-state-validation.json
final-artifact-identity.log
final-lane-cleanup.log
SHA256SUMS
```

本 lane 未修改 firmware/host 源码、AGENTS、规范、git stage/history 或远端；仓库内仅新增本报告。Browser 结果由协调者后续独立追加，本文 core PASS 不依赖该未来结果。
