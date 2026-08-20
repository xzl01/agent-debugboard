import {
  Check,
  CheckCircle2,
  ChevronRight,
  Circle,
  Clock3,
  HelpCircle,
  ShieldAlert,
  XCircle,
} from "lucide-react";
import { useState } from "react";
import { Badge } from "./ui";
import { useI18n } from "@/lib/i18n";
import type {
  PersistentConfigApiError,
  PersistentConfigItem,
  PersistentConfigValue,
  groupPersistentConfigItems,
} from "@/lib/persistentConfig";

type ConfigGroups = ReturnType<typeof groupPersistentConfigItems>;
type ApplyState = PersistentConfigItem["applyState"];
type Tone = "neutral" | "ok" | "warn" | "danger";
const APPLY_PRESENTATIONS: Record<ApplyState, { readonly tone: Tone; readonly icon: typeof Circle }> = {
  not_saved: { tone: "neutral", icon: Circle },
  applied: { tone: "ok", icon: CheckCircle2 },
  pending: { tone: "warn", icon: Clock3 },
  failed: { tone: "danger", icon: XCircle },
  unknown: { tone: "neutral", icon: HelpCircle },
};

function assertNever(value: never): never {
  throw new TypeError(`Unexpected persistent config value: ${JSON.stringify(value)}`);
}

function valueText(
  value: PersistentConfigValue | null,
  t: (key: string, params?: Record<string, string | number>) => string
): string {
  if (value === null) return t("config.value.none");
  switch (value.kind) {
    case "power":
      return t(`config.value.power.${value.state}`);
    case "switch":
      return value.route;
    case "gpio":
      return t("config.value.gpio", {
        direction: t(`config.value.gpio.${value.direction}`),
        value: value.value,
      });
    default:
      return assertNever(value);
  }
}

function effectiveApplyState(
  item: PersistentConfigItem,
  error: PersistentConfigApiError | null
): ApplyState {
  if (error?.detail.kind !== "apply_failed") return item.applyState;
  if (error.detail.failedId === item.id) return "failed";
  if (error.detail.appliedIds.includes(item.id)) return "applied";
  if (error.detail.pendingIds.includes(item.id)) return "pending";
  return item.applyState;
}

function ApplyBadge({ state }: { readonly state: ApplyState }) {
  const { t } = useI18n();
  if (state === "not_saved") {
    return <span className="text-[11px] font-medium text-ink-dim">{t("config.applyState.not_saved")}</span>;
  }
  const presentation = APPLY_PRESENTATIONS[state];
  const Icon = presentation.icon;
  return (
    <Badge tone={presentation.tone}>
      <Icon size={12} /> {t(`config.applyState.${state}`)}
    </Badge>
  );
}

function RiskBadge({ risk }: { readonly risk: PersistentConfigItem["risk"] }) {
  const { t } = useI18n();
  switch (risk) {
    case "confirmation_required":
      return <Badge tone="warn"><ShieldAlert size={12} /> {t("config.risk.confirmation")}</Badge>;
    case "unknown":
      return <Badge tone="neutral"><HelpCircle size={12} /> {t("config.risk.unknown")}</Badge>;
    case "safe":
      return <span className="sr-only">{t("config.risk.safe")}</span>;
    default:
      return assertNever(risk);
  }
}

function ConfigRow({
  item,
  checked,
  disabled,
  error,
  onToggle,
}: {
  readonly item: Exclude<PersistentConfigItem, { kind: "unknown" }>;
  readonly checked: boolean;
  readonly disabled: boolean;
  readonly error: PersistentConfigApiError | null;
  readonly onToggle: (id: string) => void;
}) {
  const { t } = useI18n();
  return (
    <li>
      <button
        type="button"
        role="checkbox"
        value={item.id}
        aria-checked={checked}
        aria-disabled={disabled}
        aria-label={t("config.select", { id: item.id })}
        onClick={() => {
          if (!disabled) onToggle(item.id);
        }}
        className={`flex min-h-14 w-full min-w-0 cursor-pointer items-start gap-2.5 rounded-lg px-2.5 py-2 text-left transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40 ${checked ? "bg-brand/15 ring-1 ring-inset ring-brand/30 hover:bg-brand/20" : "hover:bg-panel2/60"} ${disabled ? "cursor-not-allowed opacity-50" : ""}`}
      >
        <span
          aria-hidden="true"
          className={`mt-0.5 grid h-5 w-5 shrink-0 place-items-center rounded-md border transition-colors ${checked ? "border-brand bg-brand text-on-brand" : "border-line bg-panel"}`}
        >
          {checked && <Check size={13} strokeWidth={3} />}
        </span>
        <span className="min-w-0 flex-1">
          <span className="flex min-w-0 items-start justify-between gap-2">
            <span className="min-w-0 break-all font-mono text-xs font-semibold leading-5 text-ink">{item.id}</span>
            <span className="flex shrink-0 flex-wrap items-center justify-end gap-1.5">
              <RiskBadge risk={item.risk} />
              <ApplyBadge state={effectiveApplyState(item, error)} />
            </span>
          </span>
          <span className="mt-1 flex min-w-0 flex-wrap items-center gap-x-2 gap-y-1 text-[11px]">
            <span className="min-w-0">
              <span className="text-ink-dim">{t("config.current")}</span>{" "}
              <span className="break-all font-mono font-medium text-ink">{valueText(item.current, t)}</span>
            </span>
            <span aria-hidden="true" className="text-line">→</span>
            <span className="min-w-0">
              <span className="text-ink-dim">{t("config.saved")}</span>{" "}
              <span className="break-all font-mono font-medium text-ink">{valueText(item.saved, t)}</span>
            </span>
          </span>
        </span>
      </button>
    </li>
  );
}

function ConfigGroup({
  kind,
  items,
  selectedIds,
  disabled,
  error,
  onToggle,
}: {
  readonly kind: keyof ConfigGroups;
  readonly items: readonly Exclude<PersistentConfigItem, { kind: "unknown" }>[];
  readonly selectedIds: ReadonlySet<string>;
  readonly disabled: boolean;
  readonly error: PersistentConfigApiError | null;
  readonly onToggle: (id: string) => void;
}) {
  const { t } = useI18n();
  const [expanded, setExpanded] = useState(kind !== "gpio");
  if (items.length === 0) return null;
  const headingId = `persistent-config-group-${kind}`;
  const selectedCount = items.filter((item) => selectedIds.has(item.id)).length;
  return (
    <details
      className="group border-t border-line/60 first:border-t-0"
      open={expanded}
      onToggle={(event) => setExpanded(event.currentTarget.open)}
    >
      <summary className="flex min-h-11 cursor-pointer list-none items-center gap-2 px-3 py-2 text-left transition-colors hover:bg-panel2/60 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-inset focus-visible:ring-brand/40">
        <ChevronRight size={15} className="shrink-0 text-ink-dim transition-transform duration-150 group-open:rotate-90" />
        <h3 id={headingId} className="min-w-0 flex-1 text-sm font-medium text-ink">
          {t(`config.group.${kind}`)}
        </h3>
        {selectedCount > 0 && <span className="text-[11px] font-medium text-brand">{t("config.group.selected", { count: selectedCount })}</span>}
        <span className="rounded-full bg-panel2 px-2 py-0.5 text-[11px] tabular-nums text-ink-dim">{t("config.group.items", { count: items.length })}</span>
      </summary>
      <ul aria-labelledby={headingId} className="space-y-1 border-t border-line/50 p-2">
        {items.map((item) => (
          <ConfigRow
            key={item.id}
            item={item}
            checked={selectedIds.has(item.id)}
            disabled={disabled}
            error={error}
            onToggle={onToggle}
          />
        ))}
      </ul>
    </details>
  );
}

export function PersistentConfigRows({
  groups,
  selectedIds,
  disabled,
  error,
  onToggle,
}: {
  readonly groups: ConfigGroups;
  readonly selectedIds: ReadonlySet<string>;
  readonly disabled: boolean;
  readonly error: PersistentConfigApiError | null;
  readonly onToggle: (id: string) => void;
}) {
  return (
    <div className="overflow-hidden rounded-xl border border-line/70">
      <ConfigGroup kind="power" items={groups.power} selectedIds={selectedIds} disabled={disabled} error={error} onToggle={onToggle} />
      <ConfigGroup kind="switch" items={groups.switch} selectedIds={selectedIds} disabled={disabled} error={error} onToggle={onToggle} />
      <ConfigGroup kind="gpio" items={groups.gpio} selectedIds={selectedIds} disabled={disabled} error={error} onToggle={onToggle} />
    </div>
  );
}
