import { mkdir, mkdtemp, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import {
  ERROR_CODES, FROZEN_SUMMARY, HIL_REPORT_PATH, RESPONSE_FIELDS, SKILL_CURRENT_SYNC_CONTRACT,
  WEB_CURRENT_SYNC_CONTRACT,
} from "./contracts.mjs";

function summaryTable() {
  return ["| Contract ID | Frozen literal |", "| --- | --- |", ...FROZEN_SUMMARY.map(([id, value]) => `| \`${id}\` | \`${value.replaceAll("|", "\\|")}\` |`)].join("\n");
}

function example(id, command) {
  return `<!-- persistent-config-example: ${id} -->\n\`\`\`sh\n${command}\n\`\`\``;
}

function marked(id, language, command) {
  return `<!-- persistent-config-example: ${id} -->\n\`\`\`${language}\n${command}\n\`\`\``;
}

function currentSyncMarkerBlock(contract = WEB_CURRENT_SYNC_CONTRACT) {
  const body = contract.map(([id, value]) => `${id}:${value}`).join("\n");
  return `<!-- persistent-config-current-sync:\n${body}\n-->`;
}

function canonical(extra) {
  const fields = RESPONSE_FIELDS.join(", ");
  const errors = ERROR_CODES.join(", ");
  return `# Persistent Configuration

See the [HIL procedure](../testing/hil-functional-test-spec.md).

## Scope And Non-Goals

One explicit snapshot has no named profiles, encryption, authentication, authorization, or automatic config rollback.

## User Model

Ordinary setters are volatile; explicit save is the only persistence operation, save also applies immediately, and clear does not alter live hardware. Dangerous values require firmware confirmation at Save time only.

## Storage Model

The existing \`storage_partition\` stores one explicit snapshot at \`linkr/config/snapshot\` through \`Settings+NVS\`. It does not introduce a \`storage_partition\` Device Tree node. Corrupt or unsupported storage falls back safely without formatting. Missing key means absent and defaults. Corrupt or unsupported storage surfaces its reason and defaults. Storage recovery does not auto-format, erase, or delete; storage read and write failures remain errors. Save does not gate on the service reason and can replace a corrupt or unsupported snapshot, then sets the reason to \`ready\`. Clear is the explicit
deletion/recovery path, but it is not the only possible replacement path.

The snapshot header is exactly 12 bytes. byte 4 version 1 is the only accepted version and byte 7 zero is the only accepted restore padding. A version byte other than 1 is reported as \`unsupported_version\` and is never replayed, migrated, or auto-cleared; a v1 blob with a nonzero byte 7 is reported as \`invalid_snapshot\`. Every successful Save writes the v1 header. The maximum encoded size is 104 bytes (12-byte header + 23 entries of 4 bytes each).

## Firmware-Owned Catalog And Risk

The firmware owns item IDs and risk. Dangerous values require explicit firmware confirmation at Save time; an unconfirmed dangerous Save is rejected with \`confirmation_required\`.

## Boot Restore

Boot restores defaults first. Every structurally valid v1 snapshot replays every saved entry, including dangerous values, on every normal boot; the confirmation given at Save time is the only danger gate, so v1 full restore needs no second authorization. Save captures live values, persists the v1 snapshot, and then replays those values in the same shared order, so a confirmed Save persists and applies in one operation. Replay stops at the first hardware failure without hidden rollback: earlier rows stay applied, the failed row is reported failed, and the remaining saved rows stay pending. There is no auto-retry; retrying means repeating the confirmed Save, and capturing from live values makes the retry replay the same intended values, so failed retry via repeated save is the only retry path. A failed Save still persists the snapshot, so the next boot replays it again. An encoded snapshot \`entry_count=0\` is rejected as \`empty_selection\`. A successful save owns the complete control snapshot and clear owns the complete store deletion. 10. Saved GPIO outputs (GPIOs in output mode). The service acquires the
capture owner and then the flash owner once before persisting and replaying. GET never acquires owners.

## HTTP API

The default URL is \`http://172.29.203.1\`. Fields: ${fields}. Errors: ${errors}.

${example("curl-config-get", "curl -fsS http://172.29.203.1/api/v1/config")}

${example("curl-config-save-safe", "curl -fsS -X PUT http://172.29.203.1/api/v1/config -H 'Content-Type: application/json' --data '{\"items\":[\"switch/sd\"],\"confirm\":false}'")}

${example("curl-config-save-dangerous", "curl -fsS -X PUT http://172.29.203.1/api/v1/config -H 'Content-Type: application/json' --data '{\"items\":[\"switch/usb\"],\"confirm\":true}'")}

${example("curl-config-clear", "curl -fsS -X DELETE http://172.29.203.1/api/v1/config")}

${example("curl-config-extra-read", "curl -fsS http://172.29.203.1/api/v1/config")}

## Status And WebSocket Summary

The summary reports available, reason, saved_count, and pending_count and does not emit
\`backend_unavailable\` as a summary reason. It contains exactly \`available\`, \`reason\`, \`saved_count\`, and \`pending_count\`; firmware may omit the config summary fragment on encode failure. Web preserves the last valid summary when the next WS value is absent or malformed. Rust TUI clears support, focus, and confirmation when \`snapshot.config\` is \`None\`.

## Rust CLI

${example("cli-config-show", "radxa-linkr-debuggerctl --json config show")}
${example("cli-config-save-safe", "radxa-linkr-debuggerctl --json config save switch/sd")}
${example("cli-config-save-dangerous", "radxa-linkr-debuggerctl --json config save --confirm switch/usb")}
${example("cli-config-clear", "radxa-linkr-debuggerctl --json config clear")}
${example("cli-config-extra-show", "radxa-linkr-debuggerctl --json config show")}

The CLI strictly binds the expected \`action\` and HTTP
status before rendering.

## Interactive TUI

The TUI uses \`ConfigWorker\` as a bounded
background worker and renders firmware-enumerated saved configuration. \`r\` refreshes HTTP status and requests an authoritative config GET. While focused,
\`c\` or \`Esc\` blurs it.

## Embedded Web UI

The Web card source is under repository \`web/\` and renders firmware-enumerated saved configuration.

## Automatic Current Synchronization

Current is firmware-authoritative data retrieved from \`/api/v1/config\`; WS live-control observations trigger the bounded refresh but are not client-side overlays. Observed live firmware-enumerated power \`state\`, switch \`route\`, and GPIO \`direction-value\` changes auto-refresh the Current column. Identifiers and names are supplied by firmware rather than a host-side catalog. One actual relevant value transition causes exactly one Current refresh; identical, reordered, or unrelated telemetry frames do not trigger additional config GETs. Display synchronization does not write flash, change the saved snapshot, apply pending values, or auto-persist ordinary setters. Local unsaved item-selection checkbox drafts survive ordinary Current synchronization. Save and clear remain pending until the latest authoritative config response commits. \`Refresh\` is a manual recovery or retry action after a transient failed request or suspected stale UI, not a required step. Local validation is not real-hardware HIL; Todo 6 post-fix real-hardware HIL remains required for this code change until executed.

${currentSyncMarkerBlock()}

## CDC ACM Fallback

CDC grammar: \`config show\`, \`config save [--confirm] <firmware-item-id>...\`, \`config clear\`. The primary result or error line precedes \`confirmation_id\`, \`applied_id\`, \`failed_id\`, and \`pending_id\` detail lines; prompt or echo is not a result.

${marked("cdc-config-show", "console", "linkr-debugger:~$ config show")}

## Capture And OTA Exclusion

Mutations report \`busy\` while capture or OTA owns the resource.

## Clear, Recovery, And Firmware Update

The app-only \`zephyr.uf2\` is invalid for ROM BOOTSEL; use \`radxa-linkr-debugger-rp2350.uf2\`. OTA uses \`radxa-linkr-debugger-rp2350-ota.bin\`. BOOTSEL scratch index 0 marker \`0xadb00751\` and OTA-test scratch index 2 marker \`0x07a7e571\` are independent of Settings snapshot.

## Security Boundaries

This feature does not provide encrypted or secure storage, authentication, or authorization.

## Validation Boundaries

Local validation is not real-hardware HIL. The 2026-08-05 real-hardware HIL passed the v1 save-and-apply flow. See the [dated v1-save HIL report](../testing/results/2026-08-05-persistent-config-v1-save-hil.md). The historical 2026-07-30 real-hardware HIL passed all six runner flows; see the [historical six-flow report](../testing/results/2026-07-30-persistent-config-hil.md). Future local tests remain distinct from board HIL.

## Source Of Truth

Firmware source, tests, and the repository-local skill are authoritative.
${extra}`;
}

export function fixtureDocuments(extra = "") {
  const table = summaryTable();
  return {
    "docs/reference/persistent-configuration.md": canonical(extra),
    "README.md": `# Root\n\n## Persistent Configuration\n\nSee [canonical](docs/reference/persistent-configuration.md). Local validation is not real-hardware HIL. The 2026-08-05 real-hardware HIL passed the v1 save-and-apply flow; see the [dated v1-save HIL report](docs/testing/results/2026-08-05-persistent-config-v1-save-hil.md). The historical 2026-07-30 real-hardware HIL passed all six runner flows; see the [historical six-flow report](docs/testing/results/2026-07-30-persistent-config-hil.md). Future local tests remain distinct from board HIL.\n\n### Frozen Contract Summary\n\n${table}\n\n## Other\n\nNamed profiles are available and authentication is guaranteed elsewhere.`,
    "README.zh-CN.md": `# 根\n\n## 持久化配置\n\n见[规范](docs/reference/persistent-configuration.md)。本地验证不等于真实硬件 HIL。2026-08-05 真实硬件 HIL 通过 v1 save-and-apply flow；见[日期 v1-save HIL 报告](docs/testing/results/2026-08-05-persistent-config-v1-save-hil.md)。历史 2026-07-30 真实硬件 HIL 六个 runner flow 全部通过；见[历史六 flow 报告](docs/testing/results/2026-07-30-persistent-config-hil.md)。未来本地测试仍不能替代板级 HIL。\n\n### 固定契约摘要\n\n${table}`,
    "apps/radxa_linkr_debugger/README.md": `# App\n\n\`curl -fsS http://172.29.203.1/api/v1/status\`\n\n## Persistent Configuration\n\nSee [canonical](../../docs/reference/persistent-configuration.md).\n\n### Storage And Startup\n### HTTP Contract\n\nThe only common response fields are \`schema\`, \`ok\`, \`command\`, and \`action\`. Successful \`get\` adds \`backend\`, \`snapshot\`, \`pending\`, and \`items\`. Successful \`save\` adds \`saved_items\`,\n\`confirmation_items\`, \`applied_items\`, \`snapshot\`, and numeric \`pending\`. Successful\n\`clear\` adds \`noop\`, \`snapshot\`, and numeric \`pending\`. A \`confirmation_required\` error lists \`dangerous_items\`. A \`busy\` error carries \`activity\`. An \`apply_failed\` error carries \`applied_items\`, \`failed_item\`, and \`pending_items\`.\n\n### Boot Restore And Replay Order\n### CDC ACM Commands\n### Capture And OTA Exclusion\n### Recovery Boundaries\n### Local Versus HIL Validation\n\nLocal validation is not real-hardware HIL. The 2026-08-05 real-hardware HIL passed the v1 save-and-apply flow; see the [dated v1-save HIL report](../../docs/testing/results/2026-08-05-persistent-config-v1-save-hil.md). The historical 2026-07-30 real-hardware HIL passed all six runner flows; see the [historical six-flow report](../../docs/testing/results/2026-07-30-persistent-config-hil.md). Future local tests remain distinct from board HIL.\n\nRaw MCUboot OTA API\n\n\`\`\`sh\ncurl -fsS http://172.29.203.1/api/v1/ota\n\`\`\``,
    "skills/radxa-linkr-debugger/SKILL.md": `# Skill\n\n## JSON Contract\n\n## Persistent Configuration\n\nSee [canonical](../../docs/reference/persistent-configuration.md).\n\n### Read Saved Configuration\n### Save Selected Current Values\n### Clear Without Changing Hardware\n### Handle Confirmation And Busy Errors\n\nPreserve the non-2xx JSON body. Do not auto-confirm a dangerous save.\n\n${example("curl-config-save-dangerous-unconfirmed", "set +e\nresponse=\"$(curl --fail-with-body -sS -X PUT http://172.29.203.1/api/v1/config -H 'Content-Type: application/json' --data '{\"items\":[\"switch/usb\"],\"confirm\":false}')\"\ncurl_status=$?\nset -e\n[ \"$curl_status\" -eq 22 ]\nprintf '%s\\n' \"$response\"")}\n\n### CLI And CDC Fallback\n\n### Automatic Current Synchronization

The Web Saved Config \`Current\` column is firmware-authoritative data from \`/api/v1/config\`. Live firmware-enumerated control changes auto-refresh Current; identical, reordered, or unrelated WS frames do not cause additional config GETs. Automatic Current sync does not write flash, change the saved snapshot, or auto-persist ordinary setters. Local unsaved checkbox drafts survive Current refresh. \`Refresh\` is a manual recovery or retry action after a transient failure or suspected stale UI, not a required normal step.

${currentSyncMarkerBlock(SKILL_CURRENT_SYNC_CONTRACT)}

### Persistence Recovery Safety\n\nUse the canonical contract for snapshot semantics. The snapshot lives under \`storage_partition\` through Settings+NVS at \`linkr/config/snapshot\`; \`config clear\` removes the snapshot only.\n\n## Common Commands`,
    "docs/testing/hil-functional-test-spec.md": [
      "# HIL", "", "### 2c. 强制门户发现", "", "### 2d. 持久化配置", "",
      "See [canonical](../reference/persistent-configuration.md). 本地验证不等于真实硬件 HIL。2026-08-05 真实硬件 HIL 通过 v1 save-and-apply flow；见[日期 v1-save HIL 报告](results/2026-08-05-persistent-config-v1-save-hil.md)。历史 2026-07-30 真实硬件 HIL 六个 runner flow 全部通过；见[历史六 flow 报告](results/2026-07-30-persistent-config-hil.md)。未来本地测试仍不能替代板级 HIL。", "",
       "#### Todo 14 本地文档验收", "#### Todo 16 实机前置条件",
       "sh skills/radxa-linkr-debugger/scripts/config-persistence-hil.sh --dry-run safe-reboot",
       "Use --serial <identified-cdc-device>. config-persistence-hil.sh --execute --url http://172.29.203.1 all --confirm-dangerous-save radxa-linkr-debugger-rp2350.uf2 radxa-linkr-debugger-rp2350-ota.bin. CH347 target UART 不是持久化",
      "配置的前置条件。", "#### 安全恢复", "switch/sd=usb-reader, switch/tf_wp=protected, switch/sd=target, switch/tf_wp=writable.",
      "#### 危险项 pending 与固件确认", "危险项必须精确选择非启动默认值 `switch/usb=pc`。未确认 save 返回 HTTP 409 confirmation_required 必须包含 dangerous_items; Save 即保存并立即应用, 部分失败时 `apply_failed` 列出 `applied_items`、`failed_item` 和 `pending_items`. 成功 save 的断言字段是 `saved_items`、`confirmation_items`、`applied_items`、`snapshot` 和数值 `pending`; `pending_items` 只属于 `apply_failed` 的部分执行结果.",
      "#### dangerous-auto-restore 与固件确认", "dangerous-auto-restore 是当前固件上唯一在 `all` runner 中验证危险项的真实硬件 flow, consecutive 2 次重启的 v1 自动重放是核心契约. 一次确认的危险 Save 保存非启动默认值 `switch/usb=pc` 并写 v1; snapshot.version == 1, pending == 0; 连续两次重启之后 current 与 saved 都为 pc 且 apply_state 仍为 applied.",
      "#### Clear 不改变硬件", "#### Capture/OTA busy 排斥", "逻辑分析仪或 sigrok capture arbiter; activity=capture; activity=ota. GET 从不获取 owner。Active capture and active OTA each make save and clear return busy with `activity=capture` or `activity=ota`. Each OTA upload uses --limit-rate 64K. 每个已准备状态记录",
      "准确且有界的 HTTP 结果。",
      "#### OTA 与 combined-UF2 保留", "#### CDC config 与 BOOTSEL fallback", "#### 最终安全清理",
      "switch/vin=3.3v; GPIO 都为 input; power output 都为", "off. Enumerate controllable power outputs to off and output GPIOs to input, then GET-validate both.", "#### 证据与报告", "", "### 3. 电源输出 get/set",
    ].join("\n"),
    [HIL_REPORT_PATH]: "# 2026-08-05 Persistent Configuration v1 Save HIL\n\nPASS\n",
    "docs/testing/results/2026-07-30-persistent-config-hil.md": "# 2026-07-30 Persistent Configuration HIL\n\nPASS\n",
  };
}

export async function withFixture(callback, documents = fixtureDocuments()) {
  const root = await mkdtemp(path.join(os.tmpdir(), "persistent-config-docs-"));
  try {
    await Promise.all(Object.entries(documents).map(async ([relativePath, content]) => {
      const target = path.join(root, relativePath);
      await mkdir(path.dirname(target), { recursive: true });
      await writeFile(target, content);
    }));
    return await callback(root);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
}

export const FORBIDDEN_MUTATIONS = Object.freeze([
  ["user_partition", "The user_partition stores persistent configuration."],
  ["app-only ROM BOOTSEL", "Use app-only zephyr.uf2 for ROM BOOTSEL."],
  ["ordinary setter auto-persist", "Ordinary setters auto-persist every control change."],
  ["client confirmation ownership", "The client owns the confirmation policy."],
  ["named profiles", "Named profiles are available."],
  ["secure storage", "The snapshot uses encrypted secure storage."],
  ["authentication guarantee", "The API guarantees authentication and authorization."],
  ["automatic rollback", "Apply automatically rolls back on failure."],
]);
