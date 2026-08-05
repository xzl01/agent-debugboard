import {
  CheckCircle2,
  CircleAlert,
  CircleSlash,
  Clock3,
  Database,
  Loader2,
  RefreshCw,
  Save,
  Trash2,
  TriangleAlert,
  Unplug,
} from "lucide-react";
import { Badge, Button, Card } from "./ui";
import { PersistentConfigDialog } from "./PersistentConfigDialog";
import { PersistentConfigFeedback } from "./PersistentConfigFeedback";
import { PersistentConfigRows } from "./PersistentConfigRows";
import { usePersistentConfigCardModel } from "./usePersistentConfigCardModel";
import type { UsePersistentConfig } from "@/hooks/usePersistentConfig";
import { useI18n } from "@/lib/i18n";

const FOCUSABLE_DISABLED_CLASS = "aria-disabled:cursor-not-allowed aria-disabled:opacity-50 aria-disabled:active:scale-100";

export function PersistentConfigCard({
  state,
  connected,
}: {
  readonly state: UsePersistentConfig;
  readonly connected: boolean;
}) {
  const { t } = useI18n();
  const model = usePersistentConfigCardModel(state, connected);

  const headerStatus = !connected ? (
    <Badge tone="danger"><Unplug size={12} /> {t("config.status.disconnected")}</Badge>
  ) : !state.supported ? (
    <Badge tone="neutral"><CircleSlash size={12} /> {t("config.status.unsupported")}</Badge>
  ) : state.config && !state.config.backend.available ? (
    <Badge tone="danger"><TriangleAlert size={12} /> {t("config.status.unavailable")}</Badge>
  ) : state.config?.pending ? (
    <Badge tone="warn"><Clock3 size={12} /> {t("config.pendingCount", { count: state.config.pending })}</Badge>
  ) : (
    <Badge tone="ok"><CheckCircle2 size={12} /> {t("config.status.ready")}</Badge>
  );

  return (
    <Card title={t("config.title")} subtitle={t("config.subtitle")} icon={Database} right={headerStatus}>
      {!connected && !state.config ? (
        <PersistentConfigFeedback connected={false} loading={false} hasConfig={false} error={state.error} />
      ) : !state.supported ? (
        <div className="flex gap-3 rounded-xl border border-line/70 bg-panel2/50 px-3 py-3">
          <CircleSlash size={18} className="shrink-0 text-ink-dim" />
          <div><div className="text-sm font-medium text-ink">{t("config.unsupported.title")}</div><p className="mt-1 text-xs text-ink-dim">{t("config.unsupported.body")}</p></div>
        </div>
      ) : state.loading && !state.config ? (
        <div className="flex min-h-24 items-center justify-center gap-2 text-sm text-ink-dim"><Loader2 size={18} className="animate-spin text-brand" /> {t("config.loading")}</div>
      ) : !state.config ? (
        <div className="space-y-3">
          <PersistentConfigFeedback connected={connected} loading={state.loading} hasConfig={false} error={state.error} />
          <Button type="button" className="min-h-11" onClick={() => void model.refresh()} disabled={!connected || state.loading}><RefreshCw size={15} /> {t("config.refresh")}</Button>
        </div>
      ) : !state.config.backend.available ? (
        <div className="space-y-3">
          <div className="flex gap-3 rounded-xl border border-danger/30 bg-danger/10 px-3 py-3"><CircleAlert size={18} className="shrink-0 text-danger" /><div><div className="text-sm font-medium text-danger">{t("config.unavailable.title")}</div><p className="mt-1 break-words text-xs text-ink-dim">{state.config.backend.reason}</p></div></div>
          <Button type="button" className="min-h-11" onClick={() => void model.refresh()} disabled={model.disabled}><RefreshCw size={15} /> {t("config.refresh")}</Button>
        </div>
      ) : (
        <div className="space-y-4">
          <div className="space-y-2" aria-live="polite">
            <PersistentConfigFeedback connected={connected} loading={state.loading} hasConfig error={state.error} />
            {model.notice && <div className="flex gap-2 rounded-lg border border-ok/30 bg-ok/15 px-3 py-2 text-xs text-ok"><CheckCircle2 size={14} className="shrink-0" /> {t(`config.success.${model.notice}`)}</div>}
            {model.selectedIds.size === 0 && <div className="flex gap-2 rounded-lg border border-line/60 bg-panel2/50 px-3 py-2 text-xs text-ink-dim"><CircleAlert size={14} className="shrink-0" /> {t("config.emptySelection")}</div>}
            {state.config.pending > 0 && <div className="flex gap-2 rounded-lg border border-warn/30 bg-warn/10 px-3 py-2 text-xs text-warn"><Clock3 size={14} className="shrink-0" /> {t(state.config.pending === 1 ? "config.pendingNote.one" : "config.pendingNote.many", { count: state.config.pending })}</div>}
          </div>

          {model.visibleItems.length === 0 ? <p className="text-sm text-ink-dim">{t("config.emptyCatalog")}</p> : <PersistentConfigRows groups={model.groups} selectedIds={model.selectedIds} disabled={model.disabled} error={state.error} onToggle={model.toggle} />}

          <div className="grid grid-cols-1 gap-2 sm:grid-cols-2">
            <Button type="button" variant="primary" className={`min-h-11 whitespace-nowrap ${FOCUSABLE_DISABLED_CLASS}`} aria-disabled={model.saveDisabled} onClick={model.save}>{state.busy === "save" ? <Loader2 size={15} className="animate-spin" /> : <Save size={15} />} {t(state.busy === "save" ? "config.saving" : "config.save")}</Button>
            <Button type="button" className="min-h-11 whitespace-nowrap" onClick={() => void model.refresh()} disabled={model.disabled}><RefreshCw size={15} className={state.loading ? "animate-spin" : ""} /> {t("config.refresh")}</Button>
            <Button type="button" variant="danger" className={`min-h-11 whitespace-nowrap ${FOCUSABLE_DISABLED_CLASS}`} aria-disabled={model.clearDisabled} onClick={model.clear}>{state.busy === "clear" ? <Loader2 size={15} className="animate-spin" /> : <Trash2 size={15} />} {t(state.busy === "clear" ? "config.clearing" : "config.clear")}</Button>
          </div>
        </div>
      )}

      {model.confirmation && <PersistentConfigDialog kind={model.confirmation.kind} ids={model.confirmation.ids} busy={state.busy !== null} opener={model.confirmation.opener} onCancel={model.cancelConfirmation} onConfirm={model.confirm} />}
    </Card>
  );
}
