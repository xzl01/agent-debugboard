import { clauseAround, negated, normalizeShell, shellBlocks } from "./markdown.mjs";
import { STALE_HIL_STATUS } from "./contracts.mjs";

function requireFacts(content, facts, code, surface, failures) {
  for (const [name, expression] of facts) {
    if (!expression.test(content)) failures.push({ code, surface, detail: `missing ${name}` });
  }
}

export function checkHilCompletionFacts(content, surface, failures) {
  if (STALE_HIL_STATUS.test(content)) {
    failures.push({ code: "stale-hil-status", surface, detail: "stale Todo 16 pending wording remains after dated HIL completion" });
  }
  if (!/(?:Local\s+validation\s+is\s+not\s+real-hardware\s+HIL|本地验证不等于真实硬件\s*HIL)/i.test(content)) {
    failures.push({ code: "hil-boundary", surface, detail: "missing local-validation versus real-HIL distinction" });
  }
  if (!/2026-08-05[\s\S]{0,180}(?:PASS|passed|通过)/i.test(content)) {
    failures.push({ code: "hil-completion", surface, detail: "missing dated 2026-08-05 v1-save real-HIL PASS verdict" });
  }
  if (/v1[- ]save[^.\n]{0,200}board[- ]?HIL[^.\n]{0,80}(?:pending|remains pending|待完成|尚待完成)|board[- ]?HIL[^.\n]{0,40}v1[- ]save[^.\n]{0,80}(?:pending|remains pending|待完成|尚待完成)/i.test(content)) {
    failures.push({ code: "hil-completion", surface, detail: "v1-save board-HIL-pending wording remains after 2026-08-05 v1-save PASS" });
  }
}

export function checkCanonicalFacts(content, surface, failures) {
  requireFacts(content, [
    ["save owner order", /acquires the\n?capture owner and then the flash owner once before persisting and replaying/i],
    ["save-clear ownership", /successful save[\s\S]{0,140}complete control snapshot[\s\S]{0,180}clear[\s\S]{0,140}complete store deletion/i],
    ["clear recovery", /`?clear`? is the explicit\s+deletion\/recovery (?:path|operation)/i],
    ["save replaces corrupt snapshot", /save[\s\S]{0,180}does not gate[\s\S]{0,220}replace a corrupt or unsupported snapshot[\s\S]{0,100}`ready`/i],
    ["clear is not sole replacement", /clear[\s\S]{0,180}not the only possible replacement path/i],
    ["missing storage defaults", /missing\s+key means absent and defaults/i],
    ["corrupt storage defaults", /corrupt or unsupported storage surfaces its\s+reason and defaults/i],
    ["storage does not auto-delete", /does not auto-format, erase, or delete/i],
    ["storage failures remain errors", /storage read and write failures remain errors/i],
    ["empty encoded snapshot", /encoded snapshot `entry_count=0` is rejected as `empty_selection`/i],
    ["saved GPIO output stage", /10\. Saved GPIO outputs \(GPIOs in output mode\)/i],
    ["save persists and applies", /Save captures live values, persists the v1 snapshot, and then replays/i],
    ["boot full restore", /replays every saved entry, including dangerous values, on every normal boot/i],
    ["v1 only version", /version byte other than 1[\s\S]{0,140}never replayed, migrated, or auto-cleared/i],
    ["byte7 zero", /byte 7 zero/i],
    ["first failure stop", /stops at the first hardware failure/i],
    ["retry via resave", /[Rr]etrying means repeating the confirmed Save/i],
    ["failed save persists", /[Aa] failed Save still persists the snapshot/i],
    ["existing partition", /does not introduce a `storage_partition` Device Tree node/i],
    ["summary reason mapping", /does not emit\n?`backend_unavailable` as a summary reason/i],
    ["CLI status binding", /strictly binds the expected `action`[\s\S]{0,220}HTTP\n?status/i],
    ["TUI worker", /`ConfigWorker` as a bounded\n?background worker/i],
    ["TUI refresh key", /`r` refreshes HTTP status and requests an authoritative config GET/i],
    ["TUI blur keys", /While focused,\n?\s*`c` or `Esc` blurs it/i],
    ["Web repository path", /source is under repository `web\/`/i],
    ["WS exact fields", /(?:exactly `available`, `reason`, `saved_count`, and `pending_count`|contains exactly\s+four fields:\s+`available`, `reason`, `saved_count`, and `pending_count`)/i],
    ["WS encode omission", /firmware\s+may omit the config summary fragment on encode failure/i],
    ["Web WS retention", /Web preserves the last valid summary when the next WS value is absent or\s+malformed/i],
    ["Rust TUI WS difference", /Rust TUI clears support, focus, and confirmation when\s+`snapshot\.config` is `None`/i],
    ["CDC primary line order", /primary result or error line precedes `confirmation_id`, `applied_id`,\s+`failed_id`, and `pending_id` detail lines/i],
    ["CDC prompt is not a result", /prompt or echo is not a result/i],
    ["recovery marker identities", /BOOTSEL scratch index 0 marker `0xadb00751`[\s\S]{0,120}OTA-test scratch index 2 marker\s+`0x07a7e571`[\s\S]{0,120}independent of Settings snapshot/i],
    ["GET owner-free", /GET never acquires owners/i],
  ], "source-contract", surface, failures);
  for (const expression of [
    /re-acquires the capture owner/i,
    /does not run a background worker/i,
    /apps\/radxa_linkr_debugger\/web\//i,
    /clear path is the only path/i,
    /service refuses to overwrite/i,
    /10\. Safe GPIO outputs/i,
    /Current is (?:overlaid|computed|derived) from (?:status|WebSocket|WS) values/i,
    /(?:auto|automatically)[\s-]+(?:saves?|writes? flash|persists?|refreshes?)\s+(?:the\s+|every\s+|current\s+)?(?:current\s+value|snapshot|flash)/i,
    /Auto-?save every/i,
    /Inventory from(?:out)? (?:status|WebSocket)[\s\S]{0,80}overlaying/i,
    /\bboot-safe\b/i,
    /\bAUTO_RESTORE_ALL\b/i,
    /Save, apply, and clear/i,
  ]) {
    const match = expression.exec(content);
    if (match && !negated(content, match.index)) {
      failures.push({ code: "source-contract", surface, detail: `forbidden drift ${expression}` });
    }
  }
}

export function checkApplicationFacts(content, surface, failures) {
  requireFacts(content, [
    ["common action fields", /only common response fields are `schema`, `ok`, `command`, and `action`/i],
    ["get action fields", /successful `get` adds `backend`, `snapshot`, `pending`, and `items`/i],
    ["save action fields", /successful `save` adds `saved_items`,\n?`confirmation_items`, `applied_items`, `snapshot`, and numeric `pending`/i],
    ["clear action fields", /successful\n?`clear` adds `noop`, `snapshot`, and numeric `pending`/i],
    ["confirmation error fields", /`confirmation_required`[\s\S]{0,120}`dangerous_items`/i],
    ["busy error fields", /`busy` error carries\s+`activity`/i],
    ["apply failure fields", /`apply_failed` error carries `applied_items`, `failed_item`,\s+and `pending_items`/i],
  ], "action-fields", surface, failures);
  if (/Every response carries[\s\S]{0,100}`backend`/i.test(content)) {
    failures.push({ code: "action-fields", surface, detail: "common fields must not include GET-only fields" });
  }
}

export function checkSkillFacts(content, surface, failures) {
  requireFacts(content, [
    ["fail-with-body", /curl --fail-with-body/i],
    ["expected curl exit", /\[ "\$curl_status" -eq 22 \]/],
    ["preserved error body", /printf '%s\\n' "\$response"/],
    ["no auto-confirm", /do not auto-confirm/i],
    ["skill local validation distinct", /(?:local|Local) (?:checker|mock|fixture|test|validation)s? (?:is |are )?not real-hardware HIL/i],
  ], "skill-policy", surface, failures);
  for (const expression of [
    /(?:auto|automatically)\s+(?:Current (?:sync|refresh|synchronization)|writes?|saves?|persists?)/i,
    /Inventory from(?:out)? (?:status|WebSocket)[\s\S]{0,80}overlaying/i,
    /(?:Web|web) (?:invents?|owns?) (?:the )?Current column/i,
    /(?:config|saved) snapshot is (?:updated|saved|written)[\s\S]{0,80}(?:automatic(?:ally)?|auto|on each|every)[\s\S]{0,80}(?:Current|refresh)/i,
  ]) {
    const match = expression.exec(content);
    if (match && !negated(content, match.index)) {
      failures.push({ code: "skill-policy", surface, detail: `forbidden drift ${expression}` });
    }
  }
}

const HIL_FACTS = Object.freeze([
  ["explicit CDC serial", /--serial <identified-cdc-device>/],
  ["future all runner", /config-persistence-hil\.sh --execute --url http:\/\/172\.29\.203\.1/],
  ["dangerous save confirmation flag", /--confirm-dangerous-save/],
  ["combined UF2", /radxa-linkr-debugger-rp2350\.uf2/],
  ["OTA image", /radxa-linkr-debugger-rp2350-ota\.bin/],
  ["no CH347 prerequisite", /CH347 target UART 不是持久化\s+配置的前置条件/],
  ["safe SD restore", /switch\/sd=usb-reader/],
  ["safe TF-WP restore", /switch\/tf_wp=protected/],
  ["final SD target", /switch\/sd=target/],
  ["final TF-WP writable", /switch\/tf_wp=writable/],
  ["dangerous USB non-default", /(?:exact `switch\/usb` `pc`|非启动默认值 `switch\/usb=pc`)/],
  ["dangerous save 409", /未确认 save 返回 HTTP 409/],
  ["save response fields", /`saved_items`、`confirmation_items`、`applied_items`、`snapshot` 和\n?数值 `pending`/],
  ["dangerous auto restore flow", /(?:dangerous-auto-restore|dangerous\s+auto\s+restore)/i],

  ["dangerous auto restore consecutive reboots", /(?:dangerous[- ]?auto[- ]?restore[\s\S]{0,400}?(?:连续\s*(?:两|2)\s*次|consecutive (?:two|2)))/i],
  ["dangerous auto restore snapshot v1", /(?:dangerous[- ]?auto[- ]?restore[\s\S]{0,400}?snapshot\.version(?:\s*(?:==|为|=|等于)\s*1|= 1|=1))/i],
  ["dangerous auto restore pending zero", /(?:dangerous[- ]?auto[- ]?restore[\s\S]{0,400}?pending(?:\s*(?:==|为|=)\s*0|= 0|=0))/i],
  ["capture arbiter", /逻辑分析仪或 sigrok capture arbiter/],
  ["capture busy activity", /activity=capture/],
  ["OTA busy activity", /activity=ota/],
  ["GET has no owners", /GET 从不获取 owner/],
  ["bounded prepared states", /(?:每个已准备状态记录\s*准确且有界的 HTTP 结果|为这些状态记录准确且有界的 HTTP 结果)/],
  ["active capture and OTA save clear", /(?:active capture and active OTA each make save and clear return busy with `activity=capture` or `activity=ota`|capture 或 OTA 分别活跃时，save 与 clear 都必须返回 busy，`activity` 分别为 `capture` 与 `ota`)/i],
  ["paced OTA upload", /--limit-rate 64K/],
  ["enumerated cleanup", /(?:enumerate controllable power outputs to off and output GPIOs to input, then GET-validate both|枚举所有可控 power output 并逐个关闭[\s\S]{0,100}direction 为\s*output 的 GPIO[\s\S]{0,100}再次 GET 读取并验证两类状态)/i],
  ["final VIN", /switch\/vin=3\.3v/],
  ["final GPIO", /GPIO 都为 input/],
  ["final power", /power output 都为\n?off/],
]);

const HIL_SHELL = new Set([
  "node scripts/check-persistent-configuration-docs.mjs --root .",
  "node --test scripts/check-persistent-configuration-docs.test.mjs",
  "sh skills/radxa-linkr-debugger/scripts/config-persistence-hil.sh --dry-run safe-reboot",
]);

export function checkHilFacts(content, surface, failures) {
  requireFacts(content, HIL_FACTS, "hil-policy", surface, failures);
  if (/\/dev\/ttyACM\d+/i.test(content)) failures.push({ code: "hil-fixed-tty", surface, detail: "use an identified --serial device instead of a fixed tty" });
  for (const block of shellBlocks(content)) {
    if (!HIL_SHELL.has(normalizeShell(block.command))) {
      failures.push({ code: "hil-executable", surface, detail: `non-local shell block at section line ${block.line}` });
    }
  }
}
