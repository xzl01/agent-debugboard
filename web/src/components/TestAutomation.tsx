import { lazy, Suspense, useCallback, useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import { FlaskConical, Loader2, PenLine, Play, ScrollText } from "lucide-react";
import { Card } from "./ui";
import { TestRunnerView } from "./TestRunnerView";
import { TestReport } from "./TestReport";
import type { UseBoard } from "@/hooks/useBoard";
import type { SerialAutomationHandle } from "./SerialCard";
import type { TestScript, StepResult, StepStatus, RunSummary, AdcSampleEntry, SerialLogEntry } from "@/lib/testScript";
import { defaultScript, parseTestScript, serializeTestScript, tryBuildExecutionPlan } from "@/lib/testScript";
import { createTestRunner, type RunnerCallbacks, type RunnerHandle } from "@/lib/testRunner";
import { useI18n } from "@/lib/i18n";
import type { AutomationTaskControl } from "@/lib/automationTask";

type Tab = "editor" | "running" | "report";

const WorkflowComposer = lazy(() => import("./WorkflowComposer").then((module) => ({
  default: module.WorkflowComposer,
})));

const STORAGE_KEY = "linkr-test-script";

function loadScript(): TestScript {
  try {
    const saved = localStorage.getItem(STORAGE_KEY);
    if (saved) return parseTestScript(saved, { validatePlan: false });
  } catch { /* ignore corrupted data */ }
  return defaultScript();
}

const TAB_ICONS = { editor: PenLine, running: Play, report: ScrollText } as const;

export function TestAutomation({
  board,
  serialRef,
  taskControl,
  workspaceTabs,
}: {
  board: UseBoard;
  serialRef: React.RefObject<SerialAutomationHandle>;
  taskControl: AutomationTaskControl;
  workspaceTabs?: ReactNode;
}) {
  const { t } = useI18n();
  const [tab, setTab] = useState<Tab>("editor");
  const [script, setScript] = useState<TestScript>(loadScript);
  const [stepStates, setStepStates] = useState<Map<string, StepStatus>>(new Map());
  const [stepResults, setStepResults] = useState<StepResult[]>([]);
  const [serialLogs, setSerialLogs] = useState<SerialLogEntry[]>([]);
  const [adcSamples, setAdcSamples] = useState<AdcSampleEntry[]>([]);
  const [runSummary, setRunSummary] = useState<RunSummary | null>(null);
  const [startedAtMs, setStartedAtMs] = useState(0);
  const [isRunning, setIsRunning] = useState(false);
  const [runError, setRunError] = useState<string | null>(null);
  const runnerRef = useRef<RunnerHandle | null>(null);
  const runningRef = useRef(false);
  const boardRef = useRef(board);
  boardRef.current = board;
  const execution = useMemo(() => tryBuildExecutionPlan(script), [script]);
  const executionPlan = execution.plan;

  useEffect(() => {
    try {
      // Persist drafts even when the execution plan is temporarily invalid
      // (e.g. empty loop body while composing). Run still validates strictly.
      localStorage.setItem(STORAGE_KEY, serializeTestScript(script));
    } catch {
      // Keep the editor usable when storage is unavailable or full.
    }
  }, [script]);

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
      onSerialLog: (_id, text, direction) =>
        setSerialLogs((prev) => {
          const next = [...prev, { stepId: _id, text, direction, timestampMs: Date.now() }];
          return next.length > 10000 ? next.slice(-5000) : next;
        }),
      onAdcSample: (_id, channel, currentUa, timestampMs) =>
        setAdcSamples((prev) => {
          const next = [...prev, { stepId: _id, channel, currentUa, timestampMs }];
          return next.length > 5000 ? next.slice(-2500) : next;
        }),
      onComplete: (summary) => {
        taskControl.release("test");
        runningRef.current = false;
        runnerRef.current = null;
        setIsRunning(false);
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

  const handleRun = useCallback(() => {
    if (runningRef.current) return;
    if (execution.error) {
      setRunError(execution.error);
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
    setSerialLogs([]);
    setAdcSamples([]);
    setRunSummary(null);
    const now = Date.now();
    setStartedAtMs(now);
    let runner: RunnerHandle;
    try {
      runner = createTestRunner(script, boardRef, serialRef, callbacks);
    } catch (reason) {
      callbacks.onError(reason instanceof Error ? reason.message : String(reason));
      return;
    }
    runnerRef.current = runner;
    void runner.start().catch((reason) => callbacks.onError(
      reason instanceof Error ? reason.message : String(reason),
    ));
  }, [callbacks, execution.error, script, serialRef, t, taskControl.acquire]);

  const handleAbort = useCallback(() => {
    runnerRef.current?.abort();
  }, []);

  const handleReRun = useCallback(() => {
    handleRun();
  }, [handleRun]);

  const viewTabs = (
    <div
      className="inline-flex rounded-xl border border-line/70 bg-panel2 p-1"
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
            type="button"
            role="tab"
            aria-selected={active}
            disabled={disabled || disabledReport}
            onClick={() => setTab(t2)}
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

  return (
    <Card
      title={workspaceTabs ? undefined : t("test.title")}
      subtitle={workspaceTabs ? undefined : t("test.subtitle")}
      icon={FlaskConical}
      headerLeading={workspaceTabs}
      className="min-h-[clamp(680px,76vh,980px)] xl:h-[clamp(680px,76vh,980px)] xl:min-h-0"
      contentClassName="flex min-h-0 flex-col overflow-hidden p-0"
    >
      <div className="flex shrink-0 items-center border-b border-line/60 bg-panel2/35 px-3 py-2">
        {viewTabs}
      </div>
      {runError && (
        <div className="mx-4 mt-3 shrink-0 rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
          {runError}
        </div>
      )}
      <div className={tab === "editor" ? "min-h-0 flex-1 overflow-hidden" : "min-h-0 flex-1 overflow-auto p-4"}>
        {tab === "editor" && (
          <Suspense fallback={
            <div className="flex min-h-[420px] items-center justify-center gap-2 text-sm text-ink-dim">
              <Loader2 size={18} className="animate-spin text-brand" />
              {t("loading")}
            </div>
          }>
            <WorkflowComposer
              script={script}
              onChange={setScript}
              onRun={handleRun}
              runDisabled={execution.error != null || (taskControl.owner != null && taskControl.owner !== "test")}
            />
          </Suspense>
        )}
        {tab === "running" && (
          <TestRunnerView
            steps={executionPlan}
            stepStates={stepStates}
            stepResults={stepResults}
            serialLogs={serialLogs}
            adcSamples={adcSamples}
            startedAtMs={startedAtMs}
            onAbort={handleAbort}
          />
        )}
        {tab === "report" && runSummary && (
          <TestReport
            script={script}
            summary={runSummary}
            serialLogs={serialLogs}
            adcSamples={adcSamples}
            onReRun={handleReRun}
          />
        )}
      </div>
    </Card>
  );
}
