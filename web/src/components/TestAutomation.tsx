import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { FlaskConical, PenLine, Play, ScrollText } from "lucide-react";
import { Card } from "./ui";
import { TestEditor } from "./TestEditor";
import { TestRunnerView } from "./TestRunnerView";
import { TestReport } from "./TestReport";
import type { UseBoard } from "@/hooks/useBoard";
import type { SerialAutomationHandle } from "./SerialCard";
import type { TestScript, StepResult, StepStatus, RunSummary, AdcSampleEntry, SerialLogEntry } from "@/lib/testScript";
import { defaultScript, parseTestScript, serializeTestScript } from "@/lib/testScript";
import { createTestRunner, type RunnerCallbacks, type RunnerHandle } from "@/lib/testRunner";
import { useI18n } from "@/lib/i18n";

type Tab = "editor" | "running" | "report";

const STORAGE_KEY = "linkr-test-script";

function loadScript(): TestScript {
  try {
    const saved = localStorage.getItem(STORAGE_KEY);
    if (saved) return parseTestScript(saved);
  } catch { /* ignore corrupted data */ }
  return defaultScript();
}

const TAB_ICONS = { editor: PenLine, running: Play, report: ScrollText } as const;

export function TestAutomation({
  board,
  serialRef,
}: {
  board: UseBoard;
  serialRef: React.RefObject<SerialAutomationHandle>;
}) {
  const { t } = useI18n();
  const [tab, setTab] = useState<Tab>("editor");
  const [script, setScript] = useState<TestScript>(loadScript);

  useEffect(() => {
    localStorage.setItem(STORAGE_KEY, serializeTestScript(script));
  }, [script]);
  const [stepStates, setStepStates] = useState<Map<string, StepStatus>>(new Map());
  const [stepResults, setStepResults] = useState<StepResult[]>([]);
  const [serialLogs, setSerialLogs] = useState<SerialLogEntry[]>([]);
  const [adcSamples, setAdcSamples] = useState<AdcSampleEntry[]>([]);
  const [runSummary, setRunSummary] = useState<RunSummary | null>(null);
  const [startedAtMs, setStartedAtMs] = useState(0);
  const [isRunning, setIsRunning] = useState(false);
  const runnerRef = useRef<RunnerHandle | null>(null);
  const runningRef = useRef(false);
  const boardRef = useRef(board);
  boardRef.current = board;

  useEffect(() => () => runnerRef.current?.abort(), []);

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
        runningRef.current = false;
        runnerRef.current = null;
        setIsRunning(false);
        setRunSummary(summary);
        setTab("report");
      },
      onError: (err) => console.error("[TestRunner]", err),
    }),
    [],
  );

  const handleRun = useCallback(() => {
    if (runningRef.current) return;
    runningRef.current = true;
    setIsRunning(true);
    setTab("running");
    setStepStates(new Map());
    setStepResults([]);
    setSerialLogs([]);
    setAdcSamples([]);
    setRunSummary(null);
    const now = Date.now();
    setStartedAtMs(now);
    const runner = createTestRunner(script, boardRef, serialRef, callbacks);
    runnerRef.current = runner;
    void runner.start();
  }, [script, serialRef, callbacks]);

  const handleAbort = useCallback(() => {
    runnerRef.current?.abort();
  }, []);

  const handleReRun = useCallback(() => {
    handleRun();
  }, [handleRun]);

  return (
    <Card
      title={t("test.title")}
      subtitle={t("test.subtitle")}
      icon={FlaskConical}
      right={
        <div className="inline-flex rounded-xl border border-line/70 bg-panel2 p-1" role="tablist">
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
                className={`flex items-center gap-1.5 rounded-lg px-2.5 py-1 text-[11px] font-semibold transition-colors ${
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
      }
    >
      {tab === "editor" && (
        <TestEditor script={script} onChange={setScript} onRun={handleRun} />
      )}
      {tab === "running" && (
        <TestRunnerView
          steps={script.steps}
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
    </Card>
  );
}
