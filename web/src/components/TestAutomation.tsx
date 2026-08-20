import { lazy, Suspense, useCallback, useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import {
  CircleAlert,
  FlaskConical,
  Loader2,
  Maximize2,
  Minimize2,
  PenLine,
  Play,
  ScrollText,
} from "lucide-react";
import { Button, Card } from "./ui";
import { TestRunnerView } from "./TestRunnerView";
import { TestReport } from "./TestReport";
import type { UseBoard } from "@/hooks/useBoard";
import type { SerialAutomationHandle } from "./SerialCard";
import type { TestScript, StepResult, StepStatus, RunSummary, AdcSampleEntry, SerialLogEntry, PowerCaptureEvidenceEntry } from "@/lib/testScript";
import { defaultScript, parseTestScript, serializeTestScript, tryBuildExecutionPlan } from "@/lib/testScript";
import { createTestRunner, preflightTestRun, type RunnerCallbacks, type RunnerHandle } from "@/lib/testRunner";
import { useI18n } from "@/lib/i18n";
import type { AutomationTaskControl } from "@/lib/automationTask";

type Tab = "editor" | "running" | "report";

const WorkflowComposer = lazy(() => import("./WorkflowComposer").then((module) => ({
  default: module.WorkflowComposer,
})));

const STORAGE_KEY = "linkr-test-script";

function loadScript(defaultName: string): TestScript {
  try {
    const saved = localStorage.getItem(STORAGE_KEY);
    if (saved) {
      const parsed = parseTestScript(saved, { validatePlan: false });
      return parsed.name === "New Test" ? { ...parsed, name: defaultName } : parsed;
    }
  } catch { /* ignore corrupted data */ }
  return defaultScript(defaultName);
}

const TAB_ICONS = { editor: PenLine, running: Play, report: ScrollText } as const;

export function TestAutomation({
  board,
  serialRef,
  taskControl,
  workspaceTabs,
  focusMode = false,
  onFocusModeChange,
}: {
  board: UseBoard;
  serialRef: React.RefObject<SerialAutomationHandle>;
  taskControl: AutomationTaskControl;
  workspaceTabs?: ReactNode;
  focusMode?: boolean;
  onFocusModeChange?: (focused: boolean) => void;
}) {
  const { t } = useI18n();
  const [tab, setTab] = useState<Tab>("editor");
  const [script, setScript] = useState<TestScript>(() => loadScript(t("test.defaultName")));
  const [draftState, setDraftState] = useState<"saving" | "saved" | "error">("saved");
  const [stepStates, setStepStates] = useState<Map<string, StepStatus>>(new Map());
  const [stepResults, setStepResults] = useState<StepResult[]>([]);
  const [serialLogs, setSerialLogs] = useState<SerialLogEntry[]>([]);
  const [adcSamples, setAdcSamples] = useState<AdcSampleEntry[]>([]);
  const [powerCaptures, setPowerCaptures] = useState<PowerCaptureEvidenceEntry[]>([]);
  const [runSummary, setRunSummary] = useState<RunSummary | null>(null);
  const [executedScript, setExecutedScript] = useState<TestScript | null>(null);
  const [startedAtMs, setStartedAtMs] = useState(0);
  const [isRunning, setIsRunning] = useState(false);
  const [runError, setRunError] = useState<string | null>(null);
  const serialLogsRef = useRef<SerialLogEntry[]>([]);
  const adcSamplesRef = useRef<AdcSampleEntry[]>([]);
  const powerCapturesRef = useRef<PowerCaptureEvidenceEntry[]>([]);
  const runnerRef = useRef<RunnerHandle | null>(null);
  const runningRef = useRef(false);
  const boardRef = useRef(board);
  boardRef.current = board;
  const execution = useMemo(() => tryBuildExecutionPlan(script), [script]);
  const executionPlan = execution.plan;
  const executedPlan = useMemo(
    () => executedScript ? tryBuildExecutionPlan(executedScript).plan : executionPlan,
    [executedScript, executionPlan],
  );

  useEffect(() => {
    try {
      // Persist drafts even when the execution plan is temporarily invalid
      // (e.g. empty loop body while composing). Run still validates strictly.
      localStorage.setItem(STORAGE_KEY, serializeTestScript(script));
      setDraftState("saved");
    } catch {
      // Keep the editor usable when storage is unavailable or full.
      setDraftState("error");
    }
  }, [script]);

  const handleScriptChange = useCallback((nextScript: TestScript) => {
    setDraftState("saving");
    setRunError(null);
    setScript(nextScript);
  }, []);

  const handleNew = useCallback(() => {
    if (!window.confirm(t("test.new.confirm"))) return;
    runnerRef.current?.abort();
    runningRef.current = false;
    taskControl.release("test");
    setScript(defaultScript(t("test.defaultName")));
    setDraftState("saving");
    setRunError(null);
    setStepStates(new Map());
    setStepResults([]);
    setSerialLogs([]);
    setAdcSamples([]);
    setPowerCaptures([]);
    setRunSummary(null);
    setExecutedScript(null);
    setIsRunning(false);
    setTab("editor");
  }, [t, taskControl.release]);

  useEffect(() => () => {
    runnerRef.current?.abort();
    taskControl.release("test");
  }, [taskControl.release]);

  const callbacks: RunnerCallbacks = useMemo(
    () => ({
      onStepStart: (id) => setStepStates((prev) => new Map(prev).set(id, "running")),
      onStepResult: (r) => {
        setStepResults((prev) => [...prev, r]);
        setStepStates((prev) => new Map(prev).set(r.stepId, r.status));
      },
      onSerialLog: (_id, channel, text, direction, timestampMs) => {
        const entry: SerialLogEntry = { stepId: _id, channel, text, direction, timestampMs };
        serialLogsRef.current.push(entry);
        setSerialLogs((prev) => {
          const next = [...prev, entry];
          return next.length > 1000 ? next.slice(-1000) : next;
        });
      },
      onAdcSample: (_id, channel, currentUa, timestampMs) => {
        const entry: AdcSampleEntry = { stepId: _id, channel, currentUa, timestampMs };
        adcSamplesRef.current.push(entry);
        setAdcSamples((prev) => {
          const next = [...prev, entry];
          return next.length > 1000 ? next.slice(-1000) : next;
        });
      },
      onPowerCapture: (evidence) => {
        powerCapturesRef.current.push(evidence);
        setPowerCaptures([...powerCapturesRef.current]);
      },
      onComplete: (summary) => {
        taskControl.release("test");
        runningRef.current = false;
        runnerRef.current = null;
        setIsRunning(false);
        setSerialLogs([...serialLogsRef.current]);
        setAdcSamples([...adcSamplesRef.current]);
        setPowerCaptures([...powerCapturesRef.current]);
        setRunSummary(summary);
        setTab("report");
      },
      onError: (err) => {
        console.error("[TestRunner]", err);
        taskControl.release("test");
        runningRef.current = false;
        runnerRef.current = null;
        setIsRunning(false);
        setRunError(err);
      },
    }),
    [taskControl.release],
  );

  const startRun = useCallback((sourceScript: TestScript) => {
    if (runningRef.current) return;
    const runExecution = tryBuildExecutionPlan(sourceScript);
    if (runExecution.error) {
      setRunError(runExecution.error);
      return;
    }
    const runScript = parseTestScript(serializeTestScript(sourceScript));
    try {
      preflightTestRun(runScript, boardRef.current, serialRef.current);
    } catch (reason) {
      const message = reason instanceof Error ? reason.message : String(reason);
      const serialMatch = /^(UART[01]) is required for this test but is not connected$/.exec(
        message,
      );
      if (message === "debug board is not connected") {
        setRunError(t("test.error.boardDisconnected"));
      } else if (serialMatch) {
        setRunError(t("test.error.serialDisconnected", { channel: serialMatch[1] }));
      } else {
        setRunError(message);
      }
      return;
    }
    if (!taskControl.acquire("test")) {
      setRunError(t("test.error.taskBusy"));
      return;
    }
    runningRef.current = true;
    setRunError(null);
    setIsRunning(true);
    setTab("running");
    setStepStates(new Map());
    setStepResults([]);
    serialLogsRef.current = [];
    adcSamplesRef.current = [];
    powerCapturesRef.current = [];
    setSerialLogs([]);
    setAdcSamples([]);
    setPowerCaptures([]);
    setRunSummary(null);
    setExecutedScript(runScript);
    const now = Date.now();
    setStartedAtMs(now);
    let runner: RunnerHandle;
    try {
      runner = createTestRunner(runScript, boardRef, serialRef, callbacks);
    } catch (reason) {
      callbacks.onError(reason instanceof Error ? reason.message : String(reason));
      return;
    }
    runnerRef.current = runner;
    void runner.start().catch((reason) => callbacks.onError(
      reason instanceof Error ? reason.message : String(reason),
    ));
  }, [callbacks, serialRef, t, taskControl.acquire]);

  const handleRun = useCallback(() => {
    startRun(script);
  }, [script, startRun]);

  const handleAbort = useCallback(() => {
    runnerRef.current?.abort();
  }, []);

  const handleReRun = useCallback(() => {
    startRun(executedScript ?? script);
  }, [executedScript, script, startRun]);

  const viewTabs = (
    <div
      className="automation-view-tabs inline-flex min-w-0 rounded-xl border border-line/70 bg-panel p-1"
      role="tablist"
      aria-label={t("test.title")}
    >
      {(["editor", "running", "report"] as Tab[]).map((t2) => {
        const Icon = TAB_ICONS[t2];
        const active = tab === t2;
        const disabled = isRunning ? t2 !== "running" : t2 === "running";
        const disabledReport = t2 === "report" && !runSummary;
        return (
          <button
            key={t2}
            id={`automation-tab-${t2}`}
            type="button"
            role="tab"
            aria-controls={`automation-panel-${t2}`}
            aria-selected={active}
            disabled={disabled || disabledReport}
            onClick={() => setTab(t2)}
            className={`flex min-h-8 items-center gap-1.5 rounded-lg px-2.5 py-1 text-[11px] font-semibold transition-colors ${
              active
                ? "bg-brand/10 text-brand ring-1 ring-inset ring-brand/15"
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

  const taskBusy = taskControl.owner != null && taskControl.owner !== "test";
  const runDisabledReason = !board.connected
    ? t("test.error.boardDisconnected")
    : taskBusy
      ? t("test.error.taskBusy")
      : undefined;

  return (
    <Card
      title={workspaceTabs ? undefined : t("test.title")}
      subtitle={workspaceTabs ? undefined : t("test.subtitle")}
      icon={FlaskConical}
      headerLeading={workspaceTabs}
      className="automation-workspace-shell min-h-[clamp(680px,76vh,980px)] xl:h-[clamp(680px,76vh,980px)] xl:min-h-0"
      contentClassName="flex min-h-0 flex-col overflow-hidden p-0"
    >
      <div className="automation-taskbar flex shrink-0 flex-wrap items-center justify-between gap-2 border-b border-line/60 bg-panel px-3 py-2">
        {viewTabs}
        {onFocusModeChange && (
          <Button
            variant="ghost"
            className="min-h-8 py-1 text-xs"
            aria-controls="hardware-controls"
            aria-expanded={!focusMode}
            aria-pressed={focusMode}
            id="automation-focus-toggle"
            onClick={() => onFocusModeChange(!focusMode)}
          >
            {focusMode ? <Minimize2 size={14} /> : <Maximize2 size={14} />}
            {focusMode ? t("test.focus.exit") : t("test.focus.enter")}
          </Button>
        )}
      </div>
      {runError && (
        <div
          className="automation-error mx-4 mt-3 flex shrink-0 items-start gap-2 rounded-lg border border-danger/30 bg-panel px-3 py-2 text-xs text-danger"
          data-testid="automation-error"
          role="alert"
        >
          <CircleAlert className="mt-0.5 shrink-0" size={14} />
          <span className="min-w-0 break-words">{runError}</span>
        </div>
      )}
      <div
        id={`automation-panel-${tab}`}
        role="tabpanel"
        aria-labelledby={`automation-tab-${tab}`}
        aria-busy={isRunning && tab === "running"}
        data-automation-view={tab}
        className={tab === "editor"
          ? "automation-view-region min-h-0 flex-1 overflow-hidden"
          : "automation-view-region min-h-0 flex-1 overflow-auto p-3 sm:p-4"}
      >
        {tab === "editor" && (
          <Suspense fallback={
            <div className="flex min-h-[420px] items-center justify-center gap-2 text-sm text-ink-dim">
              <Loader2 size={18} className="animate-spin text-brand" />
              {t("loading")}
            </div>
          }>
            <WorkflowComposer
              script={script}
              onChange={handleScriptChange}
              onNew={handleNew}
              draftState={draftState}
              onRun={handleRun}
              runDisabled={execution.error != null || taskBusy || !board.connected}
              runDisabledReason={runDisabledReason}
            />
          </Suspense>
        )}
        {tab === "running" && (
          <TestRunnerView
            steps={executedPlan}
            stepStates={stepStates}
            stepResults={stepResults}
            serialLogs={serialLogs}
            adcSamples={adcSamples}
            startedAtMs={startedAtMs}
            onAbort={handleAbort}
          />
        )}
        {tab === "report" && runSummary && executedScript && (
          <TestReport
            script={executedScript}
            summary={runSummary}
            serialLogs={serialLogs}
            adcSamples={adcSamples}
            powerCaptures={powerCaptures}
            onReRun={handleReRun}
          />
        )}
      </div>
    </Card>
  );
}
