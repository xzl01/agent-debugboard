import { PenLine, Play, ScrollText, Workflow } from "lucide-react";
import { useI18n } from "@/lib/i18n";

export type TestAutomationTab = "editor" | "running" | "report" | "tasks";

const TAB_ICONS = { editor: PenLine, running: Play, report: ScrollText, tasks: Workflow } as const;

export function TestAutomationTabs({
  tab,
  isRunning,
  hasReport,
  onSelect,
}: {
  readonly tab: TestAutomationTab;
  readonly isRunning: boolean;
  readonly hasReport: boolean;
  readonly onSelect: (tab: TestAutomationTab) => void;
}) {
  const { t } = useI18n();
  return (
    <div
      className="inline-flex rounded-xl border border-line/70 bg-panel2 p-1"
      role="tablist"
      aria-label={t("test.title")}
    >
      {(["editor", "running", "report", "tasks"] as TestAutomationTab[]).map((t2) => {
        const Icon = TAB_ICONS[t2];
        const active = tab === t2;
        const disabled = isRunning ? t2 !== "running" : t2 === "running";
        const disabledReport = t2 === "report" && !hasReport;
        return (
          <button
            key={t2}
            type="button"
            role="tab"
            aria-selected={active}
            disabled={disabled || disabledReport}
            onClick={() => onSelect(t2)}
            className={`flex min-h-8 items-center gap-1.5 rounded-lg px-2.5 py-1 text-[11px] font-semibold transition-colors ${
              active
                ? "bg-panel text-ink shadow-sm"
                : "text-ink-dim hover:text-ink disabled:opacity-40"
            }`}
          >
            <Icon size={12} />
            {t(`test.${t2}`)}
          </button>
        );
      })}
    </div>
  );
}
