import { useEffect, useMemo, useRef } from "react";
import {
  Activity,
  CheckCircle2,
  Circle,
  CircleDot,
  GitBranch,
  Hourglass,
  LockKeyhole,
  MessageSquareText,
  Power,
  Radio,
  Send,
  ShieldCheck,
  Square,
  Terminal as TerminalIcon,
  Timer,
  Zap,
} from "lucide-react";
import { Badge, Button } from "./ui";
import type {
  AdcSampleEntry,
  ExecutionStep,
  SerialLogEntry,
  StepResult,
  StepStatus,
  StepType,
} from "@/lib/testScript";
import { formatMs } from "@/lib/utils";
import { useI18n } from "@/lib/i18n";

export type ExecutionGroupKind = "step" | "loop" | "unit" | "condition";

export interface ExecutionGroup {
  id: string;
  kind: ExecutionGroupKind;
  steps: ExecutionStep[];
  loopCount?: number;
  unitName?: string;
}

/** Reconstruct the editor hierarchy from the expanded, executable plan. */
export function buildExecutionGroups(steps: ExecutionStep[]): ExecutionGroup[] {
  const groups: ExecutionGroup[] = [];
  const groupById = new Map<string, ExecutionGroup>();

  for (const step of steps) {
    const kind: ExecutionGroupKind = step.conditionId
      ? "condition"
      : step.loopId
        ? step.unitName && (step.loopCount ?? 1) === 1
          ? "unit"
          : "loop"
        : "step";
    const identity = step.conditionId ?? step.loopId ?? step.executionId;
    const id = `${kind}:${identity}`;
    let group = groupById.get(id);
    if (!group) {
      group = {
        id,
        kind,
        steps: [],
        loopCount: step.loopCount,
        unitName: step.unitName,
      };
      groupById.set(id, group);
      groups.push(group);
    }
    group.steps.push(step);
  }

  return groups;
}

const STATUS_PRIORITY: StepStatus[] = ["running", "error", "fail", "aborted", "pending", "skip", "pass"];

export function executionGroupStatus(
  group: ExecutionGroup,
  states: Map<string, StepStatus>,
  resultMap: Map<string, StepResult>,
): StepStatus {
  const statuses = group.steps.flatMap((step) => {
    const result = resultMap.get(step.executionId);
    if (result?.conditionalSkip) return [];
    return [result?.status ?? states.get(step.executionId) ?? "pending"];
  });
  if (statuses.length === 0 && group.steps.some((step) => resultMap.get(step.executionId)?.conditionalSkip)) {
    return "pass";
  }
  return STATUS_PRIORITY.find((status) => statuses.includes(status)) ?? "pending";
}

function activeIteration(group: ExecutionGroup, states: Map<string, StepStatus>): number | undefined {
  if (group.kind !== "loop" && group.kind !== "unit") return undefined;
  const running = group.steps.find((step) => states.get(step.executionId) === "running");
  if (running?.loopIteration != null) return running.loopIteration;
  const completed = group.steps.filter((step) => {
    const status = states.get(step.executionId);
    return status != null && status !== "pending";
  });
  return completed.at(-1)?.loopIteration ?? group.steps[0]?.loopIteration ?? 1;
}

export function visibleGroupSteps(group: ExecutionGroup, states: Map<string, StepStatus>): ExecutionStep[] {
  const iteration = activeIteration(group, states);
  if (group.kind === "loop" && iteration != null) {
    return group.steps.filter((step) => step.loopIteration === iteration);
  }
  return group.steps;
}

const STEP_ICONS: Record<StepType, typeof Activity> = {
  power_on: Zap,
  power_off: Power,
  delay: Timer,
  serial_wait: Hourglass,
  serial_send: Send,
  serial_expect: MessageSquareText,
  adc_read: Activity,
  gpio_set: CircleDot,
  gpio_assert: CheckCircle2,
  switch_route: GitBranch,
  capture: Activity,
};

function statusTone(status: StepStatus): "neutral" | "brand" | "ok" | "danger" {
  if (status === "running") return "brand";
  if (status === "pass") return "ok";
  if (status === "fail" || status === "error") return "danger";
  return "neutral";
}

function statusDot(status: StepStatus): string {
  if (status === "running") return "bg-brand";
  if (status === "pass") return "bg-ok";
  if (status === "fail" || status === "error") return "bg-danger";
  return "border border-line bg-panel";
}

function statusLabel(status: StepStatus, t: ReturnType<typeof useI18n>["t"]): string {
  if (status === "pending") return "—";
  if (status === "running") return t("test.running");
  return t(`test.status.${status}`);
}

function elapsedStr(startMs: number): string {
  const s = Math.max(0, Math.floor((Date.now() - startMs) / 1000));
  const m = Math.floor(s / 60);
  return `${String(m).padStart(2, "0")}:${String(s % 60).padStart(2, "0")}`;
}

function stepDetail(step: ExecutionStep): string {
  const params = step.params as unknown as Record<string, unknown>;
  switch (step.type) {
    case "power_on":
    case "power_off":
      return String(params.rail ?? "—");
    case "delay":
      return formatMs(Number(params.ms ?? 0));
    case "serial_wait":
      return `${String(params.channel ?? "").toUpperCase()} · ${String(params.pattern ?? "")}`;
    case "serial_send":
      return `${String(params.channel ?? "").toUpperCase()} · ${String(params.text ?? "")}`;
    case "serial_expect":
      return `${String(params.channel ?? "").toUpperCase()} · ${String(params.command ?? "")}`;
    case "adc_read":
      return String(params.channel ?? "—");
    case "gpio_set":
    case "gpio_assert":
      return `${String(params.pin ?? "—")} · ${String(params.value ?? "—")}`;
    case "switch_route":
      return `${String(params.switch ?? "—")} → ${String(params.route ?? "—")}`;
    case "capture":
      return `${String(params.rail ?? "—")} · ${formatMs(Number(params.duration_ms ?? 0))}`;
  }
}

function resultDetail(step: ExecutionStep, result: StepResult | undefined): string {
  if (!result) return stepDetail(step);
  if (result.error && !result.conditionalSkip) return result.error;
  if (result.adcValueUa != null) return `${(result.adcValueUa / 1000).toFixed(1)} mA`;
  if (result.assertionResult?.detail) return result.assertionResult.detail;
  if (result.conditionOutcome != null) return result.conditionOutcome ? "TRUE" : "FALSE";
  return stepDetail(step);
}

function StepRow({
  step,
  index,
  status,
  result,
  nested = false,
}: {
  step: ExecutionStep;
  index: string;
  status: StepStatus;
  result?: StepResult;
  nested?: boolean;
}) {
  const { t } = useI18n();
  const Icon = STEP_ICONS[step.type];
  const isCurrent = status === "running";
  const failed = status === "fail" || status === "error";

  return (
    <div
      data-testid={`runner-step-${step.executionId}`}
      data-status={status}
      className={`flex min-w-0 items-center gap-2 rounded-xl border px-2.5 py-2 transition-colors ${
        isCurrent
          ? "border-brand/60 bg-brand/5"
          : failed
            ? "border-danger/50 bg-danger/5"
            : "border-line/60 bg-panel"
      } ${nested ? "ml-5" : ""}`}
    >
      <span className={`grid h-7 w-10 shrink-0 place-items-center rounded-lg font-mono text-[10px] font-semibold ${
        isCurrent ? "bg-brand text-on-brand" : "bg-panel2 text-ink-dim"
      }`}>
        {index}
      </span>
      <Icon size={14} className={isCurrent ? "text-brand" : "text-ink-dim"} />
      <div className="min-w-0 flex-1">
        <div className="flex min-w-0 items-center gap-2">
          <span className="truncate text-xs font-semibold text-ink">{t(`test.step.${step.type}`)}</span>
          {step.unitName && (
            <span className="shrink-0 rounded-full border border-line/70 px-2 py-0.5 text-[9px] font-medium text-ink-dim">
              {step.unitName}
            </span>
          )}
          {step.conditionRole && (
            <span className="shrink-0 rounded-full bg-violet-500/10 px-2 py-0.5 text-[9px] font-medium text-violet-600 dark:text-violet-300">
              {t(`test.condition.branch.${step.conditionRole}`)}
            </span>
          )}
        </div>
        <div className={`truncate font-mono text-[9px] ${failed ? "text-danger" : "text-ink-dim"}`}>
          {resultDetail(step, result)}
        </div>
      </div>
      {result && <span className="shrink-0 font-mono text-[9px] text-ink-dim">{formatMs(result.durationMs)}</span>}
      <Badge tone={statusTone(status)} className="shrink-0">
        {status === "pending" ? <Circle size={8} /> : <span className={`h-1.5 w-1.5 rounded-full ${statusDot(status)}`} />}
        {statusLabel(status, t)}
      </Badge>
    </div>
  );
}

function GroupTimeline({
  group,
  groupIndex,
  states,
  resultMap,
}: {
  group: ExecutionGroup;
  groupIndex: number;
  states: Map<string, StepStatus>;
  resultMap: Map<string, StepResult>;
}) {
  const { t } = useI18n();
  const status = executionGroupStatus(group, states, resultMap);
  const iteration = activeIteration(group, states);
  const children = visibleGroupSteps(group, states);

  if (group.kind === "step") {
    const step = group.steps[0];
    return (
      <StepRow
        step={step}
        index={String(groupIndex + 1).padStart(2, "0")}
        status={states.get(step.executionId) ?? "pending"}
        result={resultMap.get(step.executionId)}
      />
    );
  }

  const isLoop = group.kind === "loop" || group.kind === "unit";
  const title = group.kind === "condition"
    ? t("test.condition.title")
    : group.kind === "unit"
      ? group.unitName ?? t("test.loop.title")
      : t("test.loop.title");

  return (
    <section
      data-testid={`runner-group-${group.id}`}
      data-group-kind={group.kind}
      className={`rounded-xl border bg-panel p-2.5 ${status === "running" ? "border-brand/60" : "border-line/70"}`}
    >
      <div className="mb-2 flex items-center gap-2">
        <span className="grid h-7 w-10 shrink-0 place-items-center rounded-lg bg-violet-500/10 font-mono text-[10px] font-semibold text-violet-600 dark:text-violet-300">
          {String(groupIndex + 1).padStart(2, "0")}
        </span>
        {group.kind === "condition" ? (
          <GitBranch size={14} className="text-violet-500" />
        ) : (
          <Radio size={14} className="text-violet-500" />
        )}
        <div className="min-w-0 flex-1">
          <div className="text-xs font-semibold text-ink">{title}</div>
          <div className="font-mono text-[9px] text-ink-dim">
            {isLoop && group.loopCount != null
              ? `${iteration ?? 1} / ${group.loopCount} · ${children.length} ${t("test.report.unitSteps", { n: children.length }).replace(String(children.length), "").trim()}`
              : t("test.condition.summary", {
                  then: group.steps.filter((step) => step.conditionRole === "then").length,
                  else: group.steps.filter((step) => step.conditionRole === "else").length,
                })}
          </div>
        </div>
        <Badge tone={statusTone(status)}>{statusLabel(status, t)}</Badge>
      </div>
      <div className="relative space-y-1.5 before:absolute before:bottom-3 before:left-[11px] before:top-3 before:w-px before:bg-line">
        {children.map((step, index) => (
          <StepRow
            key={step.executionId}
            step={step}
            index={`${String(groupIndex + 1).padStart(2, "0")}.${index + 1}`}
            status={states.get(step.executionId) ?? "pending"}
            result={resultMap.get(step.executionId)}
            nested
          />
        ))}
      </div>
    </section>
  );
}

export interface TestRunnerViewProps {
  steps: ExecutionStep[];
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
      if (timerRef.current) timerRef.current.textContent = elapsedStr(startedAtMs);
    }, 1000);
    return () => window.clearInterval(id);
  }, [startedAtMs]);

  useEffect(() => {
    if (serialRef.current) serialRef.current.scrollTop = serialRef.current.scrollHeight;
  }, [serialLogs.length]);

  const resultMap = useMemo(() => new Map(stepResults.map((result) => [result.stepId, result])), [stepResults]);
  const groups = useMemo(() => buildExecutionGroups(steps), [steps]);
  const runningIndex = steps.findIndex((step) => stepStates.get(step.executionId) === "running");
  const currentStep = runningIndex >= 0 ? steps[runningIndex] : undefined;
  const completed = steps.filter((step) => {
    const status = stepStates.get(step.executionId);
    return status != null && status !== "pending" && status !== "running";
  }).length;
  const progress = steps.length > 0 ? ((completed + (runningIndex >= 0 ? 0.35 : 0)) / steps.length) * 100 : 0;
  const currentAdcChannel = currentStep?.type === "adc_read"
    ? (currentStep.params as unknown as { channel?: string }).channel
    : undefined;
  const currentStepSamples = currentStep
    ? adcSamples.filter((sample) => (
        sample.stepId === currentStep.executionId
        && (currentAdcChannel == null || sample.channel === currentAdcChannel)
      ))
    : [];
  const currentAdc = currentStepSamples.at(-1)?.currentUa;
  const peakAdc = currentStepSamples.length > 0
    ? Math.max(...currentStepSamples.map((sample) => sample.currentUa))
    : undefined;
  const serialChannels = Array.from(new Set(steps.flatMap((step) => {
    if (!step.type.startsWith("serial_")) return [];
    const channel = (step.params as unknown as { channel?: string }).channel;
    return channel ? [channel.toUpperCase()] : [];
  })));
  const rails = Array.from(new Set(steps.flatMap((step) => {
    const rail = (step.params as unknown as { rail?: string; channel?: string }).rail
      ?? (["power_on", "power_off", "adc_read", "capture"].includes(step.type)
        ? (step.params as unknown as { channel?: string }).channel
        : undefined);
    return rail ? [rail] : [];
  })));

  return (
    <div data-testid="automation-running-workspace" className="flex h-full min-h-[680px] flex-col gap-3">
      <header className="flex shrink-0 flex-wrap items-center gap-3 rounded-2xl border border-line/70 bg-panel px-4 py-3">
        <div className="min-w-[180px]">
          <div className="text-sm font-semibold text-ink">
            {currentStep ? t(`test.step.${currentStep.type}`) : t("test.running")}
          </div>
          <div className="font-mono text-[10px] text-ink-dim">
            {t("test.running.step", { current: Math.max(1, runningIndex + 1), total: steps.length })}
          </div>
        </div>
        <div className="min-w-[220px] flex-1">
          <div className="mb-1 flex items-center justify-between text-[10px] text-ink-dim">
            <span>{Math.round(progress)}%</span>
            <span ref={timerRef} className="font-mono">{elapsedStr(startedAtMs)}</span>
          </div>
          <div className="h-2 overflow-hidden rounded-full bg-panel2">
            <div className="h-full rounded-full bg-brand transition-[width] duration-200" style={{ width: `${Math.min(100, progress)}%` }} />
          </div>
        </div>
        <Button variant="danger" onClick={onAbort} className="min-h-9 px-4 py-1.5 text-xs">
          <Square size={13} />
          {t("test.abort")}
        </Button>
      </header>

      <div className="grid min-h-0 flex-1 gap-3 xl:grid-cols-[238px_minmax(420px,1fr)_344px]">
        <aside className="min-h-0 overflow-hidden rounded-2xl border border-line/70 bg-panel" data-testid="runner-step-navigation">
          <div className="border-b border-line/60 px-3.5 py-3">
            <div className="text-sm font-semibold text-ink">{t("test.report.stepResults")}</div>
            <div className="font-mono text-[9px] text-ink-dim">{completed} / {steps.length}</div>
          </div>
          <div className="max-h-[calc(100%-154px)] space-y-1 overflow-y-auto p-2">
            {groups.map((group, groupIndex) => {
              const status = executionGroupStatus(group, stepStates, resultMap);
              const groupIteration = activeIteration(group, stepStates);
              const label = group.kind === "condition"
                ? t("test.condition.title")
                : group.kind === "loop"
                  ? t("test.loop.title")
                  : group.kind === "unit"
                    ? group.unitName ?? t("test.loop.title")
                    : t(`test.step.${group.steps[0].type}`);
              return (
                <div key={group.id}>
                  <div className={`flex items-center gap-2 rounded-lg px-2 py-2 ${status === "running" ? "bg-brand/5" : ""}`}>
                    <span className={`grid h-6 w-8 place-items-center rounded-md font-mono text-[9px] ${status === "running" ? "bg-brand text-on-brand" : "bg-panel2 text-ink-dim"}`}>
                      {String(groupIndex + 1).padStart(2, "0")}
                    </span>
                    <span className="min-w-0 flex-1 truncate text-[11px] font-medium text-ink">{label}</span>
                    {group.kind === "loop" && (
                      <span className="font-mono text-[9px] text-violet-600 dark:text-violet-300">{groupIteration}/{group.loopCount}</span>
                    )}
                    <span className={`h-2 w-2 shrink-0 rounded-full ${statusDot(status)}`} />
                  </div>
                  {status === "running" && group.kind !== "step" && visibleGroupSteps(group, stepStates).map((step, childIndex) => (
                    <div key={step.executionId} className={`ml-5 flex items-center gap-2 rounded-lg px-2 py-1.5 ${stepStates.get(step.executionId) === "running" ? "border border-brand/50 bg-brand/5" : ""}`}>
                      <span className="w-8 font-mono text-[9px] text-ink-dim">{groupIndex + 1}.{childIndex + 1}</span>
                      <span className="min-w-0 flex-1 truncate text-[10px] text-ink">
                        {step.unitName ? `${step.unitName} · ` : ""}{t(`test.step.${step.type}`)}
                      </span>
                      <span className={`h-1.5 w-1.5 rounded-full ${statusDot(stepStates.get(step.executionId) ?? "pending")}`} />
                    </div>
                  ))}
                </div>
              );
            })}
          </div>
          <div className="m-2 mt-3 rounded-xl bg-panel2/70 p-3 text-[10px] text-ink-dim">
            <div className="mb-2 flex items-center gap-2 font-semibold text-ink">
              <LockKeyhole size={12} className="text-warn" />
              {t("test.running")}
            </div>
            <div className="font-mono">{[...serialChannels, ...rails].join(" · ") || "—"}</div>
          </div>
        </aside>

        <main className="min-h-0 overflow-hidden rounded-2xl border border-line/70 bg-panel" data-testid="runner-timeline">
          <div className="flex items-center justify-between border-b border-line/60 px-4 py-3">
            <div>
              <div className="text-sm font-semibold text-ink">{t("test.report.timeline")}</div>
              <div className="text-[10px] text-ink-dim">{t("test.report.stepResults")}</div>
            </div>
            {currentStep && <Badge tone="brand">{currentStep.executionId}</Badge>}
          </div>
          <div className="relative max-h-[calc(100%-61px)] space-y-2 overflow-y-auto p-3 before:absolute before:bottom-6 before:left-[25px] before:top-7 before:w-px before:bg-line/70">
            <div className="relative z-[1] flex items-center gap-3 rounded-xl border border-line/60 bg-panel px-3 py-2.5">
              <span className="grid h-7 w-7 place-items-center rounded-full border border-ok/50 bg-panel text-ok"><CircleDot size={14} /></span>
              <span className="text-xs font-semibold text-ink">Workflow</span>
              <span className="flex-1" />
              <Badge tone="ok">{t("test.running")}</Badge>
            </div>
            {groups.map((group, groupIndex) => (
              <div key={group.id} className="relative z-[1] pl-3">
                <GroupTimeline group={group} groupIndex={groupIndex} states={stepStates} resultMap={resultMap} />
              </div>
            ))}
            <div className="relative z-[1] flex items-center gap-3 rounded-xl border border-line/60 bg-panel px-3 py-2.5">
              <span className="grid h-7 w-7 place-items-center rounded-full border border-line bg-panel text-ink-dim"><ShieldCheck size={14} /></span>
              <span className="text-xs font-semibold text-ink">{t("test.running.cleanupPending")}</span>
              <span className="flex-1" />
              <Badge tone="neutral">{t("test.running.cleanupAlways")}</Badge>
            </div>
          </div>
        </main>

        <aside className="min-h-0 overflow-hidden rounded-2xl border border-line/70 bg-panel" data-testid="runner-evidence-inspector">
          <div className="flex items-start justify-between border-b border-line/60 px-3.5 py-3">
            <div>
              <div className="text-sm font-semibold text-ink">{t("test.report.serialLog")}</div>
              <div className="font-mono text-[9px] text-ink-dim">{currentStep?.executionId ?? "—"}</div>
            </div>
            <Badge tone="brand">{t("test.running")}</Badge>
          </div>

          <div className="max-h-[calc(100%-61px)] space-y-3 overflow-y-auto p-3">
            <section className="rounded-xl bg-brand/5 p-3">
              <div className="flex items-center gap-2 text-[11px] font-semibold text-brand">
                <Radio size={13} />
                {currentStep ? t(`test.step.${currentStep.type}`) : t("test.running")}
              </div>
              <div className="mt-1 truncate font-mono text-[9px] text-ink-dim">
                {currentStep ? resultDetail(currentStep, resultMap.get(currentStep.executionId)) : "—"}
              </div>
            </section>

            <section className="overflow-hidden rounded-xl bg-panel2/70">
              <div className="flex items-center gap-2 px-3 py-2 text-[10px] font-semibold text-ink-dim">
                <TerminalIcon size={12} />
                {serialChannels.join(" / ") || "UART"}
                <span className="flex-1" />
                {t("test.report.entries", { n: serialLogs.length })}
              </div>
              <div ref={serialRef} className="max-h-52 overflow-y-auto border-t border-line/40 p-3 font-mono text-[9px] leading-5 text-ink-dim">
                {serialLogs.length === 0 ? (
                  <div>—</div>
                ) : serialLogs.slice(-100).map((log, index) => (
                  <div key={`${log.timestampMs}-${index}`} className={log.direction === "tx" ? "text-brand" : "text-ink"}>
                    <span className="mr-1 text-ink-dim/70">[{((log.timestampMs - startedAtMs) / 1000).toFixed(1)}s]</span>
                    {log.direction === "tx" ? "→ " : ""}
                    {log.text.replace(/[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]/g, "").slice(0, 300)}
                  </div>
                ))}
              </div>
            </section>

            <section className="rounded-xl border border-line/60 p-3">
              <div className="mb-2 flex items-center gap-2 text-[11px] font-semibold text-ink">
                <Activity size={13} className="text-ink-dim" />
                {t("test.report.powerEvidence")}
              </div>
              <div className="grid grid-cols-3 gap-2">
                <div><div className="text-[9px] text-ink-dim">{t("test.report.overall")}</div><div className="font-mono text-[11px] font-semibold text-ink">{currentAdc == null ? "—" : `${(currentAdc / 1000).toFixed(1)} mA`}</div></div>
                <div><div className="text-[9px] text-ink-dim">Peak</div><div className="font-mono text-[11px] font-semibold text-ink">{peakAdc == null ? "—" : `${(peakAdc / 1000).toFixed(1)} mA`}</div></div>
                <div><div className="text-[9px] text-ink-dim">{t("test.report.samples")}</div><div className="font-mono text-[11px] font-semibold text-ink">{currentStepSamples.length}</div></div>
              </div>
            </section>

            <section className="border-t border-line/60 pt-3">
              <div className="mb-3 flex items-center gap-2 text-[11px] font-semibold text-ink">
                <ShieldCheck size={13} className="text-ink-dim" />
                {t("test.report.overall")}
                <Badge tone="warn" className="ml-auto">automation</Badge>
              </div>
              <dl className="space-y-2 text-[10px]">
                <div className="flex gap-2"><dt className="text-ink-dim">UART</dt><dd className="ml-auto text-right font-mono text-ink">{serialChannels.join(", ") || "—"}</dd></div>
                <div className="flex gap-2"><dt className="text-ink-dim">Power</dt><dd className="ml-auto text-right font-mono text-ink">{rails.join(", ") || "—"}</dd></div>
                <div className="flex gap-2"><dt className="text-ink-dim">Run</dt><dd className="ml-auto text-right text-warn"><LockKeyhole size={11} className="mr-1 inline" />{t("test.running")}</dd></div>
              </dl>
            </section>
          </div>
        </aside>
      </div>
    </div>
  );
}
