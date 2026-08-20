import { useMemo, useState, type Dispatch, type SetStateAction } from "react";
import { Database, Wrench } from "lucide-react";
import type { UsePersistentConfig } from "@/hooks/usePersistentConfig";
import type { AutomationTaskControl } from "@/lib/automationTask";
import { useI18n } from "@/lib/i18n";
import type { OtaStatus } from "@/lib/ota";
import { Badge } from "./ui";
import { BootCard } from "./BootCard";
import { OtaCard } from "./OtaCard";
import { PersistentConfigCard } from "./PersistentConfigCard";

type ConfigurationSectionId = "saved" | "firmware";

export function ConfigurationWorkspace({
  connected,
  persistentConfig,
  onEnterBootloader,
  ota,
  setOta,
  disabled,
  taskControl,
}: {
  readonly connected: boolean;
  readonly persistentConfig: UsePersistentConfig;
  readonly onEnterBootloader: () => Promise<void>;
  readonly ota: OtaStatus | null;
  readonly setOta: Dispatch<SetStateAction<OtaStatus | null>>;
  readonly disabled: boolean;
  readonly taskControl: AutomationTaskControl;
}) {
  const { t } = useI18n();
  const [section, setSection] = useState<ConfigurationSectionId>("saved");
  const tabs = [
    { id: "saved" as const, icon: Database, label: t("configuration.section.saved") },
    { id: "firmware" as const, icon: Wrench, label: t("configuration.section.firmware") },
  ];
  const summary = useMemo(() => {
    const items = persistentConfig.config?.items.filter((item) => item.kind !== "unknown") ?? [];
    const saved = items.filter((item) => item.saved !== null).length;
    return {
      saved,
      pending: persistentConfig.config?.pending ?? 0,
      unsaved: Math.max(0, items.length - saved),
    };
  }, [persistentConfig.config]);

  return (
    <section className="flex min-w-0 flex-col gap-3" data-testid="configuration-workspace">
      <header className="flex min-h-14 flex-wrap items-start justify-between gap-3 px-0.5 py-1">
        <div>
          <h2 className="text-base font-semibold tracking-[-0.01em] text-ink">{t("configuration.title")}</h2>
          <p className="mt-1 max-w-3xl text-xs leading-5 text-ink-dim">{t("configuration.subtitle")}</p>
        </div>
        <Badge tone={connected ? "ok" : "danger"}>
          {t(connected ? "configuration.connected" : "configuration.disconnected")}
        </Badge>
      </header>

      <div
        role="tablist"
        aria-label={t("configuration.sections")}
        className="grid w-full max-w-[760px] grid-cols-2 gap-1 rounded-xl border border-line/80 bg-panel p-1"
      >
        {tabs.map((tab) => {
          const Icon = tab.icon;
          const selected = section === tab.id;
          return (
            <button
              key={tab.id}
              id={`configuration-section-tab-${tab.id}`}
              type="button"
              role="tab"
              aria-selected={selected}
              aria-controls={`configuration-section-panel-${tab.id}`}
              data-testid={`configuration-section-tab-${tab.id}`}
              onClick={() => setSection(tab.id)}
              className={`flex min-h-9 items-center justify-center gap-2 rounded-lg px-3 text-xs font-medium outline-none transition-colors focus-visible:ring-2 focus-visible:ring-brand/40 ${
                selected
                  ? "bg-brand/10 text-brand ring-1 ring-inset ring-brand/15"
                  : "text-ink-dim hover:bg-panel/70 hover:text-ink"
              }`}
            >
              <Icon size={14} />
              <span>{tab.label}</span>
            </button>
          );
        })}
      </div>

      {section === "saved" ? (
        <div
          id="configuration-section-panel-saved"
          role="tabpanel"
          aria-labelledby="configuration-section-tab-saved"
          data-testid="configuration-section-panel-saved"
          className="min-w-0 space-y-3"
        >
          <dl
            aria-label={t("configuration.summary.title")}
            data-testid="configuration-summary-strip"
            className="grid gap-px overflow-hidden rounded-xl border border-line/70 bg-line/60 sm:grid-cols-3"
          >
            <div className="flex items-center justify-between gap-3 bg-panel px-4 py-2.5">
              <dt className="text-xs text-ink-dim">{t("configuration.summary.saved")}</dt>
              <dd className="font-mono text-sm font-semibold text-ink">{summary.saved}</dd>
            </div>
            <div className="flex items-center justify-between gap-3 bg-panel px-4 py-2.5">
              <dt className="text-xs text-ink-dim">{t("configuration.summary.pending")}</dt>
              <dd className={`font-mono text-sm font-semibold ${summary.pending ? "text-warn" : "text-ink"}`}>{summary.pending}</dd>
            </div>
            <div className="flex items-center justify-between gap-3 bg-panel px-4 py-2.5">
              <dt className="text-xs text-ink-dim">{t("configuration.summary.unsaved")}</dt>
              <dd className="font-mono text-sm font-semibold text-ink">{summary.unsaved}</dd>
            </div>
          </dl>
          <PersistentConfigCard state={persistentConfig} connected={connected} taskControl={taskControl} />
        </div>
      ) : (
        <div
          id="configuration-section-panel-firmware"
          role="tabpanel"
          aria-labelledby="configuration-section-tab-firmware"
          data-testid="configuration-section-panel-firmware"
          className="grid min-w-0 items-start gap-4 lg:grid-cols-2"
        >
          <OtaCard
            status={ota}
            setStatus={setOta}
            disabled={disabled}
            taskControl={taskControl}
          />
          <BootCard
            onBoot={onEnterBootloader}
            disabled={disabled}
            taskControl={taskControl}
          />
        </div>
      )}
    </section>
  );
}
