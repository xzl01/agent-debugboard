export type PersistentConfigSummary = {
  readonly available: boolean;
  readonly reason: string;
  readonly savedCount: number;
  readonly pendingCount: number;
};

export type PersistentConfigValue =
  | { readonly kind: "power"; readonly state: "on" | "off" }
  | { readonly kind: "switch"; readonly route: string }
  | { readonly kind: "gpio"; readonly direction: "input" | "output"; readonly value: 0 | 1 };

type ConfigItemBase = {
  readonly id: string;
  readonly selected: boolean;
  readonly risk: "safe" | "confirmation_required" | "unknown";
  readonly applyState: "not_saved" | "applied" | "pending" | "failed" | "unknown";
};
export type PersistentConfigItem =
  | (ConfigItemBase & { readonly kind: "power"; readonly current: Extract<PersistentConfigValue, { kind: "power" }> | null; readonly saved: Extract<PersistentConfigValue, { kind: "power" }> | null })
  | (ConfigItemBase & { readonly kind: "switch"; readonly current: Extract<PersistentConfigValue, { kind: "switch" }> | null; readonly saved: Extract<PersistentConfigValue, { kind: "switch" }> | null })
  | (ConfigItemBase & { readonly kind: "gpio"; readonly current: Extract<PersistentConfigValue, { kind: "gpio" }> | null; readonly saved: Extract<PersistentConfigValue, { kind: "gpio" }> | null })
  | (ConfigItemBase & { readonly kind: "unknown"; readonly sourceKind: string; readonly current: null; readonly saved: null });
export type PersistentConfig = {
  readonly backend: { readonly available: boolean; readonly reason: string };
  readonly snapshot: { readonly present: boolean; readonly version: number | null };
  readonly pending: number;
  readonly items: readonly PersistentConfigItem[];
};
export type PersistentConfigErrorDetail =
  | { readonly kind: "confirmation_required"; readonly dangerousIds: readonly string[] }
  | { readonly kind: "busy"; readonly activity: "capture" | "ota" | "unknown" }
  | { readonly kind: "apply_failed"; readonly appliedIds: readonly string[]; readonly failedId: string | null; readonly pendingIds: readonly string[] }
  | { readonly kind: "other"; readonly code: string | null };
export class PersistentConfigApiError extends Error {
  readonly name = "PersistentConfigApiError";
  readonly detail: PersistentConfigErrorDetail;
  constructor(detail: PersistentConfigErrorDetail, message: string) { super(message); this.detail = detail; }
}
type RecordValue = Record<string, unknown>;
const isRecord = (value: unknown): value is RecordValue => typeof value === "object" && value !== null;
const isArray = (value: unknown): value is readonly unknown[] => Array.isArray(value);
const strings = (value: unknown): readonly string[] => isArray(value) ? value.filter((item): item is string => typeof item === "string") : [];
const number = (value: unknown): number | null => typeof value === "number" && Number.isInteger(value) && value >= 0 ? value : null;
const state = (value: unknown) => value === "on" || value === "off" ? value : null;
const direction = (value: unknown) => value === "input" || value === "output" ? value : null;
const risk = (value: unknown): ConfigItemBase["risk"] => value === true ? "confirmation_required" : value === false ? "safe" : "unknown";
const applyState = (value: unknown): ConfigItemBase["applyState"] => value === "not_saved" || value === "applied" || value === "pending" || value === "failed" ? value : "unknown";

function valueFor(kind: string, value: unknown): PersistentConfigValue | null {
  if (!isRecord(value)) return null;
  if (kind === "power") { const itemState = state(value.state); return itemState ? { kind, state: itemState } : null; }
  if (kind === "switch") return typeof value.route === "string" ? { kind, route: value.route } : null;
  if (kind === "gpio") { const itemDirection = direction(value.direction); const itemValue = value.value; return itemDirection && (itemValue === 0 || itemValue === 1) ? { kind, direction: itemDirection, value: itemValue } : null; }
  return null;
}
function item(value: unknown): PersistentConfigItem {
  if (!isRecord(value) || typeof value.id !== "string" || typeof value.kind !== "string") throw new PersistentConfigApiError({ kind: "other", code: "invalid_response" }, "Invalid config item");
  if (!("current" in value) || !("saved" in value) || typeof value.selected !== "boolean" || (value.requires_confirm !== true && value.requires_confirm !== false && value.requires_confirm !== null) || typeof value.apply_state !== "string") throw new PersistentConfigApiError({ kind: "other", code: "invalid_response" }, "Invalid config item");
  const base: ConfigItemBase = { id: value.id, selected: value.selected === true, risk: risk(value.requires_confirm), applyState: applyState(value.apply_state) };
  const current = value.current === null ? null : valueFor(value.kind, value.current);
  const saved = value.saved === null ? null : valueFor(value.kind, value.saved);
  if ((value.kind === "power" || value.kind === "switch" || value.kind === "gpio") && (current === null && value.current !== null || saved === null && value.saved !== null)) throw new PersistentConfigApiError({ kind: "other", code: "invalid_response" }, "Invalid config item");
  if (value.kind === "power" && (!current || current.kind === "power") && (!saved || saved.kind === "power")) return { ...base, kind: "power", current, saved };
  if (value.kind === "switch" && (!current || current.kind === "switch") && (!saved || saved.kind === "switch")) return { ...base, kind: "switch", current, saved };
  if (value.kind === "gpio" && (!current || current.kind === "gpio") && (!saved || saved.kind === "gpio")) return { ...base, kind: "gpio", current, saved };
  return { ...base, kind: "unknown", sourceKind: value.kind, current: null, saved: null };
}
function root(value: unknown, action: string): RecordValue {
  if (!isRecord(value) || value.schema !== "radxa-linkr-debugger.v1" || value.ok !== true || value.command !== "config" || value.action !== action) throw new PersistentConfigApiError({ kind: "other", code: "invalid_response" }, "Invalid config response");
  return value;
}
export function parsePersistentConfigGet(value: unknown): PersistentConfig {
  const data = root(value, "get"); const backend = isRecord(data.backend) ? data.backend : null; const snapshot = isRecord(data.snapshot) ? data.snapshot : null;
  const pending = number(data.pending);
  if (!backend || typeof backend.available !== "boolean" || typeof backend.reason !== "string" || !snapshot || typeof snapshot.present !== "boolean" || (snapshot.present ? snapshot.version !== 1 : snapshot.version !== null) || pending === null || !isArray(data.items)) throw new PersistentConfigApiError({ kind: "other", code: "invalid_response" }, "Invalid config response");
  return { backend: { available: backend.available, reason: backend.reason }, snapshot: { present: snapshot.present, version: number(snapshot.version) }, pending, items: data.items.map(item) };
}
export function parsePersistentConfigMutation(value: unknown, action: "save" | "clear"): void {
  const data = root(value, action);
  const snapshot = isRecord(data.snapshot) ? data.snapshot : null;
  const validSnapshot = snapshot !== null && typeof snapshot.present === "boolean" && (snapshot.present ? snapshot.version === 1 : snapshot.version === null);
  if (action === "save" && isArray(data.saved_items) && strings(data.saved_items).length === data.saved_items.length && isArray(data.confirmation_items) && strings(data.confirmation_items).length === data.confirmation_items.length && isArray(data.applied_items) && strings(data.applied_items).length === data.applied_items.length && validSnapshot && number(data.pending) !== null) return;
  if (action === "clear" && typeof data.noop === "boolean" && validSnapshot && number(data.pending) !== null) return;
  throw new PersistentConfigApiError({ kind: "other", code: "invalid_response" }, "Invalid config response");
}
export function parsePersistentConfigSummary(value: unknown): PersistentConfigSummary | undefined {
  if (!isRecord(value) || typeof value.available !== "boolean" || typeof value.reason !== "string") return undefined;
  const savedCount = number(value.saved_count); const pendingCount = number(value.pending_count);
  return savedCount === null || pendingCount === null || pendingCount > savedCount ? undefined : { available: value.available, reason: value.reason, savedCount, pendingCount };
}
export function mergePersistentConfigSummary(previous: PersistentConfigSummary | undefined, value: unknown): PersistentConfigSummary | undefined {
  return parsePersistentConfigSummary(value) ?? previous;
}
export function groupPersistentConfigItems(items: readonly PersistentConfigItem[]) {
  return { power: items.filter((entry): entry is Extract<PersistentConfigItem, { kind: "power" }> => entry.kind === "power"), switch: items.filter((entry): entry is Extract<PersistentConfigItem, { kind: "switch" }> => entry.kind === "switch"), gpio: items.filter((entry): entry is Extract<PersistentConfigItem, { kind: "gpio" }> => entry.kind === "gpio") };
}
export function parsePersistentConfigError(value: unknown, fallback: string, action: string): PersistentConfigApiError {
  if (value === undefined) return new PersistentConfigApiError({ kind: "other", code: null }, fallback);
  if (!isRecord(value) || value.schema !== "radxa-linkr-debugger.v1" || value.ok !== false || value.command !== "config" || value.action !== action || !isRecord(value.error) || typeof value.error.code !== "string" || typeof value.error.message !== "string") return new PersistentConfigApiError({ kind: "other", code: "invalid_response" }, "Invalid config error");
  const data = isRecord(value) ? value : {}; const error = isRecord(data.error) ? data.error : {}; const code = typeof error.code === "string" ? error.code : null; const message = typeof error.message === "string" ? error.message : fallback;
  if (code === "confirmation_required" && isArray(data.dangerous_items) && strings(data.dangerous_items).length === data.dangerous_items.length) return new PersistentConfigApiError({ kind: code, dangerousIds: strings(data.dangerous_items) }, message);
  if (code === "busy" && (data.activity === "capture" || data.activity === "ota")) return new PersistentConfigApiError({ kind: code, activity: data.activity }, message);
  if (code === "apply_failed" && isArray(data.applied_items) && isArray(data.pending_items) && strings(data.applied_items).length === data.applied_items.length && strings(data.pending_items).length === data.pending_items.length && typeof data.failed_item === "string") return new PersistentConfigApiError({ kind: code, appliedIds: strings(data.applied_items), failedId: data.failed_item, pendingIds: strings(data.pending_items) }, message);
  if (code === "confirmation_required" || code === "busy" || code === "apply_failed") return new PersistentConfigApiError({ kind: "other", code: "invalid_response" }, "Invalid config error");
  return new PersistentConfigApiError({ kind: "other", code }, message);
}
