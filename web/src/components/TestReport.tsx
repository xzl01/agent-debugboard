import { useRef, useState } from "react";
import {
  Activity,
  CheckCircle,
  Clock,
  Download,
  FileJson,
  FileSpreadsheet,
  FileText,
  RotateCcw,
  Terminal as TerminalIcon,
  XCircle,
} from "lucide-react";
import { Badge, Button } from "./ui";
import type {
  TestScript,
  RunSummary,
  StepResult,
  SerialLogEntry,
  AdcSampleEntry,
  PowerCaptureEvidenceEntry,
} from "@/lib/testScript";
import { isRunSuccessful } from "@/lib/testScript";
import { downloadBlob, formatMs } from "@/lib/utils";
import { useI18n } from "@/lib/i18n";
import { buildPowerChartAxis, formatPowerChartTick } from "./PowerChartAxis";
import { exportPowerCaptureToFile } from "@/lib/powerCaptureExport";

function TimelineBar({
  results,
  totalDuration,
}: {
  results: StepResult[];
  totalDuration: number;
}) {
  const { t } = useI18n();
  if (totalDuration <= 0 || results.length === 0) return null;

  const statusColor = (s: StepResult["status"]) => {
    if (s === "pass") return "#22c55e";
    if (s === "fail" || s === "error") return "#ef4444";
    if (s === "skip" || s === "aborted") return "#f59e0b";
    return "#64748b";
  };

  return (
    <div className="rounded-xl border border-line/50 bg-panel2/30 p-3">
      <div className="mb-2 text-[10px] font-semibold uppercase tracking-wider text-ink-dim">
        <Clock size={10} className="mr-1 inline" />
        {t("test.report.timeline")}
      </div>
      <div className="relative h-8 w-full overflow-hidden rounded-lg bg-panel">
        {results.map((r) => {
          const left = ((r.startedAtMs - results[0].startedAtMs) / totalDuration) * 100;
          const width = Math.max(1, (r.durationMs / totalDuration) * 100);
          return (
            <div
              key={r.stepId}
              className="absolute top-0 h-full opacity-80 transition-opacity hover:opacity-100"
              style={{
                left: `${left}%`,
                width: `${width}%`,
                backgroundColor: statusColor(r.status),
              }}
              title={`${r.stepId}: ${r.status} (${formatMs(r.durationMs)})`}
            />
          );
        })}
      </div>
      <div className="mt-1 flex justify-between text-[9px] text-ink-dim">
        <span>0s</span>
        <span>{formatMs(totalDuration)}</span>
      </div>
    </div>
  );
}

function PowerChart({
  samples,
  results,
  startedAtMs,
  preview = false,
}: {
  samples: AdcSampleEntry[];
  results: StepResult[];
  startedAtMs: number;
  preview?: boolean;
}) {
  const { t } = useI18n();
  if (samples.length < 2) return null;

  const channelSamples = new Map<string, AdcSampleEntry[]>();
  for (const sample of samples) {
    const entries = channelSamples.get(sample.channel) ?? [];
    entries.push(sample);
    channelSamples.set(sample.channel, entries);
  }
  const series = Array.from(channelSamples, ([channel, entries]) => ({
    channel,
    points: entries
      .map((sample) => ({
        x: sample.timestampMs - startedAtMs,
        y: sample.currentUa / 1_000_000,
      }))
      .sort((a, b) => a.x - b.x),
  }));
  const allPoints = series.flatMap((entry) => entry.points);
  const chartAxis = buildPowerChartAxis(
    "current",
    allPoints.reduce((max, point) => Math.max(max, point.y), 0.001),
  );
  const maxY = chartAxis.maximum;
  const maxX = allPoints.reduce((max, point) => Math.max(max, point.x), 0);
  if (maxX <= 0) return null;

  const W = 600;
  const H = 120;
  const pad = { top: 10, right: 10, bottom: 20, left: 40 };
  const plotW = W - pad.left - pad.right;
  const plotH = H - pad.top - pad.bottom;

  const sx = (x: number) => pad.left + (x / maxX) * plotW;
  const sy = (y: number) => pad.top + plotH - (y / maxY) * plotH;

  const colors = ["#4f7cff", "#f59e0b", "#22c55e", "#a855f7"];

  return (
    <div className="rounded-xl border border-line/50 bg-panel2/30 p-3">
      <div className="mb-2 text-[10px] font-semibold uppercase tracking-wider text-ink-dim">
        <Activity size={10} className="mr-1 inline" />
        {t("test.report.powerChart")}
        {preview && <span className="ml-1 font-normal normal-case">· {t("test.report.preview")}</span>}
      </div>
      <svg
        viewBox={`0 0 ${W} ${H}`}
        className="w-full"
        style={{ maxHeight: 150 }}
        role="img"
        aria-label={`${t("test.report.powerChart")} (${chartAxis.unit})`}
      >
        <title>{`${t("test.report.powerChart")} (${chartAxis.unit})`}</title>

        {results.map((r) => {
          const x1 = sx(r.startedAtMs - startedAtMs);
          const x2 = sx(r.finishedAtMs - startedAtMs);
          const color = r.status === "pass" ? "#22c55e" : r.status === "fail" || r.status === "error" ? "#ef4444" : "#f59e0b";
          return (
            <rect
              key={r.stepId}
              x={x1}
              y={pad.top}
              width={Math.max(1, x2 - x1)}
              height={plotH}
              fill={color}
              opacity={0.08}
            />
          );
        })}

        {chartAxis.ticks.map((tick, index) => {
          const y = sy(tick);
          return (
            <g key={tick}>
              <line
                x1={pad.left}
                y1={y}
                x2={pad.left + plotW}
                y2={y}
                stroke="currentColor"
                strokeDasharray={index === chartAxis.ticks.length - 1 ? undefined : "3 5"}
                strokeOpacity={index === chartAxis.ticks.length - 1 ? 0.16 : 0.1}
              />
              <text
                x={pad.left - 4}
                y={y + 3}
                fill="currentColor"
                fontSize="8"
                opacity="0.55"
                textAnchor="end"
              >
                {formatPowerChartTick(tick, chartAxis.multiplier)}{index === 0 ? ` ${chartAxis.unit}` : ""}
              </text>
            </g>
          );
        })}
        <line x1={pad.left} y1={pad.top} x2={pad.left} y2={pad.top + plotH} stroke="currentColor" strokeOpacity={0.16} />

        {series.map((entry, index) => (
          <path
            key={entry.channel}
            d={entry.points.map((point, pointIndex) => (
              `${pointIndex === 0 ? "M" : "L"}${sx(point.x).toFixed(1)},${sy(point.y).toFixed(1)}`
            )).join(" ")}
            fill="none"
            stroke={colors[index % colors.length]}
            strokeWidth="1.5"
          />
        ))}

        <text x={pad.left} y={H - 2} fill="currentColor" fontSize="8" opacity={0.4}>0s</text>
        <text x={W - pad.right} y={H - 2} fill="currentColor" fontSize="8" opacity={0.4} textAnchor="end">
          {formatMs(maxX)}
        </text>
      </svg>
      <div className="mt-1 flex flex-wrap gap-x-3 gap-y-1">
        {series.map((entry, index) => (
          <span key={entry.channel} className="inline-flex items-center gap-1 text-[9px] text-ink-dim">
            <span className="h-1.5 w-3 rounded-full" style={{ backgroundColor: colors[index % colors.length] }} />
            {entry.channel.replace(/_out$/, "").toUpperCase()}
          </span>
        ))}
      </div>
    </div>
  );
}

function ResultsTable({ results, startedAtMs }: { results: StepResult[]; startedAtMs: number }) {
  const { t } = useI18n();
  const statusBadge = (s: StepResult["status"]) => {
    if (s === "pass") return <Badge tone="ok">{t("test.status.pass")}</Badge>;
    if (s === "fail") return <Badge tone="danger">{t("test.status.fail")}</Badge>;
    if (s === "skip") return <Badge tone="warn">{t("test.status.skip")}</Badge>;
    if (s === "aborted") return <Badge tone="warn">{t("test.status.aborted")}</Badge>;
    if (s === "error") return <Badge tone="danger">{t("test.status.error")}</Badge>;
    return <Badge tone="neutral">{s}</Badge>;
  };
  const unitMap = new Map<string, { name: string; results: StepResult[] }>();
  for (const result of results) {
    if (!result.unitId || !result.unitName) continue;
    const entry = unitMap.get(result.unitId) ?? { name: result.unitName, results: [] };
    entry.results.push(result);
    unitMap.set(result.unitId, entry);
  }
  const unitSummaries = Array.from(unitMap, ([id, unit]) => {
    const status: StepResult["status"] = unit.results.some((result) => result.status === "error")
      ? "error"
      : unit.results.some((result) => result.status === "fail")
        ? "fail"
        : unit.results.some((result) => result.status === "aborted")
          ? "aborted"
          : unit.results.some((result) => result.status === "skip")
            ? "skip"
            : "pass";
    return {
      id,
      name: unit.name,
      status,
      durationMs: unit.results.reduce((total, result) => total + result.durationMs, 0),
      stepCount: unit.results.length,
    };
  });

  return (
    <div className="space-y-2">
      {unitSummaries.length > 0 && (
        <div className="rounded-xl border border-line/50 bg-panel2/30 p-3">
          <div className="mb-2 text-[10px] font-semibold uppercase tracking-wider text-ink-dim">
            {t("test.report.unitResults")}
          </div>
          <div className="grid gap-2 sm:grid-cols-2">
            {unitSummaries.map((unit) => (
              <div key={unit.id} className="flex items-center gap-2 rounded-lg border border-line/40 bg-panel px-2.5 py-2">
                <span className="min-w-0 flex-1 truncate text-xs font-semibold text-ink">{unit.name}</span>
                <span className="text-[10px] text-ink-dim">
                  {t("test.report.unitSteps", { n: unit.stepCount })} · {formatMs(unit.durationMs)}
                </span>
                {statusBadge(unit.status)}
              </div>
            ))}
          </div>
        </div>
      )}
      <div className="rounded-xl border border-line/50 bg-panel2/30">
      <table className="w-full text-xs">
        <thead>
          <tr className="border-b border-line/40 text-[10px] uppercase tracking-wider text-ink-dim">
            <th className="px-3 py-2 text-left">#</th>
            <th className="px-3 py-2 text-left">{t("test.report.colType")}</th>
            <th className="px-3 py-2 text-left">{t("test.report.colStatus")}</th>
            <th className="px-3 py-2 text-right">{t("test.report.colDuration")}</th>
            <th className="px-3 py-2 text-left">{t("test.report.colDetail")}</th>
          </tr>
        </thead>
        <tbody>
          {results.map((r, i) => (
            <tr key={r.stepId} className="border-b border-line/20 last:border-0">
              <td className="px-3 py-1.5 text-ink-dim">{i + 1}</td>
              <td className="px-3 py-1.5 font-medium text-ink">
                {t(`test.step.${r.stepType}`)}
                {r.unitName && (
                  <div className="mt-0.5 text-[9px] font-normal text-brand">{r.unitName}</div>
                )}
                {!r.unitName && r.loopIteration != null && r.loopCount != null && (
                  <div className="mt-0.5 text-[9px] font-normal text-brand">
                    {t("test.loop.iteration", { current: r.loopIteration, total: r.loopCount })}
                  </div>
                )}
                {r.conditionRole && (
                  <div className="mt-0.5 text-[9px] font-normal text-violet-500">
                    {t(`test.condition.branch.${r.conditionRole}`)}
                  </div>
                )}
              </td>
              <td className="px-3 py-1.5">{statusBadge(r.status)}</td>
              <td className="px-3 py-1.5 text-right font-mono text-ink-dim">{formatMs(r.durationMs)}</td>
              <td className="max-w-[300px] truncate px-3 py-1.5 text-ink-dim">
                {r.conditionOutcome != null
                  ? r.conditionOutcome ? t("test.condition.outcomeTrue") : t("test.condition.outcomeFalse")
                  : r.conditionalSkip
                    ? t("test.condition.notSelected")
                    : r.error ?? r.assertionResult?.detail ?? (r.adcValueUa != null ? `${(r.adcValueUa / 1_000_000).toFixed(3)}A` : "—")}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      </div>
    </div>
  );
}

function SerialLogPanel({ logs, startedAtMs }: { logs: SerialLogEntry[]; startedAtMs: number }) {
  const ref = useRef<HTMLDivElement>(null);
  const { t } = useI18n();

  if (logs.length === 0) return null;

  return (
    <div className="rounded-xl border border-line/50 bg-panel2/30">
      <div className="flex items-center gap-2 border-b border-line/40 px-3 py-1.5">
        <TerminalIcon size={12} className="text-ink-dim" />
        <span className="text-[10px] font-semibold uppercase tracking-wider text-ink-dim">
          {t("test.report.serialLog")}
        </span>
        <span className="text-[10px] text-ink-dim">{t("test.report.entries", { n: logs.length })}</span>
      </div>
      <div ref={ref} className="max-h-60 overflow-y-auto p-2 font-mono text-[10px] leading-relaxed text-ink-dim">
        {logs.map((log, i) => (
          <div key={i} className={log.direction === "tx" ? "text-brand" : ""}>
            <span className="text-ink-dim/50">[{((log.timestampMs - startedAtMs) / 1000).toFixed(1)}s]</span>
            <span className="ml-1 text-ink-dim/70">[{log.channel.toUpperCase()}]</span>
            {log.direction === "tx" ? " → " : " "}
            {log.text.replace(/[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]/g, "").slice(0, 500)}
          </div>
        ))}
      </div>
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
  const passed = isRunSuccessful(summary);
  const [captureExportError, setCaptureExportError] = useState<string | null>(null);

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
    downloadBlob(
      `report-${script.name.replace(/\s+/g, "-")}.json`,
      JSON.stringify(report, null, 2),
      "application/json",
    );
  };

  const handleExportCsv = () => {
    const header = "step_id,status,duration_ms,error,detail,adc_ua,unit_id,unit_name,condition_id,condition_role,condition_outcome,conditional_skip";
    const rows = summary.results.map((r) =>
      [
        r.stepId,
        r.status,
        r.durationMs,
        r.error ?? "",
        r.assertionResult?.detail ?? "",
        r.adcValueUa ?? "",
        r.unitId ?? "",
        r.unitName ?? "",
        r.conditionId ?? "",
        r.conditionRole ?? "",
        r.conditionOutcome ?? "",
        r.conditionalSkip ?? false,
      ]
        .map((v) => (/[",\n]/.test(String(v)) ? `"${String(v).replaceAll('"', '""')}"` : v))
        .join(","),
    );
    downloadBlob(
      `report-${script.name.replace(/\s+/g, "-")}.csv`,
      [header, ...rows].join("\n") + "\n",
      "text/csv",
    );
  };

  const handleExportNdjson = () => {
    const lines = summary.results.map((r) => JSON.stringify({ type: "step_result", ...r }));
    downloadBlob(
      `report-${script.name.replace(/\s+/g, "-")}.ndjson`,
      lines.join("\n") + "\n",
      "application/x-ndjson",
    );
  };

  return (
    <div className="space-y-3">
      <div className="flex items-center gap-3">
        {passed ? (
          <CheckCircle size={20} className="text-ok" />
        ) : (
          <XCircle size={20} className="text-danger" />
        )}
        <div>
          <div className="text-sm font-semibold text-ink">
            {passed ? t("test.status.pass") : t("test.status.fail")}
          </div>
          <div className="text-[11px] text-ink-dim">
            {formatMs(summary.durationMs)} · {t("test.report.passedCount", { n: summary.passed, total: summary.totalSteps })}
          </div>
        </div>
        <span className="flex-1" />
        <Button variant="ghost" onClick={handleExportJson}>
          <FileJson size={14} />
          {t("test.report.exportJson")}
        </Button>
        <Button variant="ghost" onClick={handleExportCsv}>
          <FileSpreadsheet size={14} />
          {t("test.report.exportCsv")}
        </Button>
        <Button variant="ghost" onClick={handleExportNdjson}>
          <FileText size={14} />
          {t("test.report.exportNdjson")}
        </Button>
        <Button variant="primary" onClick={onReRun}>
          <RotateCcw size={14} />
          {t("test.report.rerun")}
        </Button>
      </div>

      <TimelineBar results={summary.results} totalDuration={summary.durationMs} />
      {(summary.infrastructureError || summary.cleanup) && (
        <div className={`rounded-xl border px-3 py-2 text-xs ${
          summary.cleanup?.passed !== false && !summary.infrastructureError
            ? "border-ok/30 bg-ok/10 text-ok"
            : "border-danger/30 bg-danger/10 text-danger"
        }`}>
          <div className="font-semibold">
            {summary.cleanup?.passed !== false && !summary.infrastructureError
              ? t("test.report.cleanupPassed")
              : t("test.report.cleanupFailed")}
          </div>
          {summary.infrastructureError && <div className="mt-1 break-words">{summary.infrastructureError}</div>}
        </div>
      )}
      {powerCaptures.length > 0 && (
        <div className="rounded-xl border border-line/50 bg-panel2/30 p-3">
          <div className="mb-2 flex items-center gap-2 text-[10px] font-semibold uppercase tracking-wider text-ink-dim">
            <Download size={10} />
            {t("test.report.powerEvidence")}
          </div>
          <div className="space-y-2">
            {powerCaptures.map(({ stepId, rail, capture }) => (
              <div key={`${stepId}-${capture.id}`} className="flex flex-wrap items-center gap-2 rounded-lg border border-line/60 bg-panel px-2.5 py-2 text-[10px]">
                <span className="font-mono text-ink">{stepId}</span>
                <span className="text-ink-dim">{rail} · {(capture.sampleCount ?? capture.samples.length).toLocaleString()} {t("test.report.samples")}</span>
                {(capture.incomplete || (capture.droppedSamples ?? 0) > 0) && (
                  <Badge tone="danger">{t("test.report.incomplete")}</Badge>
                )}
                <span className="flex-1" />
                {(["csv", "ndjson"] as const).map((format) => (
                  <Button
                    key={format}
                    variant="ghost"
                    className="min-h-7 px-2 py-1 text-[10px]"
                    onClick={() => void exportPowerCaptureToFile(capture, format, undefined, {
                      fileName: `test-${stepId}-power.${format}`,
                    }).then(() => setCaptureExportError(null)).catch((reason) => {
                      setCaptureExportError(reason instanceof Error ? reason.message : String(reason));
                    })}
                  >
                    <Download size={11} />{format.toUpperCase()}
                  </Button>
                ))}
              </div>
            ))}
          </div>
          {captureExportError && <div className="mt-2 text-[10px] text-danger">{captureExportError}</div>}
        </div>
      )}
      <PowerChart samples={adcSamples} results={summary.results} startedAtMs={summary.startedAtMs} preview={powerCaptures.length > 0} />
      <ResultsTable results={summary.results} startedAtMs={summary.startedAtMs} />
      <SerialLogPanel logs={serialLogs} startedAtMs={summary.startedAtMs} />
    </div>
  );
}
