import { useEffect, useMemo, useRef } from "react";
import { Activity, Loader2, Square, Terminal as TerminalIcon } from "lucide-react";
import { Badge, Button } from "./ui";
import type { TestStep, StepStatus, StepResult, SerialLogEntry, AdcSampleEntry } from "@/lib/testScript";
import { formatMs } from "@/lib/utils";
import { useI18n } from "@/lib/i18n";

const STATUS_STYLES: Record<StepStatus, string> = {
  pending: "text-ink-dim",
  running: "text-brand animate-pulse",
  pass: "text-ok",
  fail: "text-danger",
  skip: "text-warn",
  error: "text-danger",
  aborted: "text-warn",
};

const STATUS_ICONS: Record<StepStatus, string> = {
  pending: "○",
  running: "●",
  pass: "✓",
  fail: "✗",
  skip: "⊘",
  error: "⚠",
  aborted: "■",
};

function elapsedStr(startMs: number): string {
  const s = Math.floor((Date.now() - startMs) / 1000);
  const m = Math.floor(s / 60);
  return `${String(m).padStart(2, "0")}:${String(s % 60).padStart(2, "0")}`;
}

export interface TestRunnerViewProps {
  steps: TestStep[];
  stepStates: Map<string, StepStatus>;
  stepResults: StepResult[];
  serialLogs: SerialLogEntry[];
  adcSamples: AdcSampleEntry[];
  startedAtMs: number;
  onAbort: () => void;
}

export function TestRunnerView({
  steps,
  stepStates,
  stepResults,
  serialLogs,
  adcSamples,
  startedAtMs,
  onAbort,
}: TestRunnerViewProps) {
  const { t } = useI18n();
  const serialRef = useRef<HTMLDivElement>(null);
  const timerRef = useRef<HTMLSpanElement>(null);

  useEffect(() => {
    const id = window.setInterval(() => {
      if (timerRef.current) {
        timerRef.current.textContent = elapsedStr(startedAtMs);
      }
    }, 1000);
    return () => window.clearInterval(id);
  }, [startedAtMs]);

  useEffect(() => {
    if (serialRef.current) {
      serialRef.current.scrollTop = serialRef.current.scrollHeight;
    }
  }, [serialLogs.length]);

  const runningIndex = steps.findIndex((s) => stepStates.get(s.id) === "running");
  const doneCount = stepResults.length;
  const currentA = adcSamples.length > 0 ? (adcSamples[adcSamples.length - 1].currentUa / 1_000_000).toFixed(3) : "—";
  const resultMap = useMemo(() => new Map(stepResults.map((r) => [r.stepId, r])), [stepResults]);

  return (
    <div className="space-y-3">
      <div className="flex items-center gap-3">
        <Loader2 size={16} className="animate-spin text-brand" />
        <span className="text-sm font-semibold text-ink">
          {runningIndex >= 0
            ? t("test.running.step", { current: runningIndex + 1, total: steps.length })
            : t("test.running.step", { current: doneCount, total: steps.length })}
        </span>
        <span ref={timerRef} className="font-mono text-xs text-ink-dim">{elapsedStr(startedAtMs)}</span>
        {adcSamples.length > 0 && (
          <Badge tone="brand">
            <Activity size={10} />
            {currentA}A
          </Badge>
        )}
        <span className="flex-1" />
        <Button variant="danger" onClick={onAbort}>
          <Square size={14} />
          {t("test.abort")}
        </Button>
      </div>

      <div className="space-y-1">
        {steps.map((step, i) => {
          const status = stepStates.get(step.id) ?? "pending";
          const result = resultMap.get(step.id);
          return (
            <div
              key={step.id}
              className={`flex items-center gap-2 rounded-lg px-2 py-1.5 text-xs ${
                status === "running" ? "bg-brand/5" : ""
              }`}
            >
              <span className={`w-4 text-center text-sm ${STATUS_STYLES[status]}`}>
                {STATUS_ICONS[status]}
              </span>
              <span className="w-5 text-right text-[10px] text-ink-dim">{i + 1}</span>
              <span className="min-w-0 flex-1 truncate font-medium text-ink">
                {t(`test.step.${step.type}`)}
              </span>
              {result && (
                <span className="font-mono text-[10px] text-ink-dim">{formatMs(result.durationMs)}</span>
              )}
              {result?.error && (
                <span className="max-w-[200px] truncate text-[10px] text-danger" title={result.error}>
                  {result.error}
                </span>
              )}
              {result?.assertionResult && (
                <Badge tone={result.assertionResult.passed ? "ok" : "danger"}>
                  {result.assertionResult.passed ? t("test.status.pass") : t("test.status.fail")}
                </Badge>
              )}
              {status === "skip" && <Badge tone="warn">{t("test.status.skip")}</Badge>}
              {status === "aborted" && <Badge tone="warn">{t("test.status.aborted")}</Badge>}
            </div>
          );
        })}
      </div>

      {serialLogs.length > 0 && (
        <div className="rounded-xl border border-line/50 bg-panel2/30">
          <div className="flex items-center gap-2 border-b border-line/40 px-3 py-1.5">
            <TerminalIcon size={12} className="text-ink-dim" />
            <span className="text-[10px] font-semibold uppercase tracking-wider text-ink-dim">
              {t("test.report.serialLog")}
            </span>
            <span className="text-[10px] text-ink-dim">{t("test.report.entries", { n: serialLogs.length })}</span>
          </div>
          <div ref={serialRef} className="max-h-40 overflow-y-auto p-2 font-mono text-[10px] leading-relaxed text-ink-dim">
            {serialLogs.slice(-200).map((log, i) => (
              <div key={i} className={log.direction === "tx" ? "text-brand" : ""}>
                <span className="text-ink-dim/50">[{((log.timestampMs - startedAtMs) / 1000).toFixed(1)}s]</span>
                {log.direction === "tx" ? " → " : " "}
                {log.text.replace(/[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]/g, "").slice(0, 200)}
              </div>
            ))}
          </div>
        </div>
      )}
    </div>
  );
}
