export const HIL_REPORT_PATH = "doc/testing/results/2026-07-30-persistent-config-hil.md";
export const HIL_COMPLETION_LITERAL = "local-distinct;real-HIL-2026-07-30-pass";
export const STALE_HIL_STATUS = /(?:Todo\s*16[^.\n]{0,140}(?:pending|remains pending|待完成|尚待完成|证据尚未生成)|(?:real-hardware HIL evidence|真实硬件 HIL 证据)[^.\n]{0,100}(?:pending|尚待完成|尚未生成))/i;

export const DOC_SURFACES = Object.freeze([
  {
    path: "doc/persistent-configuration.md",
    heading: "# Persistent Configuration",
    links: ["doc/testing/hil-functional-test-spec.md", HIL_REPORT_PATH],
  },
  {
    path: "README.md",
    heading: "## Persistent Configuration",
    links: ["doc/persistent-configuration.md", HIL_REPORT_PATH],
    summaryHeading: "### Frozen Contract Summary",
  },
  {
    path: "README.zh-CN.md",
    heading: "## 持久化配置",
    links: ["doc/persistent-configuration.md", HIL_REPORT_PATH],
    summaryHeading: "### 固定契约摘要",
  },
  {
    path: "apps/radxa_linkr_debugger/README.md",
    heading: "## Persistent Configuration",
    links: ["doc/persistent-configuration.md", HIL_REPORT_PATH],
  },
  {
    path: "skills/radxa-linkr-debugger/SKILL.md",
    heading: "## Persistent Configuration",
    links: ["doc/persistent-configuration.md", HIL_REPORT_PATH],
  },
  {
    path: "doc/testing/hil-functional-test-spec.md",
    heading: "### 2d. 持久化配置",
    links: ["doc/persistent-configuration.md", HIL_REPORT_PATH],
  },
]);

export const FROZEN_SUMMARY = Object.freeze([
  ["storage", "storage_partition+Settings+NVS"],
  ["snapshot", "linkr/config/snapshot;v1;one"],
  ["explicit-save", "ordinary-setters-volatile;explicit-save-only"],
  ["boot-safe", "defaults-first;safe-auto-restore"],
  ["danger-pending", "dangerous-pending-after-boot"],
  ["firmware-confirmation", "firmware-owned-confirmation"],
  ["clear", "settings_delete;hardware-unchanged"],
  ["busy", "busy:capture|ota"],
  ["recovery", "BOOTSEL:radxa-linkr-debugger-rp2350.uf2;OTA:radxa-linkr-debugger-rp2350-ota.bin;zephyr.uf2-invalid"],
  ["security", "no-profiles;no-encryption;no-authentication-or-authorization;no-config-rollback"],
  ["hil-boundary", HIL_COMPLETION_LITERAL],
]);

export const WEB_CURRENT_SYNC_CONTRACT = Object.freeze([
  ["current-source", "Current-from-/api/v1/config"],
  ["current-trigger", "live-power/switch/GPIO-transitions-auto-refresh"],
  ["current-scope", "power-state|switch-route|GPIO-direction-value"],
  ["current-no-write", "display-sync-no-auto-save-no-flash-no-apply"],
  ["current-no-flood", "one-transition-one-refresh;identical-frames-zero-GETs"],
  ["current-draft-survives", "local-checkbox-draft-survives-refresh"],
  ["current-refresh-recovery", "Refresh-manual-recovery-not-required"],
  ["current-mutation-truthful", "save-apply-clear-pending-until-authority"],
  ["current-hil-boundary", "Todo-6-post-fix-HIL-still-required"],
]);

export const CANONICAL_HEADINGS = Object.freeze([
  "## Scope And Non-Goals",
  "## User Model",
  "## Storage Model",
  "## Firmware-Owned Catalog And Risk",
  "## Boot Restore And Apply",
  "## HTTP API",
  "## Status And WebSocket Summary",
  "## Rust CLI",
  "## Interactive TUI",
  "## Embedded Web UI",
  "## Automatic Current Synchronization",
  "## CDC ACM Fallback",
  "## Capture And OTA Exclusion",
  "## Clear, Recovery, And Firmware Update",
  "## Security Boundaries",
  "## Validation Boundaries",
  "## Source Of Truth",
]);

export const APP_HEADINGS = Object.freeze([
  "### Storage And Startup",
  "### HTTP Contract",
  "### Boot Restore And Apply Order",
  "### CDC ACM Commands",
  "### Capture And OTA Exclusion",
  "### Recovery Boundaries",
  "### Local Versus HIL Validation",
]);

export const APP_END_MARKER = "Raw MCUboot OTA API";

export const SKILL_HEADINGS = Object.freeze([
  "### Read Saved Configuration",
  "### Save Selected Current Values",
  "### Apply Pending Values",
  "### Clear Without Changing Hardware",
  "### Handle Confirmation And Busy Errors",
  "### CLI And CDC Fallback",
  "### Automatic Current Synchronization",
  "### Persistence Recovery Safety",
  "### Dry-Run And HIL Boundaries",
]);

export const HIL_HEADINGS = Object.freeze([
  "#### Todo 14 本地文档验收",
  "#### Todo 16 实机前置条件",
  "#### 安全恢复",
  "#### 危险项 pending 与固件确认",
  "#### Clear 不改变硬件",
  "#### Capture/OTA busy 排斥",
  "#### OTA 与 combined-UF2 保留",
  "#### CDC config 与 BOOTSEL fallback",
  "#### 最终安全清理",
  "#### 证据与报告",
]);

export const REQUIRED_EXAMPLES = Object.freeze({
  "curl-config-get": { kind: "curl", method: "GET", path: "/api/v1/config" },
  "curl-config-save-safe": { kind: "curl", method: "PUT", path: "/api/v1/config", body: { items: ["switch/sd"], confirm: false } },
  "curl-config-save-dangerous": { kind: "curl", method: "PUT", path: "/api/v1/config", body: { items: ["switch/usb"], confirm: true } },
  "curl-config-save-dangerous-unconfirmed": { kind: "curl", method: "PUT", path: "/api/v1/config", body: { items: ["switch/usb"], confirm: false } },
  "curl-config-apply-dangerous": { kind: "curl", method: "POST", path: "/api/v1/config/apply", body: { confirm: true } },
  "curl-config-clear": { kind: "curl", method: "DELETE", path: "/api/v1/config" },
  "cli-config-show": { kind: "cli", command: "config show" },
  "cli-config-save-safe": { kind: "cli", command: "config save switch/sd" },
  "cli-config-save-dangerous": { kind: "cli", command: "config save --confirm switch/usb" },
  "cli-config-apply": { kind: "cli", command: "config apply --confirm" },
  "cli-config-clear": { kind: "cli", command: "config clear" },
});

export const REQUIRED_LITERALS = Object.freeze([
  "http://172.29.203.1", "storage_partition", "linkr/config/snapshot", "Settings+NVS",
  "one explicit snapshot", "ordinary setters are volatile", "safe values auto-restore after defaults",
  "dangerous values remain pending", "firmware confirmation", "clear does not alter live hardware",
  "corrupt or unsupported storage falls back safely without formatting",
  "Local validation is not real-hardware HIL.", "The 2026-07-30 real-hardware HIL passed all six runner flows.",
  "radxa-linkr-debugger-rp2350.uf2", "radxa-linkr-debugger-rp2350-ota.bin",
]);

export const RESPONSE_FIELDS = Object.freeze([
  "schema", "ok", "command", "action", "backend", "snapshot", "pending", "items", "id", "kind",
  "current", "saved", "selected", "requires_confirm", "apply_state", "saved_items", "confirmation_items",
  "dangerous_items", "activity", "applied_items", "failed_item", "pending_items", "noop", "error.code", "error.message",
]);

export const ERROR_CODES = Object.freeze([
  "invalid_json", "empty_selection", "unknown_item", "duplicate_item", "confirmation_required", "item_unavailable",
  "no_snapshot", "busy", "body_too_large", "backend_unavailable", "invalid_snapshot", "unsupported_version",
  "control_capture_failed", "storage_error", "storage_write_failed", "apply_failed", "internal_error", "capture", "ota",
]);

export const FORBIDDEN_CLAIMS = Object.freeze([
  ["user-partition", /\buser_partition\b/i],
  ["app-only-bootsel", /(?:\b(?:use|flash|install|load|accept|support|valid)\b[^.\n]{0,120}\bzephyr\.uf2\b[^.\n]{0,120}\bROM[- ]?BOOTSEL\b|\bzephyr\.uf2\b[^.\n]{0,120}\b(?:use|flash|install|load|accept|support|valid)\b[^.\n]{0,120}\bROM[- ]?BOOTSEL\b|\bROM[- ]?BOOTSEL\b[^.\n]{0,120}\b(?:use|flash|install|load|accept|support|valid)\b[^.\n]{0,120}\bzephyr\.uf2\b)/i],
  ["setter-auto-persist", /\b(?:ordinary\s+)?setters?\s+(?:auto[- ]?persist|persist\s+automatically)\b/i],
  ["client-confirmation-policy", /\b(?:client|host|CLI|TUI|Web)\b[^.\n]{0,120}\b(?:owns?|enforces?|decides?|derives?)\b[^.\n]{0,120}\bconfirmation\b/i],
  ["named-profiles", /\b(?:named\s+profiles?|profiles?\s+(?:are|is)\s+(?:available|supported|provided|stored))\b/i],
  ["secure-storage", /\b(?:encrypted|secure)\s+storage\b|\bstorage\s+(?:is|uses)\s+(?:encrypted|secure)\b/i],
  ["authentication-guarantee", /\b(?:guarantees?|provides?|enforces?|requires?)\s+(?:authentication|authorization)\b|\b(?:authentication|authorization)\s+(?:is|are)\s+guaranteed\b/i],
  ["automatic-rollback", /\b(?:automatic(?:ally)?|auto)\b[^.\n]{0,80}\broll(?:s|ed|ing)?\s+back\b|\broll(?:s|ed|ing)?\s+back\b[^.\n]{0,80}\b(?:automatic(?:ally)?|auto)\b/i],
]);
