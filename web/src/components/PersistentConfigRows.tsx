import {
  CheckCircle2,
  Circle,
  Clock3,
  HelpCircle,
  ShieldAlert,
  XCircle,
} from "lucide-react";
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
      return <Badge tone="ok"><CheckCircle2 size={12} /> {t("config.risk.safe")}</Badge>;
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
        className={`grid min-h-11 w-full min-w-0 cursor-pointer rounded-lg px-3 py-2 text-left transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40 ${checked ? "bg-brand/15 ring-1 ring-inset ring-brand/30 hover:bg-brand/20" : "hover:bg-panel2/50"} ${disabled ? "cursor-not-allowed opacity-50" : ""}`}
      >
        <span className="min-w-0">
          <span className="flex min-w-0 flex-wrap items-center gap-2">
            <span className="min-w-0 break-all font-mono text-sm font-medium text-ink">{item.id}</span>
            <RiskBadge risk={item.risk} />
            <ApplyBadge state={effectiveApplyState(item, error)} />
          </span>
          <span className="mt-2 grid min-w-0 grid-cols-2 gap-2">
            <span className="min-w-0 rounded-lg bg-panel2/50 px-2 py-1.5">
              <span className="block text-[11px] uppercase tracking-wide text-ink-dim">{t("config.current")}</span>
              <span className="block break-all font-mono text-xs text-ink">{valueText(item.current, t)}</span>
            </span>
            <span className="min-w-0 rounded-lg bg-panel2/50 px-2 py-1.5">
              <span className="block text-[11px] uppercase tracking-wide text-ink-dim">{t("config.saved")}</span>
              <span className="block break-all font-mono text-xs text-ink">{valueText(item.saved, t)}</span>
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
  if (items.length === 0) return null;
  const headingId = `persistent-config-group-${kind}`;
  return (
    <section aria-labelledby={headingId}>
      <h3 id={headingId} className="px-3 text-[11px] font-medium uppercase tracking-wide text-ink-dim">
        {t(`config.group.${kind}`)}
      </h3>
      <ul className="mt-1 space-y-1">
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
    </section>
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
    <div className="space-y-4">
      <ConfigGroup kind="power" items={groups.power} selectedIds={selectedIds} disabled={disabled} error={error} onToggle={onToggle} />
      <ConfigGroup kind="switch" items={groups.switch} selectedIds={selectedIds} disabled={disabled} error={error} onToggle={onToggle} />
      <ConfigGroup kind="gpio" items={groups.gpio} selectedIds={selectedIds} disabled={disabled} error={error} onToggle={onToggle} />
    </div>
  );
}
