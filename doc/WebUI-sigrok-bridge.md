# WebUI Sigrok Bridge - 工作计划

## TL;DR

> **Quick Summary**: 在现有 WebSocket 端点 (`/api/v1/ws/*`) 中添加二进制帧支持，让 WebUI 通过 WebSocket 直接使用 sigrok 二进制协议控制逻辑分析仪，替代现有的 SCPI-over-WebSocket 方案。
>
> **Deliverables**:
> - 固件: 在现有 WS 端点添加二进制帧路由到 sigrok 协议处理器
> - 前端: sigrok 协议客户端库 + 重写 LogicAnalyzerCard
> - 文档: 架构文档更新
>
> **Estimated Effort**: Medium
> **Parallel Execution**: YES - 2 waves
> **Critical Path**: 固件 WS 二进制支持 → 前端协议库 → 前端 UI 重写

---

## Context

### Original Request
用户要求重写 WebUI 逻辑分析仪逻辑，适配新写的 sigrok 协议（TCP 5556 二进制）。由于浏览器不支持原生 TCP，选择 WebSocket 直通方案：在现有 WS 端点添加二进制帧支持，前端通过 WebSocket 发送/接收 sigrok 二进制帧。

### 现有架构

**WebUI (当前)**:
- `web/src/components/LogicAnalyzerCard.tsx` (1844 行) — 主 UI 组件
- `web/src/hooks/useScpiScope.ts` (180 行) — SCPI-over-WebSocket hook
- `web/src/lib/logicAnalyzer.ts` (440 行) — 配置/导出工具
- `web/src/lib/logicDecoder.ts` (822 行) — WASM 协议解码器

**固件 (当前)**:
- `linkr_debugger_sigrok_linkr.c` (1253 行) — sigrok 协议处理器（TCP 5556）
- `linkr_debugger_ws.c` — WebSocket 基础设施（仅支持文本帧）
- `linkr_debugger_http.c` — HTTP/WS 端点注册

**协议**:
- `doc/sigrok-linkr-v1.md` — sigrok v1 协议规范

### 问题

1. 浏览器不支持原生 TCP socket
2. 现有 WebSocket 只支持文本帧（JSON），不支持二进制
3. sigrok 协议是二进制协议，需要二进制帧传输

### 决策

选择 **方案 A：在现有 WS 端点添加二进制支持**（而非新建端点）：
- 复用现有 `/api/v1/ws/*` 端点，添加二进制帧路由
- 根据帧类型（TEXT vs BINARY）路由到不同处理器
- JSON 文本帧 → 现有电源分析/遥测处理器
- 二进制帧 → sigrok 协议处理器
- 优点：不需要新端点，架构更简单

---

## Work Objectives

### Core Objective
让 WebUI 通过 WebSocket 直接使用 sigrok 二进制协议控制逻辑分析仪。

### Concrete Deliverables

1. **固件: 在现有 WS 端点添加二进制帧支持**
   - 在 `linkr_debugger_ws.c` 添加二进制帧检测和路由
   - WebSocket 二进制帧 → sigrok 协议处理器
   - sigrok 响应 → WebSocket 二进制帧发回
   - 与现有 JSON 文本帧共享 WS slot

2. **前端: sigrok 协议客户端库**
   - `web/src/lib/sigrokClient.ts` — WebSocket 连接 + 协议解析
   - 实现 HELLO/CAPS/CONFIG/START/STOP/DATA 帧处理
   - 支持二进制帧发送/接收

3. **前端: 重写 LogicAnalyzerCard**
   - 使用 sigrokClient 替代 useScpiScope
   - 适配新的协议消息格式
   - 保留波形显示、导出、解码功能

4. **文档更新**
   - `doc/sigrok-linkr-v1.md` 添加 WebSocket 传输说明
   - `doc/capture-trigger-architecture.md` 更新架构图

### Definition of Done

- [ ] WebUI 可以通过现有 WS 端点发送 sigrok 二进制帧
- [ ] WebUI 可以发送 HELLO/CAPS/CONFIG/START/STOP 命令
- [ ] WebUI 可以接收 DATA 帧并显示波形
- [ ] 触发功能正常（rising/falling/either/none）
- [ ] 100 kHz - 100 MHz 速率测试通过
- [ ] 现有 TCP 5556 功能不受影响
- [ ] 现有 JSON 文本帧功能不受影响
- [ ] 并发控制：TCP 和 WS 不能同时使用 sigrok

### Must Have
- WebSocket 二进制帧支持
- sigrok 协议完整实现
- 与 TCP 5556 共享协议处理器
- 并发控制：同一时间只有一个客户端可以使用 sigrok 协议（TCP 或 WS）

### Must NOT Have
- 不修改现有 TCP 5556 逻辑
- 不引入新的外部依赖
- 不破坏现有 WebUI 功能
- 不允许 TCP 5556 和 WS 同时使用 sigrok 协议（会冲突）

---

## Verification Strategy

### Test Decision
- **Infrastructure exists**: YES
- **Automated tests**: Tests-after (实现后添加测试)
- **Framework**: Playwright (浏览器自动化测试)

### QA Policy
每个任务必须包含可执行的验证场景。Evidence 保存到 `.sisyphus/evidence/task-{N}-{scenario-slug}.{ext}`。

---

## Execution Strategy

### Parallel Execution Waves

```
Wave 1 (Start Immediately — 固件 + 前端基础):
├── Task 1: 固件 WS 端点 [quick]
├── Task 2: 前端 sigrokClient 库 [quick]
└── Task 3: 协议测试脚本 [quick]

Wave 2 (After Wave 1 — 前端 UI):
├── Task 4: 重写 LogicAnalyzerCard [visual-engineering]
└── Task 5: 集成测试 [unspecified-high]

Wave FINAL (After ALL tasks):
├── Task F1: Playwright 完整测试 [unspecified-high]
├── Task F2: 文档更新 [writing]
└── Task F3: 固件构建验证 [quick]
```

### Dependency Matrix

| Task | Depends On | Blocks |
|------|-----------|--------|
| 1. 固件 WS 端点 | — | 4, 5, F1 |
| 2. 前端 sigrokClient | — | 4, 5, F1 |
| 3. 协议测试脚本 | — | 5 |
| 4. 重写 LogicAnalyzerCard | 1, 2 | 5, F1 |
| 5. 集成测试 | 1, 2, 3, 4 | F1 |

---

## TODOs

- [ ] 1. 固件: 在现有 WS 端点添加二进制帧支持

  **What to do**:
  - 在 `linkr_debugger_ws.c` 的接收循环中添加二进制帧检测
  - 根据 `message_type` 路由：`WEBSOCKET_FLAG_TEXT` → 现有 JSON 处理，`WEBSOCKET_FLAG_BINARY` → sigrok 处理
  - 创建 `linkr_debugger_ws_handle_sigrok_binary()` 函数处理二进制帧
  - 调用现有 sigrok 协议处理器 `linkr_debugger_sigrok_linkr_handle_request()`
  - 通过 WebSocket 发送二进制响应（`WEBSOCKET_OPCODE_DATA_BINARY`）
  - 处理异步 EVENT 帧和 DATA 帧的 WS 发送
  - 添加 sigrok session 状态跟踪（每个 WS client 一个 sigrok session）
  - 添加并发控制：如果 TCP 5556 已有客户端，WS 二进制帧应返回 BUSY 错误

  **Must NOT do**:
  - 不修改现有 TCP 5556 逻辑
  - 不修改现有 JSON 文本帧处理逻辑
  - 不新建 WS 端点

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Task 2, 3)
  - **Blocks**: Task 4, 5, F1
  - **Blocked By**: None

  **References**:

  **Pattern References**:
  - `apps/radxa_linkr_debugger/src/linkr_debugger_ws.c:1835-1857` — 现有 WS 帧接收循环，需要在 line 1841 添加二进制帧分支
  - `apps/radxa_linkr_debugger/src/linkr_debugger_ws.c:1841-1844` — 现有 WS 只支持文本帧，需要添加 `WEBSOCKET_FLAG_BINARY` 分支
  - `apps/radxa_linkr_debugger/src/linkr_debugger_ws.c:198-200` — 现有 WS 发送函数，需要添加二进制发送路径

  **API/Type References**:
  - `apps/radxa_linkr_debugger/src/linkr_debugger_sigrok_linkr.h` — 协议类型定义（header, session, config 等）
  - `apps/radxa_linkr_debugger/src/linkr_debugger_sigrok_linkr.c:827-968` — `linkr_debugger_sigrok_linkr_handle_request()` 协议分发
  - `apps/radxa_linkr_debugger/src/linkr_debugger_sigrok_linkr.c:50-113` — `sigrok_linkr_stream_callback()` 数据流回调

  **External References**:
  - Zephyr WebSocket API: `websocket_send_msg()` 支持 `WEBSOCKET_OPCODE_DATA_BINARY` 发送二进制帧

  **WHY Each Reference Matters**:
  - WS 帧接收循环：需要在现有循环中添加二进制帧分支
  - 二进制帧处理：现有代码拒绝二进制，需要添加 `WEBSOCKET_FLAG_BINARY` 处理
  - 协议处理器：复用 `handle_request()` 和 `send_data_frame()`
  - 流回调：数据流需要通过 WS 发送而非 TCP

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: WebSocket 二进制帧支持
    Tool: Playwright
    Preconditions: 固件已构建并刷写，板子连接到 172.29.203.1
    Steps:
      1. 打开浏览器访问 http://172.29.203.1
      2. 执行 JavaScript: const ws = new WebSocket('ws://172.29.203.1/api/v1/ws/0')
      3. 验证 WebSocket 连接状态为 OPEN
      4. 发送文本帧（JSON）验证现有功能正常
      5. 发送二进制帧（HELLO_REQ: [0x72, 1, 0x01, 0,0,0,0, 0,0]）
      6. 验证收到二进制响应（HELLO_RESP）
    Expected Result: 文本帧和二进制帧都能正常收发
    Failure Indicators: 连接被拒绝、二进制帧被拒绝
    Evidence: .sisyphus/evidence/task-1-ws-binary-support.png

  Scenario: HELLO/CAPS 握手流程
    Tool: Playwright
    Preconditions: WebSocket 已连接
    Steps:
      1. 发送 HELLO_REQ (type=0x01, 9字节头, 无payload)
      2. 验证收到 HELLO_RESP (type=0x02, 5字节payload)
      3. 解析 HELLO_RESP: protocol_version=1, mode_count=2
      4. 发送 CAPS_REQ (type=0x03, 9字节头, 无payload)
      5. 验证收到 CAPS_RESP (type=0x04, 1+N*8字节payload)
      6. 解析 CAPS_RESP: 两个模式 (Fast8, Wide12)
    Expected Result: 握手成功，能力查询返回正确
    Failure Indicators: 响应类型错误、payload 长度错误
    Evidence: .sisyphus/evidence/task-1-hello-caps-handshake.png

  Scenario: CONFIG/START/STOP 采样流程
    Tool: Playwright
    Preconditions: 已完成 HELLO/CAPS 握手
    Steps:
      1. 发送 CONFIG_REQ (mode=1, trigger=none, channel_mask=0x01, samplerate=1000)
      2. 验证收到 CONFIG_RESP (state=CONFIGURED, actual_rate=1000)
      3. 发送 START_REQ
      4. 验证收到 START_RESP (state=RUNNING)
      5. 验证收到 DATA 帧 (type=0x11)
      6. 发送 STOP_REQ
      7. 验证收到 STOP_RESP (state=CONFIGURED)
    Expected Result: 采样流程完整，数据帧正常接收
    Failure Indicators: 状态转换错误、无 DATA 帧
    Evidence: .sisyphus/evidence/task-1-config-start-stop.png

  Scenario: 并发控制 - TCP 占用时 WS 返回 BUSY
    Tool: Playwright + Python
    Preconditions: 固件已刷写
    Steps:
      1. 用 Python 脚本连接 TCP 5556
      2. 发送 HELLO_REQ 并等待 HELLO_RESP（确保握手完成）
      3. 保持 TCP 连接打开
      4. 用 Playwright 连接 WS 并发送二进制 HELLO_REQ
      5. 验证收到 ERROR 帧 (type=0x7f, error_code=8 BUSY)
    Expected Result: WS 收到 BUSY 错误
    Failure Indicators: WS 连接成功（应该被拒绝）
    Evidence: .sisyphus/evidence/task-1-concurrent-busy.png
  ```

  **Commit**: YES
  - Message: `feat(ws): add binary frame routing to sigrok protocol handler`
  - Files: `apps/radxa_linkr_debugger/src/linkr_debugger_ws.c`, `apps/radxa_linkr_debugger/src/linkr_debugger_sigrok_linkr.h`
  - Pre-commit: `west build -p always -b rpi_pico2/rp2350a/m33/mcuboot --sysbuild apps/radxa_linkr_debugger -d build/radxa_linkr_debugger`

---

- [ ] 2. 前端: 创建 sigrok 协议客户端库

  **What to do**:
  - 创建 `web/src/lib/sigrokClient.ts`
  - 实现 WebSocket 连接管理（connect/disconnect/reconnect）
  - 实现 sigrok 协议帧解析（9字节头 + payload）
  - 实现协议命令（HELLO/CAPS/CONFIG/START/STOP）
  - 实现 DATA/EVENT 帧处理
  - 实现二进制帧构建和解析工具函数
  - 导出 TypeScript 类型定义

  **Must NOT do**:
  - 不实现 UI 逻辑（由 Task 4 负责）
  - 不依赖外部二进制解析库

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Task 1, 3)
  - **Blocks**: Task 4, 5, F1
  - **Blocked By**: None

  **References**:

  **Pattern References**:
  - `web/src/hooks/useScpiScope.ts` — 现有 WS hook 模式（连接管理、消息处理）
  - `web/src/lib/api.ts` — API 客户端模式

  **API/Type References**:
  - `doc/sigrok-linkr-v1.md:1-100` — 协议帧格式（9字节头、消息类型、payload 结构）
  - `apps/radxa_linkr_debugger/src/linkr_debugger_sigrok_linkr.h:1-80` — 协议常量和类型定义

  **Test References**:
  - `/tmp/opencode/test_rates.py` — Python 测试脚本，展示协议使用方式
  - `/tmp/opencode/test_trigger_and_rates.py` — 触发测试脚本

  **WHY Each Reference Matters**:
  - SCPI hook：参考现有 WS 连接管理模式
  - 协议规范：帧格式、消息类型、payload 结构的权威来源
  - Python 测试：展示完整的协议交互流程，可移植到 JS

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: 协议帧构建正确性
    Tool: Node.js 单元测试
    Preconditions: 无
    Steps:
      1. 调用 buildFrame({type: 0x01}) 构建 HELLO_REQ
      2. 验证帧长度 = 9 字节
      3. 验证 magic = 0x72
      4. 验证 version = 1
      5. 验证 type = 0x01
      6. 验证 payload_len = 0
    Expected Result: 帧构建正确
    Failure Indicators: 字节值错误、长度错误
    Evidence: .sisyphus/evidence/task-2-frame-build.png

  Scenario: 协议帧解析正确性
    Tool: Node.js 单元测试
    Preconditions: 无
    Steps:
      1. 构造 HELLO_RESP 帧字节: [0x72, 1, 0x02, 0,0,0,0, 5,0, 1, 0, 2, 0x00, 0x40]
      2. 调用 parseFrame(bytes) 解析
      3. 验证 header.type = 0x02
      4. 验证 header.payload_len = 5
      5. 验证 payload.protocol_version = 1
      6. 验证 payload.mode_count = 2
    Expected Result: 帧解析正确
    Failure Indicators: 解析错误、字段值错误
    Evidence: .sisyphus/evidence/task-2-frame-parse.png

  Scenario: WebSocket 连接和握手
    Tool: Node.js 单元测试 + Mock WebSocket
    Preconditions: Mock WebSocket 服务器
    Steps:
      1. 创建 SigrokClient 实例
      2. 调用 connect('ws://172.29.203.1/api/v1/ws/0')
      3. 验证自动发送 HELLO_REQ
      4. Mock 返回 HELLO_RESP
      5. 验证自动发送 CAPS_REQ
      6. Mock 返回 CAPS_RESP
      7. 验证 client.state === 'ready'
    Expected Result: 自动握手完成
    Failure Indicators: 状态未更新、握手超时
    Evidence: .sisyphus/evidence/task-2-auto-handshake.png
  ```

  **Commit**: YES
  - Message: `feat(webui): add sigrok protocol client library`
  - Files: `web/src/lib/sigrokClient.ts`, `web/src/lib/sigrokTypes.ts`
  - Pre-commit: `cd web && npm test`

---

- [ ] 3. 创建协议测试脚本

  **What to do**:
  - 创建 `apps/radxa_linkr_debugger/tests/test_sigrok_ws.py`
  - 使用 Python websocket-client 库（`pip install websocket-client`）
  - 实现完整的协议交互测试
  - 测试 HELLO/CAPS/CONFIG/START/STOP/DATA 流程
  - 测试触发功能（rising/falling/either/none）
  - 测试不同速率（100 kHz - 100 MHz）
  - 测试并发控制：验证 TCP 和 WS 不能同时使用 sigrok

  **Must NOT do**:
  - 不测试 TCP 5556（已有测试脚本）
  - 不测试 WebUI（由 Task 5 负责）

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Task 1, 2)
  - **Blocks**: Task 5
  - **Blocked By**: None

  **References**:

  **Pattern References**:
  - `apps/radxa_linkr_debugger/tests/model_host/test_linkr_debugger_sigrok_linkr.c` — 现有 sigrok 协议测试模式
  - `/tmp/opencode/test_rates.py` — 现有速率测试脚本模式（参考用）
  - `/tmp/opencode/test_trigger_and_rates.py` — 触发测试脚本模式（参考用）

  **API/Type References**:
  - `doc/sigrok-linkr-v1.md` — 协议规范

  **WHY Each Reference Matters**:
  - 现有测试脚本：展示 Python 协议交互模式，可直接移植到 WebSocket

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: WebSocket 协议握手测试
    Tool: Python 脚本
    Preconditions: 固件已刷写，板子可达 172.29.203.1
    Steps:
      1. 运行 python3 apps/radxa_linkr_debugger/tests/test_sigrok_ws.py --test hello
      2. 验证输出 "HELLO: OK"
      3. 验证输出 "CAPS: OK, modes=2"
    Expected Result: 握手成功
    Failure Indicators: 连接超时、响应错误
    Evidence: .sisyphus/evidence/task-3-hello-caps.txt

  Scenario: 速率测试 (100 kHz - 100 MHz)
    Tool: Python 脚本
    Preconditions: 已完成握手
    Steps:
      1. 运行 python3 apps/radxa_linkr_debugger/tests/test_sigrok_ws.py --test rates
      2. 验证 100 kHz: PASS
      3. 验证 500 kHz: PASS
      4. 验证 1 MHz: PASS
      5. 验证 10 MHz: PASS
      6. 验证 100 MHz: PASS
    Expected Result: 所有速率通过
    Failure Indicators: 任何速率 FAIL
    Evidence: .sisyphus/evidence/task-3-rates.txt

  Scenario: 触发测试 (rising/falling/either)
    Tool: Python 脚本
    Preconditions: 已完成握手
    Steps:
      1. 运行 python3 apps/radxa_linkr_debugger/tests/test_sigrok_ws.py --test trigger
      2. 验证 rising: PASS (不是秒触发)
      3. 验证 falling: PASS
      4. 验证 either: PASS
    Expected Result: 所有触发类型通过
    Failure Indicators: 秒触发、触发失败
    Evidence: .sisyphus/evidence/task-3-trigger.txt

  Scenario: 并发控制测试
    Tool: Python 脚本
    Preconditions: 已完成握手
    Steps:
      1. 运行 python3 apps/radxa_linkr_debugger/tests/test_sigrok_ws.py --test concurrent
      2. 验证 TCP 连接时 WS 收到 BUSY
      3. 验证 WS 连接时 TCP 收到 BUSY
    Expected Result: 并发控制正常
    Failure Indicators: 两个连接都能成功
    Evidence: .sisyphus/evidence/task-3-concurrent.txt
  ```

  **Commit**: YES
  - Message: `test(webui): add sigrok WebSocket protocol test script`
  - Files: `apps/radxa_linkr_debugger/tests/test_sigrok_ws.py`
  - Pre-commit: `python3 apps/radxa_linkr_debugger/tests/test_sigrok_ws.py --test hello`

---

- [ ] 4. 前端: 重写 LogicAnalyzerCard

  **What to do**:
  - 重写 `web/src/components/LogicAnalyzerCard.tsx`
  - 使用 `sigrokClient` 替代 `useScpiScope`
  - 适配新的协议消息格式
  - 保留波形显示、导出、解码功能
  - 更新状态管理（IDLE/CONFIGURED/ARMED/RUNNING）
  - 更新 UI 控件（配置、启动、停止）

  **Must NOT do**:
  - 不修改波形渲染逻辑（保留现有 SVG 渲染）
  - 不修改协议解码逻辑（保留现有 WASM 解码器）
  - 不修改导出逻辑（保留现有 CSV/.sr 导出）

  **Recommended Agent Profile**:
  - **Category**: `visual-engineering`
  - **Skills**: [`frontend-ui-ux`]

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Wave 2 (after Wave 1)
  - **Blocks**: Task 5, F1
  - **Blocked By**: Task 1, 2

  **References**:

  **Pattern References**:
  - `web/src/components/LogicAnalyzerCard.tsx` — 现有 UI 组件（需要重写）
  - `web/src/hooks/useScpiScope.ts` — 现有 SCPI hook（将被替代）

  **API/Type References**:
  - `web/src/lib/sigrokClient.ts` — 新的 sigrok 客户端库（Task 2 产出）
  - `web/src/lib/types.ts` — 现有类型定义（可能需要更新）

  **UI/UX References**:
  - `web/src/components/PowerAnalyzer.tsx` — 参考现有触发式采集 UI 模式

  **WHY Each Reference Matters**:
  - 现有组件：需要理解现有功能才能完整迁移
  - sigrokClient：新的 API 接口
  - PowerAnalyzer：参考触发式采集的 UI 模式

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: UI 加载和连接
    Tool: Playwright
    Preconditions: 固件已刷写，板子可达
    Steps:
      1. 打开 http://172.29.203.1
      2. 切换到 Terminal workspace
      3. 验证 LogicAnalyzerCard 组件加载
      4. 验证 WebSocket 连接状态显示为 "Connected"
    Expected Result: UI 正常加载，WebSocket 已连接
    Failure Indicators: 组件未加载、连接状态错误
    Evidence: .sisyphus/evidence/task-4-ui-load.png

  Scenario: 配置和启动采样
    Tool: Playwright
    Preconditions: UI 已加载
    Steps:
      1. 选择模式 Fast8
      2. 选择通道 GP10
      3. 设置速率 1000 kHz
      4. 点击 "Start" 按钮
      5. 验证状态变为 "Running"
      6. 等待 1 秒
      7. 点击 "Stop" 按钮
      8. 验证状态变为 "Configured"
      9. 验证波形显示采集数据
    Expected Result: 采样流程完整，波形显示正常
    Failure Indicators: 状态转换错误、无波形数据
    Evidence: .sisyphus/evidence/task-4-sample-flow.png

  Scenario: 触发采样
    Tool: Playwright
    Preconditions: UI 已加载
    Steps:
      1. 选择模式 Fast8
      2. 选择通道 GP10
      3. 设置触发类型 Rising
      4. 设置触发通道 GP10
      5. 设置速率 1000 kHz
      6. 点击 "Start" 按钮
      7. 验证状态变为 "Armed"
      8. 在 GP10 上产生上升沿
      9. 验证状态变为 "Running"
      10. 等待采样完成
      11. 验证波形显示触发后的数据
    Expected Result: 触发采样正常，波形显示正确
    Failure Indicators: 秒触发、触发后无数据
    Evidence: .sisyphus/evidence/task-4-trigger-sample.png
  ```

  **Commit**: YES
  - Message: `feat(webui): rewrite LogicAnalyzerCard to use sigrok protocol`
  - Files: `web/src/components/LogicAnalyzerCard.tsx`, `web/src/hooks/useScpiScope.ts` (可能删除)
  - Pre-commit: `cd web && npm test && npm run build`

---

- [ ] 5. 集成测试

  **What to do**:
  - 运行完整的端到端测试
  - 测试固件 WS 端点 + 前端 UI 完整流程
  - 测试所有速率（100 kHz - 100 MHz）
  - 测试所有触发类型（none/rising/falling/either）
  - 测试数据导出（CSV/.sr）
  - 测试协议解码（UART/I2C/SPI）
  - 生成测试报告

  **Must NOT do**:
  - 不修改任何代码（只测试）

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: []

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Wave 2 (after Task 1-4)
  - **Blocks**: F1
  - **Blocked By**: Task 1, 2, 3, 4

  **References**:

  **Pattern References**:
  - `/tmp/opencode/test_rates.py` — 速率测试模式
  - `/tmp/opencode/test_trigger_and_rates.py` — 触发测试模式

  **Test References**:
  - `web/src/lib/logicAnalyzer.test.ts` — 现有前端测试

  **WHY Each Reference Matters**:
  - 现有测试脚本：展示测试模式和验证方法

  **Acceptance Criteria**:

  **QA Scenarios (MANDATORY):**

  ```
  Scenario: 完整速率测试
    Tool: Playwright + Python
    Preconditions: 固件已刷写，UI 已加载
    Steps:
      1. 运行 python3 apps/radxa_linkr_debugger/tests/test_sigrok_ws.py --test rates
      2. 在 Playwright 中配置 100 kHz 并启动
      3. 验证波形显示
      4. 重复 500 kHz, 1 MHz, 5 MHz, 10 MHz, 50 MHz, 100 MHz
    Expected Result: 所有速率通过
    Failure Indicators: 任何速率 FAIL
    Evidence: .sisyphus/evidence/task-5-all-rates.txt

  Scenario: 触发功能测试
    Tool: Playwright + Python
    Preconditions: 固件已刷写，UI 已加载
    Steps:
      1. 运行 python3 apps/radxa_linkr_debugger/tests/test_sigrok_ws.py --test trigger
      2. 在 Playwright 中配置 Rising 触发
      3. 产生上升沿
      4. 验证触发后数据
      5. 重复 Falling, Either
    Expected Result: 所有触发类型通过
    Failure Indicators: 秒触发、触发失败
    Evidence: .sisyphus/evidence/task-5-trigger.txt

  Scenario: 数据导出测试
    Tool: Playwright
    Preconditions: 已完成采样
    Steps:
      1. 完成一次采样
      2. 点击 "Export CSV" 按钮
      3. 验证下载的 CSV 文件包含正确数据
      4. 点击 "Export .sr" 按钮
      5. 验证下载的 .sr 文件可在 PulseView 中打开
    Expected Result: 导出功能正常
    Failure Indicators: 导出失败、文件格式错误
    Evidence: .sisyphus/evidence/task-5-export.png
  ```

  **Commit**: NO (测试不提交)

---

## Final Verification Wave

- [ ] F1. **Playwright 完整测试** [unspecified-high]
  验收所有任务的 QA 场景。运行完整的端到端测试流程。生成最终测试报告。

- [ ] F2. **文档更新** [writing]
  更新 `doc/sigrok-linkr-v1.md` 添加 WebSocket 传输说明。更新 `doc/capture-trigger-architecture.md` 架构图。

- [ ] F3. **固件构建验证** [quick]
  运行完整固件构建，验证无编译错误，验证 Flash/RAM 使用在范围内。

---

## Commit Strategy

- **1**: `feat(ws): add binary frame routing to sigrok protocol handler`
- **2**: `feat(webui): add sigrok protocol client library`
- **3**: `test(webui): add sigrok WebSocket protocol test script`
- **4**: `feat(webui): rewrite LogicAnalyzerCard to use sigrok protocol`

---

## Success Criteria

### Verification Commands
```bash
# 固件构建
west build -p always -b rpi_pico2/rp2350a/m33/mcuboot --sysbuild apps/radxa_linkr_debugger -d build/radxa_linkr_debugger  # Expected: BUILD SUCCESS

# 前端构建
cd web && npm run build  # Expected: BUILD SUCCESS

# 协议测试
python3 apps/radxa_linkr_debugger/tests/test_sigrok_ws.py --test all  # Expected: ALL PASS

# Playwright 测试
cd web && npx playwright test  # Expected: ALL PASS
```

### Final Checklist
- [ ] 所有 "Must Have" 存在
- [ ] 所有 "Must NOT Have" 不存在
- [ ] 所有速率测试通过（100 kHz - 100 MHz）
- [ ] 所有触发类型测试通过（none/rising/falling/either）
- [ ] 现有 TCP 5556 功能不受影响
- [ ] 现有 WebUI 其他功能不受影响
- [ ] 并发控制正常（TCP 和 WS 不能同时使用 sigrok）
