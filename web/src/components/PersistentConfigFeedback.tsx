import {
  CircleAlert,
  Loader2,
  ShieldAlert,
  TriangleAlert,
  Unplug,
} from "lucide-react";
import { useI18n } from "@/lib/i18n";
import type { PersistentConfigApiError } from "@/lib/persistentConfig";

function idList(ids: readonly string[]): string {
  return ids.join(", ");
}

function assertNever(value: never): never {
  throw new TypeError(`Unexpected persistent config error: ${JSON.stringify(value)}`);
}

export function PersistentConfigFeedback({
  connected,
  loading,
  hasConfig,
  error,
}: {
  readonly connected: boolean;
  readonly loading: boolean;
  readonly hasConfig: boolean;
  readonly error: PersistentConfigApiError | null;
}) {
  const { t } = useI18n();
  if (!connected) {
    return (
      <div className="flex gap-2 rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
        <Unplug size={14} className="mt-0.5 shrink-0" />
        <span>{t(hasConfig ? "config.disconnected.stale" : "config.disconnected.empty")}</span>
      </div>
    );
  }
  if (loading && hasConfig) {
    return (
      <div className="flex gap-2 rounded-lg border border-brand/30 bg-brand/10 px-3 py-2 text-xs text-brand">
        <Loader2 size={14} className="shrink-0 animate-spin" />
        <span>{t("config.refreshing")}</span>
      </div>
    );
  }
  if (!error) return null;

  switch (error.detail.kind) {
    case "busy":
      return (
        <div className="flex gap-2 rounded-lg border border-warn/30 bg-warn/10 px-3 py-2 text-xs text-warn">
          <CircleAlert size={14} className="mt-0.5 shrink-0" />
          <span>{t(`config.error.busy.${error.detail.activity}`)}</span>
        </div>
      );
    case "confirmation_required":
      return (
        <div className="rounded-lg border border-warn/30 bg-warn/10 px-3 py-2 text-xs text-warn">
          <div className="flex gap-2"><ShieldAlert size={14} className="shrink-0" /> {t("config.error.confirmation")}</div>
          <div className="mt-1 break-all font-mono">{idList(error.detail.dangerousIds)}</div>
        </div>
      );
    case "apply_failed":
      return (
        <div className="rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
          <div className="flex gap-2 font-medium">
            <TriangleAlert size={14} className="shrink-0" />
            {t("config.error.applyFailed", { id: error.detail.failedId ?? t("config.value.none") })}
          </div>
          {error.detail.appliedIds.length > 0 && <div className="mt-1 break-all font-mono">{t("config.error.applied", { ids: idList(error.detail.appliedIds) })}</div>}
          {error.detail.pendingIds.length > 0 && <div className="mt-1 break-all font-mono">{t("config.error.pending", { ids: idList(error.detail.pendingIds) })}</div>}
        </div>
      );
    case "other":
      return (
        <div className="rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
          <div className="flex gap-2 font-medium">
            <TriangleAlert size={14} className="shrink-0" />
            {t(error.detail.code === "storage_error" ? "config.error.storage" : "config.error.other")}
          </div>
          <div className="mt-1 break-words text-ink-dim">{error.message}</div>
        </div>
      );
    default:
      return assertNever(error.detail);
  }
}
