# 2026-11-02：J4 BFCache 恢复断线离线验证

结论：**浏览器生命周期问题已复现并修复；本记录不是板级 HIL，也不证明任意
网络中断或所有长期闲置场景都已解决。** 未连接、刷写或操作真实调试板/目标机。

## 现象与因果

用户报告 `Page entered Back-Forward Cache` 后终端保持断开，同时出现
`/api/v1/live-sessions/<id>` 404。

- 旧客户端仅在 board availability epoch 首次可用时自动尝试连接；从 BFCache
  恢复不会重建 App，因此旧 socket 失败后自动连接机会已被消耗。
- 当前固件收到 WebSocket CLOSE/接收错误会释放 live session；随后再 DELETE
  同一 ID 可正常返回 `404 unknown_session_id`。它可能是清理结果，不是断线原因。
- 30 秒预约超时不回收已连接会话，不能解释成“串口空闲 30 秒被踢”。
- `/ws/0` 是可复用 slot，`/live-sessions/37` 是 session ID，两者不需相同。

## 验证方法及结果

使用 Chromium **156.0.8078.79** 和生产 Web bundle，连接仅监听
`127.0.0.1:18765` 的临时 HTTP/WebSocket fixture。该服务不转发任何请求到硬件；
浏览器同时屏蔽真实调试板私网 URL。静态 status 数据只用于让 App 可用，不能作为
硬件测试证据。

通过真正历史导航离开页面并返回，记录浏览器原生
`pageshow.persisted === true`，不是人工派发一个 pageshow 事件替代 BFCache。

| 场景 | 结果 |
| --- | --- |
| 修复前进入/恢复 BFCache | 原生恢复成功，但保持原 session 1，终端显示断开及“旧输入不会重放” |
| 修复前旧会话清理 | 已释放 session 1 后 DELETE 返回 404，复现次生错误链 |
| 修复后进入/恢复 BFCache | session 3 → 新 session 4，只增加一次订阅，终端恢复 Connected |
| 已发送、接受但尚未完成的输入 | fixture 只收到一次 `BF_CACHE_PENDING_NO_REPLAY`；新 session 没有重发 |
| 用户主动 Disconnect 后再历史恢复 | 保持断开，不自动建立新串口会话 |
| 正常已升级 socket 关闭 | 不发多余 DELETE，没有对应的重复清理 404 |

修复后生产 app.js SHA-256：
`9a8a48118ffd358860775137d151571ebde70ca72001d218c540df2467905845`。
临时 fixture、浏览器事件与验证日志保存在
`/tmp/opencode/j4-bfcache-2026-11-02/`；验证后已关闭本轮 QA 浏览器与服务。

## 回归及构建

源码基准为 `c614331` 加本轮未提交改动。修复前新增定向用例为 12 失败、54 通过；
11 月 2 日首次修复的完整验证如下；本节产物 hash 不代表后续 review 修复镜像：

- `cd web && npm test`：85 个 Vitest 文件、**1055 项通过**；Node lane 同时通过，
  保留其已有的一条条件跳过。
- `nix-shell --run 'cd web && npm run build'`：通过，保留现有 bundle 大小警告。
- workflow/gate/registration 测试：**331 项通过**；三个 checker 通过。
- `make firmware`：完整构建通过，Flash 834804/847832 B、RAM 499256/532480 B。
- combined UF2 SHA-256：
  `bfa3c02b36fc1c1478a604b36d0e917101204bd66c1cb80a86265470e8dcca45`。
  **仅生成产物，未刷入开发板。**

单元回归还覆盖 StrictMode、freeze/pagehide 与 resume/pageshow 去重、普通隐藏
不关连接、恢复排队时手动断开优先、旧回调失效、未使用预约清理、普通网络故障
不热重试及配置结果不得跨连接确认。恢复不会重放 TX 或配置请求，不放宽
自动响应器/DTR/背压/自动化门禁；已发送未确认也不等于目标肯定没有执行。

## 未验证范围

本轮没有板载 Web/MCU/UART HIL，没有长时间实际网络丢包实验；源码中的恢复能力
需要更新板载固件后才能对开发板提供的页面生效。普通网络故障仍可需要人工
Connect J4，不承诺无限自动重连。

## 2026-11-04 review 补充

review 发现并修复两个未被首次回归覆盖的边界：

- 预约 POST 或清理 DELETE（含响应体）挂起会永久阻塞 replacement。现在每个
  请求有 8 秒总截止时间，独立超时拒绝并尽力取消；socket close/cleanup 等待
  上界为 8.75 秒。迟到预约只清理自己的旧 ID，不影响新会话，不重放输入或配置。
- 等待旧 socket 关闭或 board availability 时，再次挂起不再覆盖未消费的恢复意图；
  真正开始连接或手动 Disconnect 才清除它，手动断开仍优先。

确定性回归先在修复前复现：client 的 POST/DELETE headers/body 四例失败，hook
的连续生命周期两例失败；修复后 client 61 项、hook 21 项定向测试通过，另含迟到
清理和第二轮手动取消等覆盖。日志位于
`/tmp/opencode/j4-high-baud-2026-11-04/review-{red,green}-vitest.log` 和
`/tmp/opencode/j4-restore-intent-{red,green}.log`。这是受控离线传输证据，不是板级 HIL。

第一批暂存快照独立保留旧波特率契约，五个相关测试文件共 **171 项通过**，
`tsc -b` 通过。11 月 4 日合并工作树的生产 bundle 还通过同一 loopback fixture
的真实 Chromium 历史导航复测：原生 BFCache 恢复后 session 1 → 2，仅恢复一次，
未完成输入不重放、手动 Disconnect 保持断开、正常关闭不重复 DELETE。
该 bundle 的 app.js SHA-256 为
`54b6d4b2964d41605748fe1042af89d6eaeadf85819d15ec0bcec974e67c2235`；
复测结果在 `/tmp/opencode/j4-high-baud-2026-11-04/review-validation/`。
浏览器及本地 fixture 已关闭；没有连接真实调试板。
