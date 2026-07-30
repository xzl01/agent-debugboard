import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type KeyboardEvent,
  type ReactNode,
} from "react";
import {
  Activity,
  ChevronDown,
  Loader2,
  ServerCrash,
  Terminal,
  Workflow,
  Wrench,
  type LucideIcon,
} from "lucide-react";
import { useBoard } from "@/hooks/useBoard";
import { StatusBar } from "./components/StatusBar";
import { PowerCard } from "./components/PowerCard";
import { SwitchCard } from "./components/SwitchCard";
import { BootCard } from "./components/BootCard";
import { TargetRecoveryCard } from "./components/TargetRecoveryCard";
import { SerialCard, type SerialAutomationHandle } from "./components/SerialCard";
import { PowerAnalysisWorkspace } from "./components/PowerAnalysisWorkspace";
import { TestAutomation } from "./components/TestAutomation";
import { LogicAnalyzerCard } from "./components/LogicAnalyzerCard";
import { OtaCard } from "./components/OtaCard";
import { Badge, Button } from "./components/ui";
import { useI18n } from "@/lib/i18n";
import { apiEndpoint } from "@/lib/api";
import type { OtaStatus } from "@/lib/ota";
import { POWER_CAPTURE_SAMPLE_CAPACITY } from "@/lib/power";
import {
  createAutomationTaskLock,
  type AutomationTaskControl,
  type AutomationTaskOwner,
} from "@/lib/automationTask";
import {
  getNextWorkspaceTabIndex,
  getWorkspacePanelId,
  getWorkspaceTabId,
  type WorkspaceTabId,
} from "@/lib/workspaceTabs";

function ToolGroup({
  title,
  subtitle,
  count,
  icon: Icon,
  children,
}: {
  title: string;
  subtitle: string;
  count: string;
  icon: LucideIcon;
  children: ReactNode;
}) {
  return (
    <details className="group min-w-0 rounded-2xl border border-line/70 bg-panel shadow-sm sm:col-span-2 xl:col-span-1">
      <summary className="flex min-h-16 cursor-pointer list-none items-center gap-3 rounded-2xl px-4 py-3 outline-none transition-colors hover:bg-panel2/50 focus-visible:ring-2 focus-visible:ring-brand/40">
        <span className="grid h-9 w-9 shrink-0 place-items-center rounded-xl bg-brand/10 text-brand">
          <Icon size={17} />
        </span>
        <span className="min-w-0 flex-1">
          <span className="block text-sm font-semibold text-ink">{title}</span>
          <span className="block text-xs text-ink-dim sm:truncate">{subtitle}</span>
        </span>
        <Badge tone="neutral">{count}</Badge>
        <ChevronDown
          size={17}
          className="shrink-0 text-ink-dim transition-transform duration-200 group-open:rotate-180"
        />
      </summary>
      <div className="grid min-w-0 gap-4 border-t border-line/60 p-3 lg:grid-cols-2 xl:grid-cols-1">
        {children}
      </div>
    </details>
  );
}

export default function App() {
  const board = useBoard();
  const [ota, setOta] = useState<OtaStatus | null>(null);
  const { t } = useI18n();
  const serialAutomationRef = useRef<SerialAutomationHandle>(null);
  const automationTaskLockRef = useRef(createAutomationTaskLock());
  const [automationOwner, setAutomationOwner] = useState<AutomationTaskOwner | null>(null);
  const acquireAutomation = useCallback((owner: AutomationTaskOwner) => {
    const acquired = automationTaskLockRef.current.acquire(owner);
    if (acquired) setAutomationOwner(automationTaskLockRef.current.owner());
    return acquired;
  }, []);
  const releaseAutomation = useCallback((owner: AutomationTaskOwner) => {
    automationTaskLockRef.current.release(owner);
    setAutomationOwner(automationTaskLockRef.current.owner());
  }, []);
  const automationTaskControl = useMemo<AutomationTaskControl>(() => ({
    owner: automationOwner,
    acquire: acquireAutomation,
    release: releaseAutomation,
  }), [acquireAutomation, automationOwner, releaseAutomation]);

  const workspaceTabRefs = useRef<Record<WorkspaceTabId, HTMLButtonElement | null>>({
    terminal: null,
    powerAnalysis: null,
    logicAnalyzer: null,
    automation: null,
  });
  const pendingWorkspaceFocusRef = useRef<WorkspaceTabId | null>(null);
  const [selectedWorkspaceTab, setSelectedWorkspaceTab] = useState<WorkspaceTabId>("terminal");
  const workspaceTabs = [
    { id: "terminal" as const, icon: Terminal, label: t("workspace.terminalTab") },
    {
      id: "powerAnalysis" as const,
      icon: Activity,
      label: t("workspace.powerAnalysisTab"),
    },
    {
      id: "logicAnalyzer" as const,
      icon: Activity,
      label: t("workspace.logicAnalyzerTab"),
    },
    {
      id: "automation" as const,
      icon: Workflow,
      label: t("workspace.automationTab"),
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
    pendingWorkspaceFocusRef.current = nextTab;
    setSelectedWorkspaceTab(nextTab);
  };

  useEffect(() => {
    const nextTab = pendingWorkspaceFocusRef.current;
    if (!nextTab) return;
    workspaceTabRefs.current[nextTab]?.focus();
    pendingWorkspaceFocusRef.current = null;
  }, [selectedWorkspaceTab]);

  const workspaceTabList = (
    <div className="max-w-full overflow-x-auto">
      <div
        role="tablist"
        aria-label={t("workspace.tabs")}
        aria-orientation="horizontal"
        className="inline-flex min-w-max rounded-xl border border-line/70 bg-panel2 p-1"
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
              className={`flex min-h-9 items-center justify-center gap-2 rounded-lg px-3 text-sm font-medium transition-colors sm:justify-start ${
                selected
                  ? "bg-panel text-brand shadow-sm"
                  : "text-ink-dim hover:text-ink"
              }`}
            >
              <Icon size={15} />
              <span className="whitespace-nowrap">{tab.label}</span>
            </button>
          );
        })}
      </div>
    </div>
  );

  return (
    <div className="min-h-full bg-bg text-ink">
      <StatusBar
        snapshot={board.snapshot}
        connected={board.connected}
        loading={board.loading}
        auto={board.auto}
        setAuto={board.setAuto}
        live={board.live}
        setLive={board.setLive}
        onRefresh={board.refresh}
        ota={ota}
      />

      {!board.connected && (
        <div className="mx-auto max-w-[1600px] px-4 pt-4">
          <div className="flex items-center gap-3 rounded-xl border border-danger/30 bg-danger/10 px-4 py-3">
            <ServerCrash size={20} className="text-danger" />
            <div className="flex-1">
              <div className="text-sm font-medium text-danger">{t("banner.unreachable")}</div>
              <div className="text-xs text-ink-dim">
                {board.error || t("banner.unreachable.detail")}
              </div>
            </div>
            <Button variant="default" onClick={board.refresh}>
              {t("banner.retry")}
            </Button>
          </div>
        </div>
      )}

      <main className="mx-auto max-w-[1600px] px-4 py-5">
        {board.loading && !board.hasData ? (
          <div className="flex flex-col items-center justify-center gap-3 py-24 text-ink-dim">
            <Loader2 size={24} className="animate-spin text-brand" />
            <span className="text-sm">{t("loading")}</span>
          </div>
        ) : (
          <div className="grid animate-fade-up items-start gap-4 xl:grid-cols-[minmax(320px,370px)_minmax(0,1fr)]">
            <aside className="grid min-w-0 gap-4 sm:grid-cols-2 xl:grid-cols-1">
              <div className="min-w-0 sm:col-span-2 xl:col-span-1">
                <PowerCard
                  outputs={board.snapshot.powerOutputs}
                  readings={board.snapshot.adc}
                  onSet={board.setPower}
                />
              </div>
              <div className="min-w-0">
                <SwitchCard switches={board.snapshot.switches} onSet={board.setSwitch} />
              </div>
              <div className="min-w-0 sm:col-span-2 xl:col-span-1">
                <TargetRecoveryCard
                  outputs={board.snapshot.powerOutputs}
                  onEnter={board.enterTargetRecovery}
                />
              </div>

              <ToolGroup
                title={t("firmwareTools.title")}
                subtitle={t("firmwareTools.subtitle")}
                count={t("firmwareTools.count")}
                icon={Wrench}
              >
                <OtaCard status={ota} setStatus={setOta} />
                <BootCard onBoot={board.enterBootloader} />
              </ToolGroup>
            </aside>
            <div className="min-w-0 xl:sticky xl:top-[116px]">
              <div className="min-h-0 min-w-0">
                <div
                  id={getWorkspacePanelId("terminal")}
                  role="tabpanel"
                  aria-labelledby={getWorkspaceTabId("terminal")}
                  hidden={selectedWorkspaceTab !== "terminal"}
                  className="min-h-0 min-w-0"
                >
                  <SerialCard
                    ref={serialAutomationRef}
                    vinRoute={board.snapshot.switches.vin}
                    onSetVin={(route) => board.setSwitch("vin", route)}
                    workspaceTabs={selectedWorkspaceTab === "terminal" ? workspaceTabList : undefined}
                  />
                </div>

                <div
                  id={getWorkspacePanelId("powerAnalysis")}
                  role="tabpanel"
                  aria-labelledby={getWorkspaceTabId("powerAnalysis")}
                  hidden={selectedWorkspaceTab !== "powerAnalysis"}
                  className="min-h-0 min-w-0"
                >
                  <PowerAnalysisWorkspace
                    outputs={board.snapshot.powerOutputs}
                    gpios={board.snapshot.gpios}
                    captureState={board.captureState}
                    captureProgress={board.captureProgress}
                    captures={board.captures}
                    captureCapacity={POWER_CAPTURE_SAMPLE_CAPACITY}
                    serialRef={serialAutomationRef}
                    onSetPower={board.setPower}
                    onReadPower={board.readPower}
                    onArmCapture={board.armCapture}
                    onTriggerCapture={board.triggerCapture}
                    onCancelCapture={board.cancelCapture}
                    onClearCaptures={board.clearCaptures}
                    taskControl={automationTaskControl}
                    workspaceTabs={selectedWorkspaceTab === "powerAnalysis" ? workspaceTabList : undefined}
                  />
                </div>

                <div
                  id={getWorkspacePanelId("logicAnalyzer")}
                  role="tabpanel"
                  aria-labelledby={getWorkspaceTabId("logicAnalyzer")}
                  hidden={selectedWorkspaceTab !== "logicAnalyzer"}
                  className="min-h-0 min-w-0"
                >
                  <LogicAnalyzerCard
                    boardGpios={board.snapshot?.gpios}
                    workspaceTabs={selectedWorkspaceTab === "logicAnalyzer" ? workspaceTabList : undefined}
                  />
                </div>

                <div
                  id={getWorkspacePanelId("automation")}
                  role="tabpanel"
                  aria-labelledby={getWorkspaceTabId("automation")}
                  hidden={selectedWorkspaceTab !== "automation"}
                  className="min-h-0 min-w-0"
                >
                  <TestAutomation
                    board={board}
                    serialRef={serialAutomationRef}
                    taskControl={automationTaskControl}
                    workspaceTabs={selectedWorkspaceTab === "automation" ? workspaceTabList : undefined}
                  />
                </div>
              </div>
            </div>
          </div>
        )}

        <footer className="mt-6 flex flex-wrap items-center justify-between gap-2 text-[11px] text-ink-dim">
          <span>
            {t("footer.endpoint")}{" "}
            <span className="font-mono">{apiEndpoint()}</span>
          </span>
          <Badge tone="neutral">
            {board.live ? t("footer.live") : t("footer.polling")}
          </Badge>
        </footer>
      </main>
    </div>
  );
}
