import { useRef, useState, type KeyboardEvent, type Ref } from "react";
import { Activity, Terminal } from "lucide-react";
import { useI18n } from "@/lib/i18n";
import type { SafeGpio } from "@/lib/types";
import {
  getNextWorkspaceTabIndex,
  getWorkspacePanelId,
  getWorkspaceTabId,
  type WorkspaceTabId,
} from "@/lib/workspaceTabs";
import { LogicAnalyzerCard } from "./LogicAnalyzerCard";
import { SerialCard, type SerialAutomationHandle } from "./SerialCard";

export type AppWorkspaceProps = {
  readonly serialAutomationRef: Ref<SerialAutomationHandle>;
  readonly vinRoute?: string;
  readonly onSetVin: (route: "1.8v" | "3.3v") => Promise<void>;
  readonly boardGpios?: SafeGpio[];
};

export function AppWorkspace({
  serialAutomationRef,
  vinRoute,
  onSetVin,
  boardGpios,
}: AppWorkspaceProps) {
  const { t } = useI18n();
  const workspaceTabRefs = useRef<Record<WorkspaceTabId, HTMLButtonElement | null>>({
    terminal: null,
    powerAnalysis: null,
    logicAnalyzer: null,
    automation: null,
  });
  const [selectedWorkspaceTab, setSelectedWorkspaceTab] = useState<WorkspaceTabId>("terminal");
  const workspaceTabs = [
    { id: "terminal" as const, icon: Terminal, label: t("workspace.terminalTab") },
    {
      id: "logicAnalyzer" as const,
      icon: Activity,
      label: t("workspace.logicAnalyzerTab"),
    },
  ];

  const onWorkspaceTabKeyDown = (
    event: KeyboardEvent<HTMLButtonElement>,
    currentIndex: number
  ) => {
    const nextIndex = getNextWorkspaceTabIndex(
      currentIndex,
      event.key,
      workspaceTabs.length
    );
    if (nextIndex == null) return;
    event.preventDefault();
    const nextTab = workspaceTabs[nextIndex]?.id;
    if (!nextTab) return;
    workspaceTabRefs.current[nextTab]?.focus();
    setSelectedWorkspaceTab(nextTab);
  };

  return (
    <div className="min-w-0 xl:sticky xl:top-[116px]">
      <div className="flex min-h-0 min-w-0 flex-col gap-3">
        <div className="overflow-x-auto pb-1">
          <div
            role="tablist"
            aria-label={t("workspace.tabs")}
            aria-orientation="horizontal"
            className="inline-flex min-w-full rounded-xl border border-line/70 bg-panel p-1 shadow-sm sm:min-w-0"
          >
            {workspaceTabs.map((tab, index) => {
              const Icon = tab.icon;
              const selected = selectedWorkspaceTab === tab.id;
              return (
                <button
                  key={tab.id}
                  ref={(node) => {
                    workspaceTabRefs.current[tab.id] = node;
                  }}
                  id={getWorkspaceTabId(tab.id)}
                  type="button"
                  role="tab"
                  tabIndex={selected ? 0 : -1}
                  aria-selected={selected}
                  aria-controls={getWorkspacePanelId(tab.id)}
                  onClick={() => setSelectedWorkspaceTab(tab.id)}
                  onKeyDown={(event) => onWorkspaceTabKeyDown(event, index)}
                  className={`flex min-h-10 flex-1 items-center justify-center gap-2 rounded-lg px-3 text-sm font-medium transition-colors sm:flex-none sm:justify-start ${
                    selected
                      ? "bg-brand/12 text-brand shadow-sm"
                      : "text-ink-dim hover:text-ink"
                  }`}
                >
                  <Icon size={16} />
                  <span className="whitespace-nowrap">{tab.label}</span>
                </button>
              );
            })}
          </div>
        </div>

        <div
          id={getWorkspacePanelId("terminal")}
          role="tabpanel"
          aria-labelledby={getWorkspaceTabId("terminal")}
          hidden={selectedWorkspaceTab !== "terminal"}
          className="min-h-0 min-w-0"
        >
          <SerialCard
            ref={serialAutomationRef}
            vinRoute={vinRoute}
            onSetVin={onSetVin}
          />
        </div>

        <div
          id={getWorkspacePanelId("logicAnalyzer")}
          role="tabpanel"
          aria-labelledby={getWorkspaceTabId("logicAnalyzer")}
          hidden={selectedWorkspaceTab !== "logicAnalyzer"}
          className="min-h-0 min-w-0"
        >
          <LogicAnalyzerCard boardGpios={boardGpios} />
        </div>
      </div>
    </div>
  );
}
