# 2026-11-09：未提交改动审查与修复

## 范围与结论

以 HEAD `ff3accef3777806f5db6fbe867135a1a8a8c2581` 为基线，审查暂存、未暂存和未跟踪内容，
包括 Tray、采集内存/codec7、Web/WASM、Zephyr 迁移、WebUSB、构建门禁及相关文档。
修复一个可触发的协议错误处理问题、一个实际阻塞本地检查的 fixture 隔离问题，并修正文档漂移。
没有提交、push 或部署 Pages；原暂存内容保持不变，修复留在工作树。

## 已修复

### WebSocket 非法 DATA 异常外溢

`web/src/lib/sigrokClient.ts` 的 DATA mask/成功 CONFIG 校验会抛异常，但生产
`ws.onmessage` 没有接住。一个消息内含错误 DATA 和 STOPPED 时，未发布 error，
剩余终态留在接收 buffer，状态和等待请求未及时结束。

现在复用已有连接失败清理：关闭 socket、清空会话/buffer、拒绝 pending requests，
并发布真实错误。不继续丢帧采集，也不把尾随 STOPPED 当成功。正常解码和低层同步拒绝不变。

- 修改前针对生产入口的回归有7项失败；修改后 `sigrokClient.test.ts` **60/60** 通过。
- 真实 Chromium156 原生 WebSocket 与 loopback 服务端：错误 DATA/STOPPED 发布0，
  error + disconnected 正常，pending CONFIG 0.5ms内拒绝，两端 socket 关闭。
- window error、unhandled rejection、page exception均0；同一 client 重连后，新 mask
  `0x0441` 的有效数据 `[1,2,3]` 正确发布。不是 fake WebSocket 或真实板卡流量验证。

### 临时 Git fixture 继承真实用户签名配置

完整检查中的8个 worktree-scope 测试在临时仓库 `git commit -m base` 失败。
只读确认当前用户全局 `commit.gpgsign=true`，而 fixture 没有隔离该设置。
现在仅为测试创建的临时仓库设置本地 `commit.gpgsign=false`；未修改用户全局或项目 Git 配置，
未执行项目提交。原8项失败重验全部通过。

### 文档能力漂移

双语 Web UI 和 Web README 仍有旧的16路/512点统一上限、Terminal 合并工作区、
Stream 不支持解码和 native v2 已集成等描述。已同步当前 GP12–GP16 Web 选路、
有限采集与预触发的不同限制、滚动缓存/实时解码窗口、独立工作区和 native 原型边界，
同时将自动打开说明收敛为 best-effort。没有修改视觉或扩大实现范围。

## 已撤销的 USB 泄漏误报

初次静态审查把 GET_URL callback 的零字节分配与 ch9 无数据阶段处理连接，漏看了入口判定。
实际 `usbd_ch9.c:33-40` 要求 `wLength != 0` 才归为 to-host；零长度请求归 to-device，
本项目没有 OUT handler，直接 ENOTSUP/STALL，不进入 IN callback，所谓泄漏路径不可达。

首次真机测试因错误地期待零长度 ACK 而 FAIL。报告保留，未改写为通过；复核完整调用链后，
已撤回多余的固件 guard，不将该误报算修复成果。新增正式 live HIL 入口
`apps/radxa_linkr_debugger/tests/webusb_hil.py`，按真实契约严格检查64次零长度请求均PIPE，
每8次和最终合法 device/BOS/GET_URL 仍可读。timeout、任意错误或意外接受都不算通过。
入口已登记 canonical HIL 规范，不混入 offline runner，也未用 source 正则伪装行为测试。

## 最终验证

| 命令/范围 | 结果 |
| --- | --- |
| `make -k check` 首轮 | exit2，仅上述8个签名 fixture 失败；其他 lane 收集完成 |
| 修复后 `make gates` | exit0，383/383、原8项及后续6个checker全部通过 |
| 持久化文档/版本 | 63/63测试、14/14版本字段通过 |
| Rust CLI / Host / decoder | 688 / 115 / 21项通过，fmt/clippy通过 |
| 固件 offline C/Python | 注册 C 模型全部通过，Python115+16项通过 |
| Web | Node458通过/1个既有opt-in loopback skip，Vitest1089通过，生产构建通过 |
| 最终 `make firmware` | exit0，完整 sysbuild、镜像封装及 combined UF2 |
| 最终 native Nix 快照 | 1203个tracked+untracked源，`nix flake check -L --no-update-lock-file path:...` exit0 |

Nix 仅覆盖原生 `x86_64-linux`，最终包为
`/nix/store/d7pmcv03xc3fqdp42wcdxigp3lhzdks2-radxa-linkr-debuggerctl-0.3.0`；
WASM `wasm-release`、Web、Host、CLI687项包内测试、desktop-entry 和 rdb alias/version检查通过。
Nix 的显式 path 快照包含未跟踪源码，未用 Git flake 的文件筛选获得假通过。
最终修正快照包含测试 fixture 修复和修正后的 HIL 规范；早先快照结果仍保留为历史。

没有在修复单个 fixture 后重复全部有效 suite；383项门禁单独重验覆盖其影响面。
最初 `make -k check` 的 exit2 如实保留，不伪称该次命令 exit0。

最终应用 Flash **826000/843736B**、RAM **485296/532480B**，资源门禁保持。
最新 combined UF2：
`e0ec2f628eb75e28c785a82a495311596212aa5786e46912e00d49861f0381bf`。
中间含多余 guard 的 `c38d6896…` 已被该最终镜像替换。

## 本轮板级验收：usb-only

仅操作已授权 USB `1-7.4.2`、serial `F2836E780B95F053`。发现 J4 CDC 有连接时先停止，
取得用户“允许本次中断”后才刷写。每次启动首先设置 `5v_out=on` 并回读逻辑状态，
没有请求 PD 或追加电源/线路操作。

- 用户本机 sudo 运行正式 live HIL：64/64零长度请求严格PIPE；短长度1/3/67B、
  五个完整非法索引及之后正常 device/BOS/URL读取通过，`bcdUSB=0x0210`。
- GET_URL 仍为 HTTPS Pages 链接，fragment为当前板卡 `10.42.51.125`。
- 最终镜像 NCM HTTP/WS status正常，watchdog healthy、J4 ready；板端实际 app.js
  SHA-256 `48b20309505e54e2e237897faf27be239952e5eb35616e3d5629681314c89d31`
  与 canonical build 一致。
- 正常 reboot、HTTP BOOTSEL、控制 CDC BOOTSEL、指定 serial 的 ROM 枚举、
  combined UF2 恢复和各次 usb-only fixture通过；最终板保留上述新镜像在线。

PD20、外部电源/负载、ADC/INA准确度、功率采集、GPIO驱动及采集吞吐不在本轮profile内，
不声明完整 HIL、Windows/macOS运行或 Chrome 自动通知已验证。Pages仍未发布。

## 未扩大修复的边界

采集 arena/压缩/FAST8、Zephyr/DT迁移及 Tray 改动未确认本 diff 新增的正常流程缺陷。
旧 burst reset 的并发窗口和 probe-client初始化失败后的唤醒为既存风险线索，未做运行复现，
不将其计为本轮新增阻断或声称已解决。新增 decoder/slot-pool 等未跟踪文件仍需在未来提交时
一并纳入；本轮没有代用户暂存或提交。

完整证据：`/tmp/opencode/uncommitted-review-2026-11-09/` 的 `validation/`、
`web-protocol/`、`browser-protocol/`、`nix-validation/`、`hil/` 与 `final-corrected/`。
