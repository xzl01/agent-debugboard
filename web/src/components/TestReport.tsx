import { useEffect, useMemo, useState } from "react";
import {
  Activity,
  CircleDot,
  Download,
  FileJson,
  FileSpreadsheet,
  FileText,
  GitBranch,
  Radio,
  RotateCcw,
  ShieldCheck,
  Terminal as TerminalIcon,
  TriangleAlert,
  XCircle,
} from "lucide-react";
import { Badge, Button } from "./ui";
import type {
  AdcSampleEntry,
  ExecutionStep,
  PowerCaptureEvidenceEntry,
  RunSummary,
  SerialLogEntry,
  StepResult,
  StepStatus,
  TestScript,
} from "@/lib/testScript";
import { isRunSuccessful, tryBuildExecutionPlan } from "@/lib/testScript";
import { summarizePowerCapture } from "@/lib/powerCapture";
import { exportPowerCaptureToFile } from "@/lib/powerCaptureExport";
import { downloadBlob, formatMs } from "@/lib/utils";
import { useI18n } from "@/lib/i18n";
import { buildExecutionGroups, type ExecutionGroup } from "./TestRunnerView";

type OverallStatus = "pass" | "partial" | "fail";

function statusTone(status: StepStatus): "neutral" | "ok" | "danger" {
  if (status === "pass") return "ok";
  if (status === "fail" || status === "error") return "danger";
  return "neutral";
}

function statusLabel(status: StepStatus, t: ReturnType<typeof useI18n>["t"]): string {
  if (status === "pending") return "—";
  return t(`test.status.${status}`);
}

function resultGroupStatus(group: ExecutionGroup, resultMap: Map<string, StepResult>): StepStatus {
  const results = group.steps
    .map((step) => resultMap.get(step.executionId))
    .filter((result): result is StepResult => result != null);
  if (results.length < group.steps.length) return "pending";
  return aggregateStatus(results);
}

function groupTitle(group: ExecutionGroup, t: ReturnType<typeof useI18n>["t"]): string {
  if (group.kind === "condition") return t("test.condition.title");
  if (group.kind === "loop") return t("test.loop.title");
  if (group.kind === "unit") return group.unitName ?? t("test.loop.title");
  return t(`test.step.${group.steps[0].type}`);
}

function executionPathKey(step: ExecutionStep): string {
  return [
    step.sourceStepId,
    step.conditionId ?? "",
    step.conditionRole ?? "",
    step.loopId ?? "",
    step.unitId ?? "",
  ].join(":");
}

function sameExecutionPath(left: ExecutionStep, right: ExecutionStep): boolean {
  return executionPathKey(left) === executionPathKey(right);
}

function uniqueGroupChildren(group: ExecutionGroup): ExecutionStep[] {
  const seen = new Set<string>();
  return group.steps.filter((step) => {
    const key = executionPathKey(step);
    if (seen.has(key)) return false;
    seen.add(key);
    return true;
  });
}

function relatedResults(step: ExecutionStep, group: ExecutionGroup, resultMap: Map<string, StepResult>): StepResult[] {
  return group.steps
    .filter((candidate) => sameExecutionPath(candidate, step))
    .map((candidate) => resultMap.get(candidate.executionId))
    .filter((result): result is StepResult => result != null);
}

function aggregateStatus(results: StepResult[]): StepStatus {
  const effectiveResults = results.filter((result) => !result.conditionalSkip);
  if (effectiveResults.length === 0 && results.length > 0) return "pass";
  for (const status of ["error", "fail", "aborted", "skip", "pending", "pass"] as StepStatus[]) {
    if (effectiveResults.some((result) => result.status === status)) return status;
  }
  return "pending";
}

function EvidenceBars({ samples, incomplete }: { samples: number[]; incomplete: boolean }) {
  const points = samples.length > 0
    ? samples.filter((_, index) => index % Math.max(1, Math.ceil(samples.length / 24)) === 0).slice(0, 24)
    : [];
  const max = Math.max(...points, 1);

  return (
    <div data-testid="report-power-chart" className="flex h-24 items-end gap-1 rounded-xl bg-panel2/70 px-3 pb-3 pt-5">
      {points.length === 0 ? (
        <div className="m-auto font-mono text-[10px] text-ink-dim">—</div>
      ) : points.map((value, index) => (
        <span
          key={index}
          className={`min-w-0 flex-1 rounded-sm ${incomplete && index >= Math.floor(points.length * 0.65) ? "bg-warn/80" : "bg-ink-dim/45"}`}
          style={{ height: `${Math.max(8, (value / max) * 100)}%` }}
        />
      ))}
    </div>
  );
}

export interface TestReportProps {
  script: TestScript;
  summary: RunSummary;
  serialLogs: SerialLogEntry[];
  adcSamples: AdcSampleEntry[];
  powerCaptures: PowerCaptureEvidenceEntry[];
  onReRun: () => void;
}

export function TestReport({ script, summary, serialLogs, adcSamples, powerCaptures, onReRun }: TestReportProps) {
  const { t } = useI18n();
  const [captureExportError, setCaptureExportError] = useState<string | null>(null);
  const execution = useMemo(() => tryBuildExecutionPlan(script).plan, [script]);
  const groups = useMemo(() => buildExecutionGroups(execution), [execution]);
  const resultMap = useMemo(() => new Map(summary.results.map((result) => [result.stepId, result])), [summary.results]);
  const incompleteCaptures = useMemo(
    () => powerCaptures.filter(({ capture }) => capture.incomplete || (capture.droppedSamples ?? 0) > 0),
    [powerCaptures],
  );
  const incompleteStepIds = useMemo(() => new Set(incompleteCaptures.map((capture) => capture.stepId)), [incompleteCaptures]);
  const missingResultCount = Math.max(0, summary.totalSteps - summary.results.length);
  const hasHardFailure = summary.cleanup?.passed === false
    || summary.results.some((result) => (
      (result.status === "fail" || result.status === "error") && !incompleteStepIds.has(result.stepId)
    ));
  const overallStatus: OverallStatus = isRunSuccessful(summary)
    ? "pass"
    : !summary.aborted && (incompleteCaptures.length > 0 || missingResultCount > 0) && !hasHardFailure
      ? "partial"
      : "fail";
  const initialSelectedId = incompleteCaptures[0]?.stepId
    ?? summary.results.find((result) => result.status === "fail" || result.status === "error")?.stepId
    ?? summary.results[0]?.stepId
    ?? "";
  const [selectedStepId, setSelectedStepId] = useState(initialSelectedId);

  useEffect(() => {
    if (!resultMap.has(selectedStepId)) setSelectedStepId(initialSelectedId);
  }, [initialSelectedId, resultMap, selectedStepId]);

  const selectedResult = resultMap.get(selectedStepId) ?? summary.results[0];
  const selectedCapture = powerCaptures.find(({ stepId }) => stepId === selectedResult?.stepId);
  const selectedLogs = selectedResult
    ? serialLogs.filter((entry) => entry.stepId === selectedResult.stepId)
    : serialLogs;
  const selectedSamples = selectedResult
    ? adcSamples.filter((entry) => entry.stepId === selectedResult.stepId)
    : adcSamples;
  const powerSummary = selectedCapture
    ? summarizePowerCapture(selectedCapture.capture, selectedCapture.rail)
    : undefined;
  const chartValues = selectedSamples.length > 0
    ? selectedSamples.map((sample) => sample.currentUa)
    : selectedCapture?.capture.samples.map((sample) => {
        const reading = sample.readings.find((candidate) => candidate.name === selectedCapture.rail || candidate.signal === selectedCapture.rail);
        return reading?.current_ua ?? 0;
      }) ?? [];
  const selectedIncomplete = selectedCapture != null
    && (selectedCapture.capture.incomplete || (selectedCapture.capture.droppedSamples ?? 0) > 0);

  const handleExportJson = () => {
    const report = {
      schema: "linkr-test-report.v1",
      script: { name: script.name, steps: script.steps },
      summary: {
        total: summary.totalSteps,
        passed: summary.passed,
        failed: summary.failed,
        skipped: summary.skipped,
        errored: summary.errored,
        aborted: summary.aborted,
        completed: summary.completed,
        duration_ms: summary.durationMs,
        infrastructure_error: summary.infrastructureError,
        cleanup: summary.cleanup,
      },
      results: summary.results,
      adc_samples: adcSamples,
      power_captures: powerCaptures.map(({ stepId, rail, capture }) => ({
        step_id: stepId,
        rail,
        capture_id: capture.id,
        archive_id: capture.archiveId ?? null,
        sample_count: capture.sampleCount ?? capture.samples.length,
        dropped_samples: capture.droppedSamples ?? 0,
        incomplete: capture.incomplete ?? false,
        interruption_reason: capture.interruptionReason ?? null,
        summaries: capture.summaries ?? null,
      })),
      serial_log: serialLogs,
    };
    downloadBlob(`report-${script.name.replace(/\s+/g, "-")}.json`, JSON.stringify(report, null, 2), "application/json");
  };

  const handleExportCsv = () => {
    const header = "step_id,status,duration_ms,error,detail,adc_ua,unit_id,unit_name,condition_id,condition_role,condition_outcome,conditional_skip";
    const rows = summary.results.map((result) => [
      result.stepId,
      result.status,
      result.durationMs,
      result.error ?? "",
      result.assertionResult?.detail ?? "",
      result.adcValueUa ?? "",
      result.unitId ?? "",
      result.unitName ?? "",
      result.conditionId ?? "",
      result.conditionRole ?? "",
      result.conditionOutcome ?? "",
      result.conditionalSkip ?? false,
    ].map((value) => (/[",\n]/.test(String(value)) ? `"${String(value).replaceAll('"', '""')}"` : value)).join(","));
    downloadBlob(`report-${script.name.replace(/\s+/g, "-")}.csv`, `${[header, ...rows].join("\n")}\n`, "text/csv");
  };

  const handleExportNdjson = () => {
    const lines = summary.results.map((result) => JSON.stringify({ type: "step_result", ...result }));
    downloadBlob(`report-${script.name.replace(/\s+/g, "-")}.ndjson`, `${lines.join("\n")}\n`, "application/x-ndjson");
  };

  const overallLabel = summary.aborted
    ? t("test.status.aborted")
    : overallStatus === "pass"
      ? t("test.status.pass")
      : overallStatus === "fail"
        ? t("test.status.fail")
        : t("test.report.partial");
  const completedResults = summary.results.length;
  const incompleteLabel = (hasMissingResults: boolean) => (
    hasMissingResults && summary.aborted ? t("test.status.aborted") : t("test.report.partial")
  );

  return (
    <div data-testid="automation-report-workspace" data-overall-status={overallStatus} className="flex h-full min-h-[720px] flex-col gap-3">
      <header className="flex shrink-0 flex-wrap items-center gap-3 rounded-2xl border border-line/70 bg-panel px-4 py-3">
        <div className="min-w-[250px]">
          <div className="flex items-center gap-2">
            <h2 className="text-sm font-semibold text-ink">{script.name}</h2>
            <Badge tone={overallStatus === "pass" ? "ok" : overallStatus === "fail" ? "danger" : "warn"}>{overallLabel}</Badge>
          </div>
          <div className="font-mono text-[9px] text-ink-dim">{new Date(summary.startedAtMs).toLocaleString()}</div>
        </div>
        <div className="min-w-[240px] flex-1">
          <div className="text-xs font-semibold text-ink">
            {t("test.report.resultCount", { actual: completedResults, expected: summary.totalSteps })}
          </div>
          <div className={`font-mono text-[9px] ${overallStatus === "partial" ? "text-warn" : "text-ink-dim"}`}>
            {formatMs(summary.durationMs)} · {t("test.report.completedCount", { n: completedResults, total: summary.totalSteps })}
          </div>
        </div>
        <Button variant="default" onClick={onReRun} className="min-h-9 px-3 py-1.5 text-xs">
          <RotateCcw size={13} />{t("test.report.rerun")}
        </Button>
        <Button variant="primary" onClick={handleExportJson} className="min-h-9 px-4 py-1.5 text-xs" data-testid="report-export-primary">
          <Download size={13} />{t("test.report.exportJson")}
        </Button>
      </header>

      <div className="grid min-h-0 flex-1 gap-3 xl:grid-cols-[238px_minmax(440px,1fr)_368px]">
        <aside className="min-h-0 overflow-hidden rounded-2xl border border-line/70 bg-panel" data-testid="report-result-navigation">
          <div className="border-b border-line/60 px-3.5 py-3">
            <div className="flex items-center gap-2">
              <span className="text-sm font-semibold text-ink">{t("test.report.stepResults")}</span>
              <Badge tone="neutral" className="ml-auto">{groups.length}</Badge>
            </div>
            <div className="text-[9px] text-ink-dim">{t("test.report.timeline")}</div>
          </div>
          <div className="max-h-[calc(100%-168px)] space-y-1.5 overflow-y-auto p-2">
            {groups.map((group, index) => {
              const status = resultGroupStatus(group, resultMap);
              const firstResult = group.steps.map((step) => resultMap.get(step.executionId)).find(Boolean);
              const actualResults = group.steps.filter((step) => resultMap.has(step.executionId)).length;
              const hasMissingResults = actualResults < group.steps.length;
              const isIncomplete = hasMissingResults || group.steps.some((step) => incompleteStepIds.has(step.executionId));
              const selected = group.steps.some((step) => step.executionId === selectedStepId);
              return (
                <button
                  key={group.id}
                  type="button"
                  data-testid={`report-nav-${group.id}`}
                  onClick={() => firstResult && setSelectedStepId(firstResult.stepId)}
                  className={`flex w-full items-center gap-2 rounded-xl border px-2 py-2 text-left transition-colors ${
                    selected
                      ? isIncomplete ? "border-warn bg-warn/5" : "border-brand/60 bg-brand/5"
                      : "border-line/60 bg-panel hover:bg-panel2/60"
                  }`}
                >
                  <span className={`grid h-7 w-9 shrink-0 place-items-center rounded-lg font-mono text-[9px] font-semibold ${
                    group.kind === "loop" || group.kind === "condition" || group.kind === "unit"
                      ? "bg-violet-500/10 text-violet-600 dark:text-violet-300"
                      : status === "pass" ? "bg-ok/10 text-ok" : "bg-panel2 text-ink-dim"
                  }`}>
                    {String(index + 1).padStart(2, "0")}
                  </span>
                  <span className="min-w-0 flex-1">
                    <span className="block truncate text-[11px] font-semibold text-ink">{groupTitle(group, t)}</span>
                    <span className={`block truncate font-mono text-[9px] ${isIncomplete ? "text-warn" : "text-ink-dim"}`}>
                      {t("test.report.resultCount", { actual: actualResults, expected: group.steps.length })} · {group.steps.reduce((total, step) => total + (resultMap.get(step.executionId)?.durationMs ?? 0), 0)}ms
                    </span>
                  </span>
                  <Badge tone={isIncomplete ? "warn" : statusTone(status)}>{isIncomplete ? incompleteLabel(hasMissingResults) : statusLabel(status, t)}</Badge>
                </button>
              );
            })}
          </div>
          <div className="m-2 rounded-xl bg-panel2/70 p-3 text-[10px] text-ink-dim">
            <div className="mb-2 flex items-center gap-2 font-semibold text-ink">
              {summary.cleanup?.passed === false ? <XCircle size={13} className="text-danger" /> : <ShieldCheck size={13} className="text-ok" />}
              {summary.cleanup?.passed === false ? t("test.report.cleanupFailed") : t("test.report.cleanupPassed")}
            </div>
            <div className="font-mono">{formatMs(summary.durationMs)}</div>
          </div>
        </aside>

        <main className="min-h-0 overflow-hidden rounded-2xl border border-line/70 bg-panel" data-testid="report-hierarchical-timeline">
          <div className="flex items-center justify-between border-b border-line/60 px-4 py-3">
            <div>
              <div className="text-sm font-semibold text-ink">{t("test.report.timeline")}</div>
              <div className="text-[9px] text-ink-dim">{t("test.report.stepResults")}</div>
            </div>
            <Badge tone="neutral">{formatMs(summary.durationMs)}</Badge>
          </div>
          <div className="relative max-h-[calc(100%-61px)] space-y-2 overflow-y-auto p-3 before:absolute before:bottom-6 before:left-[27px] before:top-7 before:w-px before:bg-line">
            <div className="relative z-[1] flex items-center gap-3 rounded-xl border border-line/60 bg-panel px-3 py-2.5">
              <span className="grid h-7 w-7 place-items-center rounded-full border border-ok/50 bg-panel text-ok"><CircleDot size={14} /></span>
              <span className="text-xs font-semibold text-ink">Workflow</span>
              <span className="flex-1" />
              <Badge tone="ok">00:00</Badge>
            </div>

            {groups.map((group, groupIndex) => {
              const groupResults = group.steps.map((step) => resultMap.get(step.executionId)).filter((result): result is StepResult => result != null);
              const groupResultStatus = aggregateStatus(groupResults);
              const groupHasMissingResults = groupResults.length < group.steps.length;
              const incomplete = groupHasMissingResults || group.steps.some((step) => incompleteStepIds.has(step.executionId));
              const duration = groupResults.reduce((total, result) => total + result.durationMs, 0);

              if (group.kind === "step") {
                const step = group.steps[0];
                const result = resultMap.get(step.executionId);
                return (
                  <button
                    key={group.id}
                    type="button"
                    onClick={() => result && setSelectedStepId(result.stepId)}
                    className={`relative z-[1] ml-3 flex w-[calc(100%-0.75rem)] items-center gap-2 rounded-xl border px-3 py-2 text-left ${
                      selectedStepId === result?.stepId
                        ? incomplete ? "border-warn bg-warn/5" : "border-brand/60 bg-brand/5"
                        : "border-line/60 bg-panel"
                    }`}
                  >
                    <span className="grid h-7 w-10 place-items-center rounded-lg bg-panel2 font-mono text-[9px] text-ink-dim">{String(groupIndex + 1).padStart(2, "0")}</span>
                    <span className="min-w-0 flex-1"><span className="block text-xs font-semibold text-ink">{t(`test.step.${step.type}`)}</span><span className="block truncate font-mono text-[9px] text-ink-dim">{result?.assertionResult?.detail ?? result?.error ?? step.sourceStepId}</span></span>
                    <span className="font-mono text-[9px] text-ink-dim">{formatMs(result?.durationMs ?? 0)}</span>
                    <Badge tone={incomplete ? "warn" : statusTone(result?.status ?? "pending")}>{incomplete ? incompleteLabel(result == null) : statusLabel(result?.status ?? "pending", t)}</Badge>
                  </button>
                );
              }

              const children = uniqueGroupChildren(group);
              return (
                <section key={group.id} data-group-kind={group.kind} className={`relative z-[1] ml-3 rounded-xl border bg-panel p-2.5 ${incomplete ? "border-warn" : "border-line/70"}`}>
                  <div className="mb-2 flex items-center gap-2">
                    <span className="grid h-7 w-10 place-items-center rounded-lg bg-violet-500/10 font-mono text-[9px] font-semibold text-violet-600 dark:text-violet-300">{String(groupIndex + 1).padStart(2, "0")}</span>
                    {group.kind === "condition" ? <GitBranch size={14} className="text-violet-500" /> : <Radio size={14} className="text-violet-500" />}
                    <span className="min-w-0 flex-1"><span className="block text-xs font-semibold text-ink">{groupTitle(group, t)}</span><span className="block font-mono text-[9px] text-ink-dim">{t("test.report.resultCount", { actual: groupResults.length, expected: group.steps.length })} · {formatMs(duration)}</span></span>
                    <Badge tone={incomplete ? "warn" : statusTone(groupResultStatus)}>{incomplete ? incompleteLabel(groupHasMissingResults) : statusLabel(groupResultStatus, t)}</Badge>
                  </div>
                  <div className="space-y-1.5 border-l border-violet-300/70 pl-4 dark:border-violet-400/30">
                    {children.map((step, childIndex) => {
                      const expectedSteps = group.steps.filter((candidate) => sameExecutionPath(candidate, step));
                      const results = relatedResults(step, group, resultMap);
                      const status = aggregateStatus(results);
                      const selected = results.some((result) => result.stepId === selectedStepId);
                      const childHasMissingResults = results.length < expectedSteps.length;
                      const childIncomplete = childHasMissingResults || results.some((result) => incompleteStepIds.has(result.stepId));
                      return (
                        <button
                          key={executionPathKey(step)}
                          type="button"
                          data-unit-id={step.unitId}
                          onClick={() => results[0] && setSelectedStepId(results[0].stepId)}
                          className={`flex w-full items-center gap-2 rounded-lg border px-2.5 py-2 text-left ${selected ? childIncomplete ? "border-warn bg-warn/5" : "border-brand/60 bg-brand/5" : "border-line/50 bg-panel2/35"}`}
                        >
                          <span className="w-9 font-mono text-[9px] text-ink-dim">{groupIndex + 1}.{childIndex + 1}</span>
                          <span className="min-w-0 flex-1"><span className="flex items-center gap-2 text-[11px] font-semibold text-ink">{t(`test.step.${step.type}`)}{step.unitName && <span className="rounded-full border border-line/70 px-2 py-0.5 text-[9px] font-medium text-ink-dim">{step.unitName}</span>}</span><span className="block truncate font-mono text-[9px] text-ink-dim">{t("test.report.resultCount", { actual: results.length, expected: expectedSteps.length })} · {formatMs(results.reduce((total, result) => total + result.durationMs, 0))}</span></span>
                          {step.conditionRole && <span className="rounded-full bg-violet-500/10 px-2 py-0.5 text-[9px] text-violet-600 dark:text-violet-300">{t(`test.condition.branch.${step.conditionRole}`)}</span>}
                          <Badge tone={childIncomplete ? "warn" : statusTone(status)}>{childIncomplete ? incompleteLabel(childHasMissingResults) : statusLabel(status, t)}</Badge>
                        </button>
                      );
                    })}
                  </div>
                </section>
              );
            })}

            <div className="relative z-[1] ml-3 flex items-center gap-3 rounded-xl border border-line/60 bg-panel px-3 py-2.5">
              <span className="grid h-7 w-7 place-items-center rounded-full border border-ok/50 bg-panel text-ok"><ShieldCheck size={14} /></span>
              <span className="text-xs font-semibold text-ink">{summary.cleanup?.passed === false ? t("test.report.cleanupFailed") : t("test.report.cleanupPassed")}</span>
              <span className="flex-1" />
              <Badge tone={summary.cleanup?.passed === false ? "danger" : "ok"}>{summary.cleanup?.passed === false ? t("test.status.fail") : t("test.status.pass")}</Badge>
            </div>
          </div>
        </main>

        <aside className="min-h-0 overflow-hidden rounded-2xl border border-line/70 bg-panel" data-testid="report-evidence-inspector">
          <div className="flex items-start gap-2 border-b border-line/60 px-3.5 py-3">
            <div className="min-w-0 flex-1">
              <div className="text-sm font-semibold text-ink">{t("test.report.powerEvidence")}</div>
              <div className="truncate font-mono text-[9px] text-ink-dim">{selectedResult?.stepId ?? "—"}</div>
            </div>
            <Badge tone={selectedIncomplete ? "warn" : statusTone(selectedResult?.status ?? "pending")}>
              {selectedIncomplete ? "PARTIAL" : statusLabel(selectedResult?.status ?? "pending", t)}
            </Badge>
          </div>

          <div className="max-h-[calc(100%-61px)] space-y-3 overflow-y-auto p-3">
            <section>
              <div className="mb-2 flex items-center gap-2 text-[11px] font-semibold text-ink"><Activity size={13} className="text-ink-dim" />{selectedCapture?.rail ?? t(`test.step.${selectedResult?.stepType ?? "capture"}`)}</div>
              <div className="mb-2 grid grid-cols-3 gap-2">
                <div><div className="text-[9px] text-ink-dim">{t("test.report.samples")}</div><div className="font-mono text-[11px] font-semibold text-ink">{selectedCapture ? (selectedCapture.capture.sampleCount ?? selectedCapture.capture.samples.length).toLocaleString() : selectedSamples.length.toLocaleString()}</div></div>
                <div><div className="text-[9px] text-ink-dim">Peak</div><div className="font-mono text-[11px] font-semibold text-ink">{powerSummary ? `${(powerSummary.peakCurrentA * 1000).toFixed(1)} mA` : selectedSamples.length ? `${(Math.max(...selectedSamples.map((sample) => sample.currentUa)) / 1000).toFixed(1)} mA` : "—"}</div></div>
                <div><div className="text-[9px] text-ink-dim">Energy</div><div className="font-mono text-[11px] font-semibold text-ink">{powerSummary ? `${(powerSummary.wattHours * 3600).toFixed(2)} J` : "—"}</div></div>
              </div>
              <EvidenceBars samples={chartValues} incomplete={selectedIncomplete} />
            </section>

            {selectedIncomplete && selectedCapture && (
              <section data-testid="report-incomplete-warning" className="rounded-xl border border-warn/70 bg-warn/5 p-3 text-warn">
                <div className="flex items-center gap-2 text-[11px] font-semibold"><TriangleAlert size={13} />{t("test.report.incomplete")}</div>
                <div className="mt-1 font-mono text-[9px]">dropped_samples {selectedCapture.capture.droppedSamples ?? 0} · {selectedCapture.capture.interruptionReason ?? "incomplete"}</div>
              </section>
            )}

            <section className="border-t border-line/60 pt-3">
              <div className="mb-2 flex items-center gap-2 text-[11px] font-semibold text-ink"><TerminalIcon size={13} className="text-ink-dim" />{t("test.report.serialLog")}<Badge tone="neutral" className="ml-auto">{t("test.report.entries", { n: selectedLogs.length })}</Badge></div>
              <div className="max-h-36 overflow-y-auto rounded-xl bg-panel2/70 p-3 font-mono text-[9px] leading-5 text-ink-dim">
                {selectedLogs.length === 0 ? <div>—</div> : selectedLogs.slice(-80).map((log, index) => (
                  <div key={`${log.timestampMs}-${index}`} className={log.direction === "tx" ? "text-brand" : "text-ink"}>
                    [{((log.timestampMs - summary.startedAtMs) / 1000).toFixed(1)}s] {log.direction === "tx" ? "→ " : ""}{log.text.replace(/[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]/g, "").slice(0, 300)}
                  </div>
                ))}
              </div>
            </section>

            <section className="border-t border-line/60 pt-3">
              <div className="mb-2 flex items-center gap-2 text-[11px] font-semibold text-ink"><ShieldCheck size={13} className={summary.cleanup?.passed === false ? "text-danger" : "text-ok"} />{summary.cleanup?.passed === false ? t("test.report.cleanupFailed") : t("test.report.cleanupPassed")}</div>
              {summary.infrastructureError && <div className={`rounded-lg px-2.5 py-2 text-[9px] ${overallStatus === "partial" ? "bg-warn/5 text-warn" : "bg-danger/5 text-danger"}`}>{summary.infrastructureError}</div>}
            </section>

            <section className="border-t border-line/60 pt-3">
              <div className="mb-2 text-[10px] font-semibold text-ink-dim">{t("test.report.powerEvidence")}</div>
              {selectedCapture && (
                <div className="mb-2 flex gap-2">
                  {(["csv", "ndjson"] as const).map((format) => (
                    <Button
                      key={format}
                      variant="default"
                      className="min-h-8 flex-1 px-2 py-1 text-[10px]"
                      onClick={() => void exportPowerCaptureToFile(selectedCapture.capture, format, undefined, { fileName: `test-${selectedCapture.stepId}-power.${format}` })
                        .then(() => setCaptureExportError(null))
                        .catch((reason) => setCaptureExportError(reason instanceof Error ? reason.message : String(reason)))}
                    >
                      <Download size={11} />{format.toUpperCase()}
                    </Button>
                  ))}
                </div>
              )}
              <div className="grid grid-cols-3 gap-2">
                <Button variant="default" onClick={handleExportCsv} className="min-h-8 px-2 py-1 text-[10px]" data-testid="report-export-csv"><FileSpreadsheet size={11} />CSV</Button>
                <Button variant="default" onClick={handleExportNdjson} className="min-h-8 px-2 py-1 text-[10px]" data-testid="report-export-ndjson"><FileText size={11} />NDJSON</Button>
                <Button variant="primary" onClick={handleExportJson} className="min-h-8 px-2 py-1 text-[10px]" data-testid="report-export-json"><FileJson size={11} />JSON</Button>
              </div>
              {captureExportError && <div className="mt-2 text-[9px] text-danger">{captureExportError}</div>}
            </section>
          </div>
        </aside>
      </div>
    </div>
  );
}
