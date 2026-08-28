# 2026-08-28 Linkr Watchdog Rollback Fault-Injection HIL

## 结论

**PASS（重构后完整 HIL）**。真实 Radxa Linkr Debugger G3（RP2350A）上完成了
watchdog rollback、OTA auto/manual confirm、任务保存/两次冷重启/执行/清除、
HTTP/WS、CDC ACM、HTTP/CDC BOOTSEL 与 production 恢复验证。

## 被测目标

- Board: Radxa Linkr Debugger G3, RP2350A
- USB: `2fe3:db01`
- CDC ACM: `/dev/ttyACM2`
- HTTP: `http://172.29.203.1`
- 分支: `dev`

## 镜像

| 用途 | 归档路径 | MCUboot 版本 | Fault hook |
|---|---|---|---|
| canonical 生产镜像 | `fault-injection-hil/production/radxa-linkr-debugger-rp2350.uf2` | `0.3.0+0` | 关闭 |
| HIL 回滚基线 | `fault-injection-hil/fault-baseline/radxa-linkr-debugger-rp2350.uf2` | `0.0.1+0` | 开启 |
| HIL OTA 候选 | `fault-injection-hil/fault-candidate/radxa-linkr-debugger-rp2350.uf2` | `0.0.2+0` | 开启 |

三个镜像均通过规范要求的 `build/radxa_linkr_debugger` 完整 sysbuild 构建，并在
每次切换后再归档，不再使用旁路构建目录。关键配置见
[build-config-snippet.txt](./2026-08-28-watchdog-rollback-fault-injection-hil/build-config-snippet.txt)，
产物哈希见
[artifacts.SHA256SUMS](./2026-08-28-watchdog-rollback-fault-injection-hil/artifacts.SHA256SUMS)。

## 执行过程

1. 记录 canonical preflight：`/api/v1/watchdog/fault` 返回 HTTP 404。
2. 通过 ROM BOOTSEL 刷写 `0.0.1+0` HIL 基线 combined UF2。
3. 验证 `/api/v1/watchdog/fault` 返回 `available=true, armed=false`。
4. HTTP `POST /api/v1/watchdog/fault` 在无 OTA marker 时返回 409
   `fault_injection_rejected`。
5. 同日早前运行已通过 CDC ACM shell 验证 `watchdog fault-injection status`
   返回 `available=true armed=false`，`arm` 返回 `rejected: -22`。
6. 上传 `0.0.2+0` 候选 OTA 镜像并请求 test boot；MCUboot 执行真实 swap。
7. 板子进入 `pending_test` 且 marker 存在后，`POST /api/v1/watchdog/fault`
   返回 `armed=true`。
8. 观察到 watchdog reset 后连接中断；板子重新启动时输出
   `watchdog reset after OTA test: allowing MCUboot rollback path`。
9. 回滚后先观察到中间态：OTA `state=idle`、
   `current_image_confirmed=true`、`test_marker_present=true`，确认这是
   MCUboot 回滚保留 marker 而不是候选镜像 auto-confirm。
10. 等待 marker 清除后断言：`build.image_version=0.0.1+0` 回到基线、
    uptime 低于 `基线 uptime + HIL 经过时间`、`/api/v1/watchdog/fault`
    仍可用且 `armed=false`，没有 `RPI-RP2` 磁盘枚举。
11. 从 ROM BOOTSEL 刷写已归档的 canonical production combined UF2，板子恢复
    HTTP/CDC：`build.profile=production`、
    `build.image_version=0.3.0+0`、`/api/v1/watchdog/fault` 返回 HTTP 404。
12. CDC ACM shell 验证 `uname -a` 输出 `Zephyr linkr-debugger 0.3.0
    v0.2.1-208-gc72104d89ca1-dirty ...`，`app build-version` 输出
    `v0.2.1-208-gc72104d89ca1`。

## 过程中修复的回归

第一次实际执行发现：MCUboot 回滚后状态已经变为 `idle` 且
`current_image_confirmed=true`，但 `test_marker_present` 仍为 `true`，原
auto-confirm 处理程序只在 `pending_test` 状态下清理 marker，导致一直重试。

修复：

- `linkr_debugger_ota.c`：当 marker 存在、看门狗健康、当前镜像已确认且 OTA
  状态为 `idle` 时，直接清除 marker；不改写 MCUboot confirmed 状态。
- `test_linkr_debugger_ota.c`：新增回滚后 marker 清理回归测试，覆盖该缺陷。
- 进一步修复 confirm 与 fault arm 的原子窗口：`confirm_pending` 保持到
  `boot_write_img_confirmed()` 和 marker 清除都完成后才释放；watchdog
  回归测试要求 marker 清除发生在 confirm pending 期间。

本次验证还增加了：

- HIL runner 在复位后要求先看到 `idle/confirmed + marker=true` 的中间态。
- HIL runner 读取基线/回滚后的 `build.image_version`，并把 uptime 与
  “基线 uptime+经过时间”的无复位预期比较，避免把网络抖动误判为 reset。
- `flash-uf2` 在 dry-run 也拒绝 app-only `zephyr.uf2`，只接受规范化
  `radxa-linkr-debugger-rp2350.uf2`。

最终重构后的完整板级复测：

- Watchdog rollback：`watchdog-rollback-run-4.log`，回滚中间态 marker=true、
  `image_version=0.0.1+0`、uptime 回落、marker 清除、无 RPI-RP2。
- OTA：`ota-auto-final.log` / `ota-manual-final.log`，最终 production
  `0.3.0+0` upload/test/auto-confirm/manual-confirm 全部成功。
- Task：`task-store-final.json`，两次 `kernel reboot cold` 后仍
  `task_count=1/hil-storage`；`task-run-final.log` 执行 1 条安全 GP13 请求；
  `task-list-clear-final.json` 清除后为 0。
- HTTP/WS：`final-restore-status.json`、`ws-final.log`，WS
  `snapshot/status` 正常。
- CDC：`cdc-final.log`，`config show`、`uname -a`、`app build-version` 正常。
- BOOTSEL：HTTP 和 CDC 均进入 RPI-RP2 并恢复 production。
- 最终：`final-restore-fault.http` 返回 404，`final-restore-ota.json` 为
  `idle/confirmed`。

随后单元测试、完整 firmware 构建、Web 测试和仓库门禁均通过。

## 证据

- [watchdog-rollback-run-3.log](./2026-08-28-watchdog-rollback-fault-injection-hil/watchdog-rollback-run-3.log)
- [watchdog-rollback-run-4.log](./2026-08-28-watchdog-rollback-fault-injection-hil/watchdog-rollback-run-4.log)
- [ota-auto-final.log](./2026-08-28-watchdog-rollback-fault-injection-hil/ota-auto-final.log)
- [ota-manual-final.log](./2026-08-28-watchdog-rollback-fault-injection-hil/ota-manual-final.log)
- [task-store-final.json](./2026-08-28-watchdog-rollback-fault-injection-hil/task-store-final.json)
- [task-list-after-reboot2-final.json](./2026-08-28-watchdog-rollback-fault-injection-hil/task-list-after-reboot2-final.json)
- [task-run-final.log](./2026-08-28-watchdog-rollback-fault-injection-hil/task-run-final.log)
- [task-list-clear-final.json](./2026-08-28-watchdog-rollback-fault-injection-hil/task-list-clear-final.json)
- [watchdog-rollback-run-1-marker-stuck.log](./2026-08-28-watchdog-rollback-fault-injection-hil/watchdog-rollback-run-1-marker-stuck.log)
- [post-rollback-ota.json](./2026-08-28-watchdog-rollback-fault-injection-hil/post-rollback-ota.json)
- [post-rollback-fault.json](./2026-08-28-watchdog-rollback-fault-injection-hil/post-rollback-fault.json)
- [post-rollback-rpi-disks.txt](./2026-08-28-watchdog-rollback-fault-injection-hil/post-rollback-rpi-disks.txt)
- [cdc-fault-shell-final.log](./2026-08-28-watchdog-rollback-fault-injection-hil/cdc-fault-shell-final.log)
- [post-restore-ota.json](./2026-08-28-watchdog-rollback-fault-injection-hil/post-restore-ota.json)
- [post-restore-fault-endpoint.http](./2026-08-28-watchdog-rollback-fault-injection-hil/post-restore-fault-endpoint.http)
- [post-restore-fault-endpoint.json](./2026-08-28-watchdog-rollback-fault-injection-hil/post-restore-fault-endpoint.json)
- [post-restore-status.json](./2026-08-28-watchdog-rollback-fault-injection-hil/post-restore-status.json)
- [post-restore-uname.log](./2026-08-28-watchdog-rollback-fault-injection-hil/post-restore-uname.log)
- [cdc-final.log](./2026-08-28-watchdog-rollback-fault-injection-hil/cdc-final.log)
- [ws-final.log](./2026-08-28-watchdog-rollback-fault-injection-hil/ws-final.log)
- [final-restore-fault.http](./2026-08-28-watchdog-rollback-fault-injection-hil/final-restore-fault.http)
- [final-restore-ota.json](./2026-08-28-watchdog-rollback-fault-injection-hil/final-restore-ota.json)
- [final-restore-tasks.json](./2026-08-28-watchdog-rollback-fault-injection-hil/final-restore-tasks.json)
- [verdict.json](./2026-08-28-watchdog-rollback-fault-injection-hil/verdict.json)

## 边界说明

- Fault hook 仅存在于 `prj-fault.conf` / `prj-fault-candidate.conf` HIL 构建，
  canonical 生产构建不暴露 HTTP endpoint 或 shell 子命令。
- 本次 HIL 使用浏览器 runner 之外的 shell API runner 执行故障注入；浏览器
  runner 负责 auto/manual confirm 流程，并明确不声称执行 watchdog rollback。
- 本次运行集中验证 watchdog rollback 与 HTTP fault endpoint；CDC shell 无 marker
  拒绝沿用同日早前证据。HIL 后已从 BOOTSEL 恢复 canonical production 镜像，
  并完成 `/api/v1/watchdog/fault` 404、CDC `uname`/`app build-version`、
  WebSocket 快照、任务持久化/执行和 OTA auto/manual confirm 复验。
- 本次没有执行任何 `git push` 或远端写操作。
