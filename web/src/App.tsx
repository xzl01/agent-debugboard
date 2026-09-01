import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
  type KeyboardEvent,
} from "react";
import { createPortal } from "react-dom";
import {
  Activity,
  Cpu,
  Loader2,
  Route,
  ServerCrash,
  SlidersHorizontal,
  Terminal,
  Workflow,
  Wrench,
  X,
  Zap,
} from "lucide-react";
import { useBoard } from "@/hooks/useBoard";
import { usePersistentConfig } from "@/hooks/usePersistentConfig";
import { StatusBar } from "./components/StatusBar";
import { PowerCard } from "./components/PowerCard";
import { SwitchCard } from "./components/SwitchCard";
import { GpioCard } from "./components/GpioCard";
import { WatchdogCard } from "./components/WatchdogCard";
import {
  SerialCard,
  type SerialAutomationHandle,
  type SerialConnectionSummary,
} from "./components/SerialCard";
import { PowerAnalysisWorkspace } from "./components/PowerAnalysisWorkspace";
import { TestAutomation } from "./components/TestAutomation";
import { LogicAnalyzerCard } from "./components/LogicAnalyzerCard";
import { ConfigurationWorkspace } from "./components/ConfigurationWorkspace";
import {
  WorkbenchStatusBar,
} from "./components/WorkbenchStatusBar";
import { Button } from "./components/ui";
import { useI18n } from "@/lib/i18n";
import type { OtaStatus } from "@/lib/ota";
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

const DIALOG_FOCUSABLE_SELECTOR = [
  "button:not([disabled])",
  "[href]",
  'input:not([disabled]):not([type="hidden"])',
  "select:not([disabled])",
  "textarea:not([disabled])",
  '[tabindex]:not([tabindex="-1"])',
].join(", ");

type HardwareSectionId = "power" | "io";

const HARDWARE_SECTION_STORAGE_KEY = "linkr-hardware-controls-section";

function readStoredHardwareSection(): HardwareSectionId {
  try {
    const value = window.localStorage.getItem(HARDWARE_SECTION_STORAGE_KEY);
    return value === "power" || value === "io" ? value : "power";
  } catch {
    return "power";
  }
}

function writeStoredHardwareSection(section: HardwareSectionId): void {
  try {
    window.localStorage.setItem(HARDWARE_SECTION_STORAGE_KEY, section);
  } catch {
    // Best-effort persistence; in-memory state is the authority.
  }
}

// allow: SIZE_OK — one app-shell orchestrator owns workspace routing and shared status.
export default function App() {
  const board = useBoard();
  const [ota, setOta] = useState<OtaStatus | null>(null);
  const persistentConfig = usePersistentConfig({
    connected: board.connected,
    summary: board.snapshot.config,
    currentStateKey: board.persistentConfigCurrentStateKey,
  });
  const { t } = useI18n();
  const serialAutomationRef = useRef<SerialAutomationHandle>(null);
  const [serialConnections, setSerialConnections] = useState<SerialConnectionSummary>({
    uart0: false,
    uart1: false,
    bridgeActive: false,
  });
  const [logicAnalyzerActive, setLogicAnalyzerActive] = useState(false);
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
    configuration: null,
  });
  const pendingWorkspaceFocusRef = useRef<WorkspaceTabId | null>(null);
  const [selectedWorkspaceTab, setSelectedWorkspaceTab] = useState<WorkspaceTabId>("terminal");
  const [automationFocusMode, setAutomationFocusMode] = useState(true);
  const [hardwareDialogRequested, setHardwareDialogRequested] = useState(false);
  const [selectedHardwareSection, setSelectedHardwareSection] = useState<HardwareSectionId>(() => readStoredHardwareSection());
  const hardwareDialogOpen = hardwareDialogRequested;
  const hardwareDialogRef = useRef<HTMLDialogElement>(null);
  const hardwareDialogCloseRef = useRef<HTMLButtonElement>(null);
  const hardwareDialogOpenerRef = useRef<HTMLElement | null>(null);
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
    {
      id: "configuration" as const,
      icon: Wrench,
      label: t("workspace.configurationTab"),
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

  useLayoutEffect(() => {
    const nextTab = pendingWorkspaceFocusRef.current;
    if (!nextTab) return;
    const button = workspaceTabRefs.current[nextTab];
    button?.focus({ preventScroll: true });
    button?.scrollIntoView({ block: "nearest", inline: "nearest" });
    pendingWorkspaceFocusRef.current = null;
  }, [selectedWorkspaceTab]);

  useEffect(() => {
    const keepSelectedTabVisible = () => {
      workspaceTabRefs.current[selectedWorkspaceTab]?.scrollIntoView({
        block: "nearest",
        inline: "nearest",
      });
    };
    window.addEventListener("resize", keepSelectedTabVisible);
    return () => window.removeEventListener("resize", keepSelectedTabVisible);
  }, [selectedWorkspaceTab]);

  const onAutomationFocusModeChange = useCallback((focused: boolean) => {
    if (!focused && document.activeElement instanceof HTMLElement) {
      hardwareDialogOpenerRef.current = document.activeElement;
    }
    setHardwareDialogRequested(!focused);
    setAutomationFocusMode(focused);
  }, []);

  const closeHardwareDialog = useCallback(() => {
    setHardwareDialogRequested(false);
    if (selectedWorkspaceTab === "automation") setAutomationFocusMode(true);
  }, [selectedWorkspaceTab]);

  const openHardwareControls = useCallback(() => {
    if (document.activeElement instanceof HTMLElement) {
      hardwareDialogOpenerRef.current = document.activeElement;
    }
    setHardwareDialogRequested(true);
  }, []);

  const selectHardwareSection = useCallback((section: HardwareSectionId) => {
    setSelectedHardwareSection(section);
    writeStoredHardwareSection(section);
  }, []);

  const scrollToHardwareControl = useCallback((id: string) => {
    document.getElementById(id)?.scrollIntoView({ behavior: "smooth", block: "start" });
  }, []);

  useEffect(() => {
    if (!hardwareDialogOpen) return;
    const dialog = hardwareDialogRef.current;
    if (!dialog) return;
    const previousOverflow = document.body.style.overflow;
    document.body.style.overflow = "hidden";
    hardwareDialogCloseRef.current?.focus();

    const onKeyDown = (event: globalThis.KeyboardEvent) => {
      if (event.key === "Escape") {
        event.preventDefault();
        closeHardwareDialog();
        return;
      }
      if (event.key !== "Tab") return;
      const focusable = [...dialog.querySelectorAll<HTMLElement>(DIALOG_FOCUSABLE_SELECTOR)];
      const first = focusable[0];
      const last = focusable[focusable.length - 1];
      if (!first || !last) {
        event.preventDefault();
        dialog.focus();
      } else if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first.focus();
      }
    };

    window.addEventListener("keydown", onKeyDown);
    return () => {
      window.removeEventListener("keydown", onKeyDown);
      document.body.style.overflow = previousOverflow;
      const opener = hardwareDialogOpenerRef.current;
      hardwareDialogOpenerRef.current = null;
      if (opener?.isConnected && !opener.closest("[hidden]")) {
        opener.focus({ preventScroll: true });
      } else {
        workspaceTabRefs.current[selectedWorkspaceTab]?.focus({ preventScroll: true });
      }
    };
  }, [closeHardwareDialog, hardwareDialogOpen, selectedWorkspaceTab]);

  const workspaceTabList = (
    <div className="min-w-0 flex-1 overflow-x-auto py-2">
      <div
        role="tablist"
        aria-label={t("workspace.tabs")}
        aria-orientation="horizontal"
        className="inline-flex min-w-max gap-2"
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
              onClick={() => {
                pendingWorkspaceFocusRef.current = tab.id;
                setSelectedWorkspaceTab(tab.id);
              }}
              onKeyDown={(event) => onWorkspaceTabKeyDown(event, index)}
              className={`relative flex min-h-9 items-center justify-center gap-2 rounded-lg border px-3 text-[13px] font-medium transition-colors duration-150 sm:justify-start ${
                selected
                  ? "border-brand/25 bg-brand/10 text-brand ring-1 ring-inset ring-brand/10 after:absolute after:inset-x-3 after:-bottom-[5px] after:h-0.5 after:rounded-full after:bg-brand"
                  : "border-transparent bg-transparent text-ink-dim hover:border-line/70 hover:bg-panel2/70 hover:text-ink"
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

  const hardwareSections = [
    { id: "power" as const, icon: Zap, label: t("test.hardware.section.power") },
    { id: "io" as const, icon: Cpu, label: t("test.hardware.section.io") },
  ];

  const hardwareControls = selectedHardwareSection === "power" ? (
    <div
      id="hardware-section-panel-power"
      role="tabpanel"
      aria-labelledby="hardware-section-tab-power"
      data-testid="hardware-section-panel-power"
      className="grid min-w-0 items-start gap-4 md:grid-cols-[minmax(0,1.15fr)_minmax(300px,0.85fr)]"
    >
      <div id="hardware-control-power" className="min-w-0 scroll-mt-14">
        <PowerCard
          outputs={board.snapshot.powerOutputs}
          readings={board.snapshot.adc}
          onSet={board.setPower}
          disabled={!board.connected || automationOwner != null}
          stale={!board.connected}
        />
      </div>
      <div className="grid min-w-0 gap-4">
        <div id="hardware-control-routing" className="scroll-mt-14">
          <SwitchCard
            switches={board.snapshot.switches}
            onSet={board.setSwitch}
            disabled={!board.connected || automationOwner != null}
            stale={!board.connected}
          />
        </div>
      </div>
    </div>
  ) : (
    <div
      id="hardware-section-panel-io"
      role="tabpanel"
      aria-labelledby="hardware-section-tab-io"
      data-testid="hardware-section-panel-io"
      className="grid min-w-0 items-start gap-4 md:grid-cols-[minmax(0,1.2fr)_minmax(280px,0.8fr)]"
    >
      <div
        data-testid="gpio-controls-anchor"
        className="min-w-0"
      >
        <GpioCard
          gpios={board.snapshot.gpios}
          onSet={board.setGpio}
          disabled={!board.connected || automationOwner != null}
          stale={!board.connected}
        />
      </div>
      <div className="min-w-0">
        <WatchdogCard watchdog={board.snapshot.watchdog} />
      </div>
    </div>
  );

  return (
    <div className="min-h-full bg-bg text-ink">
      <StatusBar
        snapshot={board.snapshot}
        connected={board.connected}
        lastVerifiedAt={board.lastVerifiedAt}
        loading={board.loading}
        auto={board.auto}
        setAuto={board.setAuto}
        live={board.live}
        setLive={board.setLive}
        onRefresh={board.refresh}
        ota={ota}
        logicAnalyzerActive={logicAnalyzerActive}
        uartBridgeActive={serialConnections.bridgeActive}
      />

      <nav className="border-b border-line/80 bg-panel/95 md:sticky md:top-14 md:z-20 md:backdrop-blur-sm">
        <div className="mx-auto flex h-11 max-w-[1440px] items-center gap-3 px-6">
          {workspaceTabList}
          <Button
            type="button"
            variant="ghost"
            data-testid="open-hardware-controls"
            className="min-h-9 shrink-0 rounded-lg border-line/70 px-2.5 text-xs hover:border-line"
            onClick={() => openHardwareControls()}
          >
            <SlidersHorizontal size={14} />
            <span className="hidden sm:inline">{t("workspace.hardwareControls")}</span>
          </Button>
        </div>
      </nav>

      {!board.connected && (
        <div className="mx-auto max-w-[1440px] px-6 pt-4">
          <div className="flex items-center gap-3 rounded-xl border border-danger/30 bg-danger/10 px-4 py-3">
            <ServerCrash size={20} className="text-danger" />
            <div className="flex-1">
              <div className="text-sm font-medium text-danger">{t("banner.unreachable")}</div>
              <div className="text-xs text-ink-dim">
                {board.error || t("banner.unreachable.detail")}
              </div>
              {board.lastVerifiedAt != null && (
                <div className="mt-0.5 text-xs text-ink-dim">
                  {t("banner.readOnlySnapshot", {
                    time: new Intl.DateTimeFormat(undefined, {
                      hour: "2-digit",
                      minute: "2-digit",
                      second: "2-digit",
                      hour12: false,
                    }).format(board.lastVerifiedAt),
                  })}
                </div>
              )}
            </div>
            <Button variant="default" onClick={board.refresh}>
              {t("banner.retry")}
            </Button>
          </div>
        </div>
      )}

      <main className="mx-auto max-w-[1440px] px-6 pb-16 pt-5">
        {board.loading && !board.hasData ? (
          <div className="flex flex-col items-center justify-center gap-3 py-24 text-ink-dim">
            <Loader2 size={24} className="animate-spin text-brand" />
            <span className="text-sm">{t("loading")}</span>
          </div>
        ) : (
          <div
            data-testid="workspace-layout"
            className="grid animate-fade-up grid-cols-1 items-start gap-4"
          >
            <div data-testid="workspace-main" className="min-w-0 xl:sticky xl:top-[124px]">
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
                    vinRoute={board.snapshot.switches.vin?.route}
                    onSetVin={(route) => board.setSwitch("vin", route)}
                    onConnectionChange={setSerialConnections}
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
                    serialRef={serialAutomationRef}
                    onSetPower={board.setPower}
                    onReadPower={board.readPower}
                    onArmCapture={board.armCapture}
                    onTriggerCapture={board.triggerCapture}
                    onStopCapture={board.stopCapture}
                    onCancelCapture={board.cancelCapture}
                    onClearCaptures={board.clearCaptures}
                    taskControl={automationTaskControl}
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
                    onActivityChange={setLogicAnalyzerActive}
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
                    focusMode={automationFocusMode}
                    onFocusModeChange={onAutomationFocusModeChange}
                  />
                </div>

                <div
                  id={getWorkspacePanelId("configuration")}
                  role="tabpanel"
                  aria-labelledby={getWorkspaceTabId("configuration")}
                  hidden={selectedWorkspaceTab !== "configuration"}
                  className="min-h-0 min-w-0"
                >
                  <ConfigurationWorkspace
                    connected={board.connected}
                    persistentConfig={persistentConfig}
                    onEnterBootloader={board.enterBootloader}
                    ota={ota}
                    setOta={setOta}
                    disabled={!board.connected || automationOwner != null}
                    taskControl={automationTaskControl}
                  />
                </div>
              </div>
            </div>
          </div>
        )}

        {hardwareDialogOpen && createPortal(
          <div
            data-testid="hardware-controls-backdrop"
            className="fixed inset-0 z-50 flex justify-end bg-overlay/30"
            onClick={(event) => {
              if (event.target === event.currentTarget) closeHardwareDialog();
            }}
          >
            <dialog
              open
              ref={hardwareDialogRef}
              id="hardware-controls"
              aria-modal="true"
              aria-labelledby="hardware-controls-title"
              aria-describedby="hardware-controls-description"
              className="animate-drawer-enter relative m-0 flex h-dvh max-h-dvh w-full max-w-[760px] flex-col overflow-hidden rounded-none bg-bg p-0 shadow-2xl sm:rounded-l-2xl"
            >
              <header className="flex shrink-0 items-center gap-3 border-b border-line/60 bg-panel px-4 py-3">
                <span className="grid h-9 w-9 shrink-0 place-items-center rounded-xl bg-brand/10 text-brand">
                  <SlidersHorizontal size={17} />
                </span>
                <span className="min-w-0 flex-1">
                  <h2 id="hardware-controls-title" className="text-sm font-semibold text-ink">
                    {t("test.hardware.title")}
                  </h2>
                  <p id="hardware-controls-description" className="truncate text-xs text-ink-dim">
                    {t("test.hardware.subtitle")}
                  </p>
                </span>
                <Button
                  ref={hardwareDialogCloseRef}
                  type="button"
                  variant="ghost"
                  className="min-h-11 min-w-11 px-2"
                  aria-label={t("test.hardware.close")}
                  onClick={closeHardwareDialog}
                >
                  <X size={18} />
                </Button>
              </header>
              <div
                role="tablist"
                aria-label={t("test.hardware.sections")}
                className="grid shrink-0 grid-cols-2 gap-1 border-b border-line/70 bg-panel px-3 py-2"
              >
                {hardwareSections.map((section) => {
                  const Icon = section.icon;
                  const selected = section.id === selectedHardwareSection;
                  return (
                    <button
                      key={section.id}
                      id={`hardware-section-tab-${section.id}`}
                      type="button"
                      role="tab"
                      aria-selected={selected}
                      aria-controls={`hardware-section-panel-${section.id}`}
                      tabIndex={selected ? 0 : -1}
                      data-testid={`hardware-section-tab-${section.id}`}
                      onClick={() => selectHardwareSection(section.id)}
                      className={`flex min-h-10 items-center justify-center gap-2 rounded-lg px-3 text-xs font-medium outline-none transition-colors focus-visible:ring-2 focus-visible:ring-brand/40 ${
                        selected
                          ? "bg-brand/10 text-brand ring-1 ring-inset ring-brand/15"
                          : "text-ink-dim hover:bg-panel2/70 hover:text-ink"
                      }`}
                    >
                      <Icon size={15} />
                      <span className="whitespace-nowrap">{section.label}</span>
                    </button>
                  );
                })}
              </div>
              <div className="min-h-0 flex-1 overflow-y-auto overscroll-contain p-3 sm:p-4">
                {selectedHardwareSection === "power" && (
                  <nav
                    aria-label={t("test.hardware.section.power")}
                    data-testid="hardware-control-anchors"
                    className="sticky top-0 z-10 -mx-1 mb-3 flex flex-wrap gap-1 rounded-xl border border-line/70 bg-panel/95 p-1"
                  >
                    {([
                      { id: "hardware-control-power", icon: Zap, label: t("test.hardware.anchor.power") },
                      { id: "hardware-control-routing", icon: Route, label: t("test.hardware.anchor.routing") },
                    ] as const).map(({ id, icon: Icon, label }) => (
                      <button
                        key={id}
                        type="button"
                        data-testid={`hardware-anchor-${id.replace("hardware-control-", "")}`}
                        onClick={() => scrollToHardwareControl(id)}
                        className="inline-flex min-h-8 items-center gap-1.5 rounded-lg px-2.5 text-[11px] font-medium text-ink-dim outline-none transition-colors hover:bg-panel2 hover:text-ink focus-visible:ring-2 focus-visible:ring-brand/30"
                      >
                        <Icon size={13} />
                        {label}
                      </button>
                    ))}
                  </nav>
                )}
                {hardwareControls}
              </div>
            </dialog>
          </div>,
          document.body,
        )}

      </main>

      <WorkbenchStatusBar
        snapshot={board.snapshot}
        connected={board.connected}
        serialConnections={serialConnections}
        taskOwner={automationOwner}
      />
    </div>
  );
}
