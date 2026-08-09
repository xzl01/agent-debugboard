import { useEffect, useMemo, useState, type ReactNode } from "react";
import {
  Activity,
  BatteryCharging,
  Clock3,
  Download,
  Gauge,
  HardDrive,
  History,
  MousePointerClick,
  Play,
  Power,
  Radio,
  Square,
  Trash2,
  Waypoints,
  Zap,
  type LucideIcon,
} from "lucide-react";
import { Badge, Button } from "./ui";
import type {
  CaptureConfig,
  CaptureTrigger,
  PowerCapture,
  SafeGpio,
} from "@/lib/types";
import {
  formatPowerMetric,
  nominalVoltage,
  powerRailLabel,
  USER_POWER_RAILS,
  type PowerMetric,
} from "@/lib/power";
import {
  calculateCaptureWindow,
  estimateFiveVoltBattery,
  MAX_POWER_CAPTURE_PRE_TRIGGER_SAMPLES,
  powerCapturePreTriggerLimitSeconds,
  powerCapturePreTriggerSamples,
  summarizePowerCapture,
} from "@/lib/powerCapture";
import {
  getPowerCaptureStoragePlan,
  type PowerCaptureStoragePlan,
} from "@/lib/powerCaptureStore";
import {
  exportPowerCaptureToFile,
  type PowerCaptureExportFormat,
  type PowerCaptureExportProgress,
} from "@/lib/powerCaptureExport";
import { useI18n } from "@/lib/i18n";
import { buildPowerChartAxis, PowerChartYAxis } from "./PowerChartAxis";

const COLORS = ["#4f7cff", "#f59e0b", "#22c55e", "#ef4444"];
const WIDTH = 720;
const HEIGHT = 180;
const SAMPLE_RATE_PRESETS = [10, 50, 100, 250, 500];
const MAX_CONTINUOUS_SAMPLE_RATE_HZ = 500;
const DURATION_PRESETS_SECONDS = [10, 30, 300, 1800, 3600, 14400];
const PRE_TRIGGER_PRESETS_SECONDS = [0, 1, 2, 5];
const DEVICE_TELEMETRY_BUFFER_SAMPLES = 256;
const CONTROL_CLASS = "mt-1.5 w-full rounded-lg border border-line bg-panel px-2.5 py-2 text-xs text-ink outline-none transition-colors focus:border-brand/60 focus:ring-2 focus:ring-brand/15 disabled:cursor-not-allowed disabled:opacity-60";

const TRIGGER_OPTIONS: Array<{
  value: CaptureTrigger;
  icon: LucideIcon;
  descriptionKey: string;
}> = [
  { value: "manual", icon: MousePointerClick, descriptionKey: "analyzer.trigger.manual.description" },
  { value: "current", icon: Gauge, descriptionKey: "analyzer.trigger.current.description" },
  { value: "power_on", icon: Power, descriptionKey: "analyzer.trigger.powerOn.description" },
  { value: "gpio", icon: Waypoints, descriptionKey: "analyzer.trigger.gpio.description" },
];

function formatIntegrated(value: number) {
  if (value >= 100) return value.toFixed(1);
  if (value >= 1) return value.toFixed(3);
  if (value >= 0.01) return value.toFixed(4);
  return value.toFixed(6);
}

function formatStorageBytes(bytes: number | null): string {
  if (bytes == null || !Number.isFinite(bytes)) return "—";
  if (bytes < 1024) return `${Math.round(bytes)} B`;
  if (bytes < 1024 ** 2) return `${(bytes / 1024).toFixed(1)} KiB`;
  if (bytes < 1024 ** 3) return `${(bytes / 1024 ** 2).toFixed(bytes < 10 * 1024 ** 2 ? 1 : 0)} MiB`;
  return `${(bytes / 1024 ** 3).toFixed(1)} GiB`;
}

function formatWindowTime(milliseconds: number) {
  if (milliseconds < 1) return `${milliseconds.toFixed(2)} ms`;
  if (milliseconds < 1000) return `${milliseconds.toFixed(milliseconds < 10 ? 2 : 1)} ms`;
  if (milliseconds < 60_000) return `${(milliseconds / 1000).toFixed(milliseconds < 10_000 ? 2 : 1)} s`;
  if (milliseconds < 3_600_000) return `${(milliseconds / 60_000).toFixed(1)} min`;
  return `${(milliseconds / 3_600_000).toFixed(milliseconds < 36_000_000 ? 2 : 1)} h`;
}

function formatRuntime(hours: number | null) {
  if (hours == null || !Number.isFinite(hours)) return "—";
  if (hours < 1 / 60) return `${Math.max(0, hours * 3600).toFixed(0)} s`;
  if (hours < 1) return `${(hours * 60).toFixed(1)} min`;
  if (hours < 48) return `${hours.toFixed(hours < 10 ? 2 : 1)} h`;
  return `${(hours / 24).toFixed(1)} d`;
}

function formatCount(value: number | null) {
  if (value == null || !Number.isFinite(value)) return "—";
  if (value >= 1000) return Math.floor(value).toLocaleString();
  if (value >= 10) return value.toFixed(0);
  return value.toFixed(1);
}

function StepHeading({
  number,
  title,
  description,
}: {
  number: number;
  title: string;
  description: string;
}) {
  return (
    <div className="mb-3 flex items-start gap-2.5">
      <span className="grid h-6 w-6 shrink-0 place-items-center rounded-full bg-brand text-[11px] font-semibold text-on-brand">
        {number}
      </span>
      <div className="min-w-0">
        <h3 className="text-xs font-semibold text-ink">{title}</h3>
        <p className="mt-0.5 max-w-[72ch] text-[11px] leading-relaxed text-ink-dim">{description}</p>
      </div>
    </div>
  );
}

function MetricItem({
  icon,
  label,
  value,
  tone = "text-ink",
}: {
  icon: ReactNode;
  label: string;
  value: string;
  tone?: string;
}) {
  return (
    <div className="min-w-0 px-1 py-1.5">
      <div className="flex items-center gap-1.5 text-[10px] text-ink-dim">{icon}{label}</div>
      <div className={`mt-1 truncate font-mono text-sm font-semibold ${tone}`}>{value}</div>
    </div>
  );
}

export function PowerAnalyzer({
  metric,
  gpios,
  state,
  progress,
  captures,
  onArm,
  onTrigger,
  onStop,
  onCancel,
  onClear,
  defaultOpen = false,
  showHeader = true,
}: {
  metric: PowerMetric;
  gpios: SafeGpio[];
  state: "idle" | "connecting" | "armed" | "recording" | "receiving";
  progress: {
    received: number;
    total: number;
    persisted?: number;
    queuedChunks?: number;
    dropped?: number;
  } | null;
  captures: PowerCapture[];
  onArm: (config: CaptureConfig) => Promise<void>;
  onTrigger: () => void;
  onStop: () => void;
  onCancel: () => void;
  onClear: () => void;
  /** Opens the analyzer when it is displayed as the main workspace content. */
  defaultOpen?: boolean;
  /** Lets a workspace provide the analyzer title alongside its local navigation. */
  showHeader?: boolean;
}) {
  const { t } = useI18n();
  const [trigger, setTrigger] = useState<CaptureTrigger>("manual");
  const [source, setSource] = useState<string>(USER_POWER_RAILS[0]);
  const [gpio, setGpio] = useState("");
  const [edge, setEdge] = useState<CaptureConfig["edge"]>("either");
  const [thresholdMa, setThresholdMa] = useState(500);
  const [rateHz, setRateHz] = useState(50);
  const [preDurationSeconds, setPreDurationSeconds] = useState(1);
  const [stopMode, setStopMode] = useState<"timed" | "manual">("timed");
  const [timedDurationSeconds, setTimedDurationSeconds] = useState(10);
  const [rail, setRail] = useState<string>(USER_POWER_RAILS[0]);
  const [batteryCapacityMah, setBatteryCapacityMah] = useState(10_000);
  const [efficiencyPercent, setEfficiencyPercent] = useState(90);
  const [targetRuntimeHours, setTargetRuntimeHours] = useState(8);
  const [storagePlan, setStoragePlan] = useState<PowerCaptureStoragePlan | null>(null);
  const [exportFormat, setExportFormat] = useState<PowerCaptureExportFormat | null>(null);
  const [exportProgress, setExportProgress] = useState<PowerCaptureExportProgress | null>(null);
  const [exportError, setExportError] = useState<string | null>(null);
  const preDurationLimitSeconds = powerCapturePreTriggerLimitSeconds(rateHz);

  const windowSummary = useMemo(
    () => calculateCaptureWindow(
      rateHz,
      powerCapturePreTriggerSamples(rateHz, preDurationSeconds),
      stopMode === "timed"
        ? Math.max(1, Math.round(timedDurationSeconds * rateHz))
        : 1,
      Number.MAX_SAFE_INTEGER,
    ),
    [preDurationSeconds, rateHz, stopMode, timedDurationSeconds],
  );
  useEffect(() => {
    setPreDurationSeconds((current) => {
      const normalized = Number.isFinite(current) ? Math.max(0, current) : 0;
      return Math.min(normalized, preDurationLimitSeconds);
    });
  }, [preDurationLimitSeconds]);
  useEffect(() => {
    let cancelled = false;
    void getPowerCaptureStoragePlan({
      rateHz: windowSummary.rateHz,
      preSamples: windowSummary.preSamples,
      stopAfterMs: stopMode === "timed" ? Math.max(100, timedDurationSeconds * 1000) : undefined,
    }).then((plan) => {
      if (!cancelled) setStoragePlan(plan);
    }).catch(() => {
      if (!cancelled) setStoragePlan(null);
    });
    return () => {
      cancelled = true;
    };
  }, [stopMode, timedDurationSeconds, windowSummary.preSamples, windowSummary.rateHz]);
  const series = useMemo(() => captures.map((capture, captureIndex) => {
    const triggerTime = capture.triggerDeviceTimeUs ??
      capture.samples[capture.triggerOffset]?.deviceTimeUs ?? 0;
    return {
      capture,
      color: COLORS[captureIndex % COLORS.length],
      points: capture.samples.flatMap((sample) => {
        const reading = sample.readings.find((item) => item.name === rail);
        if (!reading) return [];
        const current = reading.power_enabled ? Math.max(0, reading.current_ua / 1_000_000) : 0;
        const value = metric === "current" ? current : current * (nominalVoltage(rail) ?? 0);
        return [{ x: (sample.deviceTimeUs - triggerTime) / 1000, value }];
      }),
    };
  }), [captures, metric, rail]);
  const allPoints = series.flatMap((item) => item.points);
  const minX = Math.min(-1, ...allPoints.map((point) => point.x));
  const maxX = Math.max(1, ...allPoints.map((point) => point.x));
  const chartAxis = buildPowerChartAxis(
    metric,
    Math.max(metric === "current" ? 0.001 : 0.01, ...allPoints.map((point) => point.value)),
  );
  const maxY = chartAxis.maximum;
  const latestCapture = captures.at(-1);
  const latestSummary = useMemo(
    () => latestCapture ? summarizePowerCapture(latestCapture, rail) : null,
    [latestCapture, rail],
  );
  const latestSampleCount = latestCapture
    ? latestCapture.sampleCount ?? latestCapture.samples.length
    : 0;
  const latestEffectiveRateHz = latestSummary && latestSummary.durationMs > 0 && latestSampleCount > 1
    ? (latestSampleCount - 1) * 1000 / latestSummary.durationMs
    : 0;
  const batteryEstimate = useMemo(
    () => latestSummary && !latestCapture?.incomplete
      ? estimateFiveVoltBattery(
        latestSummary,
        batteryCapacityMah,
        efficiencyPercent,
        targetRuntimeHours,
      )
      : null,
    [batteryCapacityMah, efficiencyPercent, latestCapture?.incomplete, latestSummary, targetRuntimeHours],
  );
  const disabled = state !== "idle";
  const exporting = exportFormat != null;

  const selectSource = (nextSource: string) => {
    setSource(nextSource);
    setRail(nextSource);
  };
  const arm = () => void onArm({
    trigger,
    source: trigger === "gpio" ? (gpio || gpios[0]?.name || "") : source,
    edge: trigger === "gpio" ? edge : "either",
    thresholdUa: Math.round(Math.max(0, thresholdMa) * 1000),
    rateHz: windowSummary.rateHz,
    preSamples: windowSummary.preSamples,
    postSamples: windowSummary.postSamples,
    streaming: true,
    stopAfterMs: stopMode === "timed"
      ? Math.max(100, timedDurationSeconds * 1000)
      : undefined,
  }).catch(() => {
    // useBoard exposes the actionable error through the shared board status.
  });

  const exportCapture = async (
    capture: PowerCapture,
    format: PowerCaptureExportFormat,
  ) => {
    setExportFormat(format);
    setExportProgress({
      writtenSamples: 0,
      totalSamples: capture.sampleCount ?? capture.samples.length,
      writtenBytes: 0,
    });
    setExportError(null);
    try {
      await exportPowerCaptureToFile(capture, format, setExportProgress);
    } catch (reason) {
      if (!(reason instanceof DOMException && reason.name === "AbortError")) {
        setExportError(reason instanceof Error ? reason.message : String(reason));
      }
    } finally {
      setExportFormat(null);
    }
  };

  const Container = showHeader ? "details" : "section";

  return (
    <Container
      className="overflow-hidden rounded-xl border border-line/60 bg-panel2/20"
      {...(showHeader ? { open: defaultOpen } : {})}
    >
      {showHeader && (
        <summary className="flex cursor-pointer list-none items-center gap-2 px-3 py-2.5 text-sm font-medium text-ink">
          <Activity size={15} className="text-brand" />
          <span className="flex-1">{t("analyzer.title")}</span>
          <Badge tone={state === "idle" ? "neutral" : state === "armed" ? "warn" : "brand"}>
            {t(`analyzer.state.${state}`)}
          </Badge>
        </summary>
      )}

      <section className={`${showHeader ? "border-t border-line/60" : ""} p-3 sm:p-4`}>
        <StepHeading
          number={1}
          title={t("analyzer.guide.triggerTitle")}
          description={t("analyzer.guide.triggerDescription")}
        />
        <div role="radiogroup" aria-label={t("analyzer.trigger")} className="grid gap-2 sm:grid-cols-2 xl:grid-cols-4">
          {TRIGGER_OPTIONS.map(({ value, icon: Icon, descriptionKey }) => {
            const selected = trigger === value;
            const labelKey = value === "power_on" ? "powerOn" : value;
            return (
              <button
                key={value}
                type="button"
                role="radio"
                aria-checked={selected}
                disabled={disabled}
                onClick={() => setTrigger(value)}
                className={`flex min-h-[86px] items-start gap-2.5 rounded-xl border p-3 text-left transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/30 disabled:cursor-not-allowed disabled:opacity-60 ${
                  selected
                    ? "border-brand/60 bg-brand/10 text-ink"
                    : "border-line/70 bg-panel/60 text-ink hover:border-brand/30 hover:bg-panel"
                }`}
              >
                <span className={`mt-0.5 grid h-7 w-7 shrink-0 place-items-center rounded-lg ${selected ? "bg-brand text-on-brand" : "bg-panel2 text-ink-dim"}`}>
                  <Icon size={14} />
                </span>
                <span className="min-w-0">
                  <span className="block text-xs font-semibold">{t(`analyzer.trigger.${labelKey}`)}</span>
                  <span className="mt-1 block text-[10px] leading-relaxed text-ink-dim">{t(descriptionKey)}</span>
                </span>
              </button>
            );
          })}
        </div>

        <div className="mt-3 grid gap-3 rounded-xl bg-panel2/50 p-3 sm:grid-cols-2 xl:grid-cols-4">
          {trigger === "gpio" ? (
            <>
              <label className="text-[11px] font-medium text-ink-dim">
                GPIO
                <select
                  value={gpio}
                  onChange={(event) => setGpio(event.target.value)}
                  disabled={disabled}
                  className={CONTROL_CLASS}
                >
                  {gpios.map((item) => <option key={item.name} value={item.name}>{item.name} · {item.note}</option>)}
                </select>
              </label>
              <label className="text-[11px] font-medium text-ink-dim">
                {t("analyzer.edge")}
                <select
                  value={edge}
                  onChange={(event) => setEdge(event.target.value as CaptureConfig["edge"])}
                  disabled={disabled}
                  className={CONTROL_CLASS}
                >
                  <option value="rising">{t("analyzer.edge.rising")}</option>
                  <option value="falling">{t("analyzer.edge.falling")}</option>
                  <option value="either">{t("analyzer.edge.either")}</option>
                </select>
              </label>
            </>
          ) : (
            <label className="text-[11px] font-medium text-ink-dim">
              {t("analyzer.source")}
              <select
                value={source}
                onChange={(event) => selectSource(event.target.value)}
                disabled={disabled}
                className={CONTROL_CLASS}
              >
                {USER_POWER_RAILS.map((name) => <option key={name} value={name}>{powerRailLabel(name)}</option>)}
              </select>
            </label>
          )}
          {trigger === "current" && (
            <label className="text-[11px] font-medium text-ink-dim">
              {t("analyzer.threshold")}
              <input
                type="number"
                min="0"
                value={thresholdMa}
                onChange={(event) => setThresholdMa(Number(event.target.value))}
                disabled={disabled}
                className={CONTROL_CLASS}
              />
            </label>
          )}
        </div>
      </section>

      <section className="border-t border-line/60 p-3 sm:p-4">
        <StepHeading
          number={2}
          title={t("analyzer.guide.windowTitle")}
          description={t("analyzer.guide.windowDescription")}
        />
        <div className="grid gap-3 lg:grid-cols-[minmax(220px,0.85fr)_minmax(0,1.15fr)]">
          <div>
            <label className="text-[11px] font-medium text-ink-dim">
              {t("analyzer.rate")}
              <input
                type="number"
                min="1"
                max={MAX_CONTINUOUS_SAMPLE_RATE_HZ}
                value={rateHz}
                onChange={(event) => setRateHz(Math.min(
                  MAX_CONTINUOUS_SAMPLE_RATE_HZ,
                  Number(event.target.value),
                ))}
                disabled={disabled}
                className={CONTROL_CLASS}
              />
            </label>
            <p className="mt-1.5 text-[10px] leading-relaxed text-ink-dim">
              {t("analyzer.rateHelp")
                .replaceAll("{rate}", String(windowSummary.rateHz))
                .replaceAll("{interval}", formatWindowTime(windowSummary.intervalMs))}
            </p>
            <div className="mt-2 flex flex-wrap gap-1">
              {SAMPLE_RATE_PRESETS.map((preset) => (
                <button
                  key={preset}
                  type="button"
                  disabled={disabled}
                  onClick={() => setRateHz(preset)}
                  className={`min-h-6 rounded-md border px-1.5 text-[9px] transition-colors ${
                    windowSummary.rateHz === preset
                      ? "border-brand/50 bg-brand/10 text-brand"
                      : "border-line/60 bg-panel text-ink-dim hover:text-ink"
                  }`}
                >
                  {preset} Hz
                </button>
              ))}
            </div>
          </div>
          <div className="grid gap-3 rounded-xl border border-line/60 bg-panel/55 p-3 sm:grid-cols-2">
            <div>
              <label className="text-[11px] font-medium text-ink-dim">
                {t("analyzer.preDuration")}
                <input
                  type="number"
                  min="0"
                  max={preDurationLimitSeconds}
                  step="0.1"
                  value={preDurationSeconds}
                  onChange={(event) => setPreDurationSeconds(Number(event.target.value))}
                  disabled={disabled}
                  className={CONTROL_CLASS}
                />
              </label>
              <p className="mt-1.5 text-[10px] font-normal leading-relaxed text-ink-dim">
                {t("analyzer.preDurationHelp")
                  .replaceAll("{samples}", String(windowSummary.preSamples))
                  .replaceAll("{duration}", formatWindowTime(windowSummary.preDurationMs))}
              </p>
              <p className="mt-1 text-[9px] text-ink-dim">
                {t("analyzer.preDurationLimit")
                  .replaceAll("{duration}", formatWindowTime(preDurationLimitSeconds * 1000))
                  .replaceAll("{samples}", MAX_POWER_CAPTURE_PRE_TRIGGER_SAMPLES.toLocaleString())}
              </p>
              <div className="mt-2 flex flex-wrap gap-1">
                {PRE_TRIGGER_PRESETS_SECONDS.map((preset) => (
                  <button
                    key={preset}
                    type="button"
                    disabled={disabled}
                    onClick={() => setPreDurationSeconds(preset)}
                    className={`min-h-6 rounded-md border px-1.5 text-[9px] transition-colors ${
                      preDurationSeconds === preset
                        ? "border-brand/50 bg-brand/10 text-brand"
                        : "border-line/60 bg-panel text-ink-dim hover:text-ink"
                    }`}
                  >
                    {preset} s
                  </button>
                ))}
              </div>
            </div>
            <div>
              <span className="text-[11px] font-medium text-ink-dim">{t("analyzer.stopMode")}</span>
              <div role="radiogroup" aria-label={t("analyzer.stopMode")} className="mt-1.5 grid grid-cols-2 gap-1.5">
                {(["timed", "manual"] as const).map((mode) => (
                  <button
                    key={mode}
                    type="button"
                    role="radio"
                    aria-checked={stopMode === mode}
                    disabled={disabled}
                    onClick={() => setStopMode(mode)}
                    className={`min-h-9 rounded-lg border px-2 text-[10px] font-medium transition-colors ${
                      stopMode === mode
                        ? "border-brand/55 bg-brand/10 text-brand"
                        : "border-line/70 bg-panel text-ink-dim hover:text-ink"
                    }`}
                  >
                    {mode === "timed" ? <Clock3 size={12} className="mr-1 inline" /> : <Square size={11} className="mr-1 inline" />}
                    {t(`analyzer.stopMode.${mode}`)}
                  </button>
                ))}
              </div>
              {stopMode === "timed" ? (
                <>
                  <label className="mt-2 block text-[11px] font-medium text-ink-dim">
                    {t("analyzer.durationSeconds")}
                    <input
                      type="number"
                      min="0.1"
                      step="0.1"
                      value={timedDurationSeconds}
                      onChange={(event) => setTimedDurationSeconds(Number(event.target.value))}
                      disabled={disabled}
                      className={CONTROL_CLASS}
                    />
                  </label>
                  <div className="mt-2 flex flex-wrap gap-1">
                    {DURATION_PRESETS_SECONDS.map((preset) => (
                      <button
                        key={preset}
                        type="button"
                        disabled={disabled}
                        onClick={() => setTimedDurationSeconds(preset)}
                        className={`min-h-6 rounded-md border px-2 text-[9px] transition-colors ${
                          timedDurationSeconds === preset
                            ? "border-brand/50 bg-brand/10 text-brand"
                            : "border-line/60 bg-panel text-ink-dim hover:text-ink"
                        }`}
                      >
                        {formatWindowTime(preset * 1000)}
                      </button>
                    ))}
                  </div>
                </>
              ) : (
                <p className="mt-2 rounded-lg bg-brand/10 px-2.5 py-2 text-[10px] leading-relaxed text-ink-dim">
                  {t("analyzer.manualStopHelp")}
                </p>
              )}
            </div>
          </div>
        </div>

        <div className="mt-3 overflow-hidden rounded-xl border border-line/60 bg-panel/60">
          <div className="grid grid-cols-[minmax(0,1fr)_auto_minmax(0,1fr)] items-stretch">
            <div className="min-w-0 bg-brand/10 px-3 py-2 text-right">
              <div className="truncate text-[10px] font-medium text-brand">{t("analyzer.timeline.before")}</div>
              <div className="mt-0.5 font-mono text-[10px] text-ink-dim">{windowSummary.preSamples} · {formatWindowTime(windowSummary.preDurationMs)}</div>
            </div>
            <div className="grid min-w-20 place-items-center border-x border-line/60 bg-panel2 px-2 py-2">
              <Radio size={13} className="text-danger" />
              <span className="text-[9px] font-medium text-danger">{t("analyzer.timeline.trigger")}</span>
            </div>
            <div className="min-w-0 bg-ok/10 px-3 py-2">
              <div className="truncate text-[10px] font-medium text-ok">
                {t(stopMode === "timed" ? "analyzer.timeline.timed" : "analyzer.timeline.manual")}
              </div>
              <div className="mt-0.5 font-mono text-[10px] text-ink-dim">
                {stopMode === "timed" ? formatWindowTime(windowSummary.postDurationMs) : t("analyzer.timeline.untilStop")}
              </div>
            </div>
          </div>
          <div className="border-t border-line/60 px-3 py-2 text-[10px] text-ink-dim">
            {t(stopMode === "timed" ? "analyzer.streamingWindowSummary" : "analyzer.streamingManualSummary")
              .replaceAll("{samples}", String(windowSummary.totalSamples))
              .replaceAll("{duration}", formatWindowTime(windowSummary.totalDurationMs))
              .replaceAll("{buffer}", String(DEVICE_TELEMETRY_BUFFER_SAMPLES))}
          </div>
          <div className={`flex flex-wrap items-center gap-x-3 gap-y-1 border-t border-line/60 px-3 py-2 text-[10px] ${
            storagePlan?.sufficient === false ? "bg-danger/10 text-danger" : "bg-panel2/35 text-ink-dim"
          }`}>
            <span className="inline-flex items-center gap-1 font-medium">
              <HardDrive size={11} />
              {t("analyzer.storage.title")}
            </span>
            <span>
              {stopMode === "timed"
                ? t("analyzer.storage.timed")
                  .replaceAll("{needed}", formatStorageBytes(storagePlan?.projectedBytes ?? null))
                  .replaceAll("{available}", formatStorageBytes(
                    storagePlan?.availableBytes == null || storagePlan.reserveBytes == null
                      ? null
                      : Math.max(0, storagePlan.availableBytes - storagePlan.reserveBytes),
                  ))
                : t("analyzer.storage.manual")
                  .replaceAll("{available}", formatStorageBytes(
                    storagePlan?.availableBytes == null || storagePlan.reserveBytes == null
                      ? null
                      : Math.max(0, storagePlan.availableBytes - storagePlan.reserveBytes),
                  ))}
            </span>
            {storagePlan?.persisted === false && (
              <span className="text-warn">{t("analyzer.storage.notPersistent")}</span>
            )}
          </div>
        </div>
      </section>

      <section className="border-t border-line/60 p-3 sm:p-4">
        <StepHeading
          number={3}
          title={t("analyzer.guide.captureTitle")}
          description={t("analyzer.guide.captureDescription")}
        />
        <div className="flex flex-wrap items-center gap-2">
          {state === "idle" ? (
            <Button
              variant="primary"
              onClick={arm}
              disabled={trigger === "gpio" && gpios.length === 0}
            >
              <Radio size={15} />{t("analyzer.arm")}
            </Button>
          ) : state === "recording" ? (
            <>
              <Button variant="primary" onClick={onStop}><Square size={15} />{t("analyzer.stopAndSave")}</Button>
              <Button variant="ghost" onClick={onCancel}><Trash2 size={14} />{t("analyzer.discard")}</Button>
            </>
          ) : state === "receiving" ? (
            <Button disabled><Download size={14} />{t("analyzer.saving")}</Button>
          ) : (
            <Button onClick={onCancel}><Square size={15} />{t("analyzer.cancel")}</Button>
          )}
          {state === "armed" && trigger === "manual" && (
            <Button variant="primary" onClick={onTrigger}><Play size={15} />{t("analyzer.fire")}</Button>
          )}
          <Badge tone={state === "idle" ? "neutral" : state === "armed" ? "warn" : "brand"}>
            {t(`analyzer.state.${state}`)}
          </Badge>
          {state === "recording" && (
            <span className="text-[10px] text-ink-dim">
              {t(stopMode === "timed" ? "analyzer.recordingTimed" : "analyzer.recordingManual")
                .replaceAll("{duration}", formatWindowTime(windowSummary.postDurationMs))}
            </span>
          )}
          {progress && progress.total > 0 && (
            <div className="flex min-w-[180px] flex-1 items-center gap-2 sm:max-w-sm">
              <div className="h-1.5 min-w-0 flex-1 overflow-hidden rounded-full bg-line/60">
                <span
                  className="block h-full rounded-full bg-brand transition-[width] duration-150"
                  style={{ width: `${Math.min(100, progress.received / Math.max(1, progress.total) * 100)}%` }}
                />
              </div>
              <span className="shrink-0 font-mono text-[10px] text-ink-dim">{progress.received}/{progress.total}</span>
            </div>
          )}
          {progress && progress.total === 0 && (
            <span className="font-mono text-[10px] text-ink-dim">
              {t("analyzer.receivedSamples").replaceAll("{samples}", progress.received.toLocaleString())}
            </span>
          )}
          {progress?.persisted != null && (
            <span className="font-mono text-[10px] text-ink-dim">
              {t("analyzer.storage.progress")
                .replaceAll("{persisted}", progress.persisted.toLocaleString())
                .replaceAll("{queued}", String(progress.queuedChunks ?? 0))
                .replaceAll("{dropped}", (progress.dropped ?? 0).toLocaleString())}
            </span>
          )}
        </div>
      </section>

      {captures.length > 0 && latestCapture && latestSummary && (
        <section className="border-t border-line/60 bg-panel/45 p-3 sm:p-4">
          <div className="mb-3 flex flex-wrap items-start justify-between gap-3">
            <div>
              <div className="flex items-center gap-2 text-xs font-semibold text-ink">
                <span className="grid h-6 w-6 place-items-center rounded-full bg-ok/15 text-ok">4</span>
                {t("analyzer.results.title")}
              </div>
              <p className="mt-1 max-w-[72ch] text-[10px] leading-relaxed text-ink-dim">{t("analyzer.results.description")}</p>
            </div>
            <div className="flex flex-wrap items-center justify-end gap-1.5">
              <select
                value={rail}
                onChange={(event) => setRail(event.target.value)}
                className="min-h-8 rounded-lg border border-line bg-panel px-2 py-1.5 text-xs text-ink"
                aria-label={t("analyzer.results.rail")}
              >
                {USER_POWER_RAILS.map((name) => <option key={name} value={name}>{powerRailLabel(name)}</option>)}
              </select>
              <Button disabled={disabled || exporting} variant="ghost" className="min-h-8 px-2 py-1 text-xs" onClick={() => void exportCapture(latestCapture, "csv")}><Download size={13} />CSV</Button>
              <Button disabled={disabled || exporting} variant="ghost" className="min-h-8 px-2 py-1 text-xs" onClick={() => void exportCapture(latestCapture, "ndjson")}><Download size={13} />NDJSON</Button>
              <Button disabled={disabled || exporting} variant="ghost" className="min-h-8 px-2 py-1" onClick={onClear} aria-label={t("analyzer.clear")}><Trash2 size={13} /></Button>
            </div>
          </div>
          {exportFormat && exportProgress && (
            <div className="mb-3 rounded-lg border border-brand/25 bg-brand/10 px-3 py-2 text-[10px] text-ink-dim">
              {t("analyzer.export.progress")
                .replaceAll("{format}", exportFormat.toUpperCase())
                .replaceAll("{written}", exportProgress.writtenSamples.toLocaleString())
                .replaceAll("{total}", exportProgress.totalSamples.toLocaleString())
                .replaceAll("{bytes}", formatStorageBytes(exportProgress.writtenBytes))}
            </div>
          )}
          {exportError && (
            <div className="mb-3 rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-[10px] text-danger">
              {t("analyzer.export.failed").replaceAll("{error}", exportError)}
            </div>
          )}

          <div className="grid divide-y divide-line/50 rounded-xl border border-line/60 bg-panel px-2 sm:grid-cols-3 sm:divide-x sm:divide-y-0 xl:grid-cols-6">
            <MetricItem icon={<Clock3 size={12} />} label={t("analyzer.duration")} value={formatWindowTime(latestSummary.durationMs)} />
            <MetricItem icon={<Activity size={12} />} label={t("analyzer.averageCurrent")} value={formatPowerMetric(latestSummary.averageCurrentA, "current")} tone="text-brand" />
            <MetricItem icon={<Gauge size={12} />} label={t("analyzer.peakCurrent")} value={formatPowerMetric(latestSummary.peakCurrentA, "current")} />
            <MetricItem icon={<Zap size={12} />} label={t("analyzer.averagePower")} value={formatPowerMetric(latestSummary.averagePowerW, "power")} />
            <MetricItem icon={<BatteryCharging size={12} />} label={t("analyzer.charge")} value={`${formatIntegrated(latestSummary.milliampHours)} mAh`} />
            <MetricItem icon={<Zap size={12} />} label={t("analyzer.energy")} value={`${formatIntegrated(latestSummary.wattHours)} Wh`} tone="text-warn" />
          </div>
          <p className="mt-1.5 text-[9px] text-ink-dim">
            {t("analyzer.results.estimateNote")
              .replaceAll("{voltage}", String(latestSummary.nominalVoltageV))
              .replaceAll("{id}", String(latestCapture.id))}
          </p>
          <p className={`mt-1 text-[9px] ${latestCapture.incomplete || latestCapture.droppedSamples ? "text-danger" : "text-ok"}`}>
            {latestCapture.droppedSamples
              ? t("analyzer.results.dropped")
                .replaceAll("{count}", latestSampleCount.toLocaleString())
                .replaceAll("{rate}", latestEffectiveRateHz.toFixed(1))
                .replaceAll("{dropped}", latestCapture.droppedSamples.toLocaleString())
              : latestCapture.incomplete
                ? t("analyzer.results.incomplete")
                  .replaceAll("{count}", latestSampleCount.toLocaleString())
                  .replaceAll("{rate}", latestEffectiveRateHz.toFixed(1))
              : t("analyzer.results.noDrops")
                .replaceAll("{count}", latestSampleCount.toLocaleString())
                .replaceAll("{rate}", latestEffectiveRateHz.toFixed(1))}
          </p>

          <div className="mt-3">
            <div className="mb-1 flex items-center justify-between gap-2 text-[10px]">
              <span className="font-medium text-ink">
                {t(`power.chart.${metric}`)} <span className="font-mono font-normal text-ink-dim">({chartAxis.unit})</span>
              </span>
            </div>
            <div className="grid grid-cols-[3rem_minmax(0,1fr)] gap-x-2">
              <PowerChartYAxis axis={chartAxis} />
              <svg
                viewBox={`0 0 ${WIDTH} ${HEIGHT}`}
                preserveAspectRatio="none"
                role="img"
                aria-label={`${t(`power.chart.${metric}`)} (${chartAxis.unit})`}
                className="h-44 w-full rounded-lg border border-line/60 bg-panel"
              >
                <title>{`${t(`power.chart.${metric}`)} (${chartAxis.unit})`}</title>
                {[0.25, 0.5, 0.75].map((ratio) => <line key={ratio} x1="0" x2={WIDTH} y1={HEIGHT * ratio} y2={HEIGHT * ratio} stroke="rgb(var(--c-line))" strokeDasharray="3 5" />)}
                <line x1={(-minX / (maxX - minX)) * WIDTH} x2={(-minX / (maxX - minX)) * WIDTH} y1="0" y2={HEIGHT} stroke="rgb(var(--c-danger))" strokeDasharray="4 3" />
                {series.map(({ capture, color, points }) => (
                  <polyline
                    key={capture.id}
                    fill="none"
                    stroke={color}
                    strokeWidth="1.7"
                    vectorEffect="non-scaling-stroke"
                    points={points.map((point) => `${(point.x - minX) / (maxX - minX) * WIDTH},${HEIGHT - point.value / maxY * (HEIGHT - 8) - 4}`).join(" ")}
                  />
                ))}
              </svg>
              <span />
              <div className="mt-1 flex min-w-0 items-center justify-between gap-2 text-[9px] text-ink-dim">
                <span className="font-mono">{minX.toFixed(0)} ms</span>
                <span className="flex min-w-0 flex-1 items-center justify-center gap-2">
                  {captures.map((capture, index) => (
                    <span key={capture.id} className="truncate"><i className="mr-1 inline-block h-2 w-2 rounded-full" style={{ background: COLORS[index % COLORS.length] }} />#{capture.id}</span>
                  ))}
                </span>
                <span className="font-mono">{maxX.toFixed(0)} ms</span>
              </div>
            </div>
          </div>

          <div className="mt-4 overflow-hidden rounded-xl border border-line/70 bg-panel">
            <div className="border-b border-line/60 px-3 py-2.5">
              <div className="flex items-center gap-2 text-xs font-semibold text-ink"><BatteryCharging size={14} className="text-brand" />{t("analyzer.battery.title")}</div>
              <p className="mt-1 text-[10px] leading-relaxed text-ink-dim">{t("analyzer.battery.description")}</p>
            </div>
            {batteryEstimate ? <><div className="grid gap-4 p-3 lg:grid-cols-[minmax(220px,0.7fr)_minmax(0,1.3fr)]">
              <div className="grid content-start gap-3 sm:grid-cols-3 lg:grid-cols-1">
                <label className="text-[11px] font-medium text-ink-dim">
                  {t("analyzer.battery.capacity")}
                  <input type="number" min="0" value={batteryCapacityMah} onChange={(event) => setBatteryCapacityMah(Number(event.target.value))} className={CONTROL_CLASS} />
                </label>
                <label className="text-[11px] font-medium text-ink-dim">
                  {t("analyzer.battery.efficiency")}
                  <input type="number" min="1" max="100" value={efficiencyPercent} onChange={(event) => setEfficiencyPercent(Number(event.target.value))} className={CONTROL_CLASS} />
                </label>
                <label className="text-[11px] font-medium text-ink-dim">
                  {t("analyzer.battery.targetRuntime")}
                  <input type="number" min="0" step="0.5" value={targetRuntimeHours} onChange={(event) => setTargetRuntimeHours(Number(event.target.value))} className={CONTROL_CLASS} />
                </label>
              </div>
              <div className="min-w-0">
                <div className="rounded-lg bg-brand/10 px-3 py-2.5">
                  <div className="text-[10px] text-ink-dim">{t("analyzer.battery.thisRun")}</div>
                  <div className="mt-1 text-sm font-semibold text-ink">
                    {formatIntegrated(batteryEstimate.equivalentCapacityMah)} mAh @ 5V
                  </div>
                  <p className="mt-1 text-[10px] leading-relaxed text-ink-dim">
                    {t("analyzer.battery.thisRunDetail")
                      .replaceAll("{energy}", formatIntegrated(latestSummary.wattHours))
                      .replaceAll("{input}", formatIntegrated(batteryEstimate.rechargeInputWh))
                      .replaceAll("{efficiency}", String(Math.round(batteryEstimate.efficiency * 100)))}
                  </p>
                </div>
                <dl className="mt-2 divide-y divide-line/50 text-[11px]">
                  <div className="flex items-center justify-between gap-3 py-2">
                    <dt className="text-ink-dim">{t("analyzer.battery.runtime").replaceAll("{capacity}", batteryCapacityMah.toLocaleString())}</dt>
                    <dd className="shrink-0 font-mono font-semibold text-ink">{formatRuntime(batteryEstimate.runtimeHours)}</dd>
                  </div>
                  <div className="flex items-center justify-between gap-3 py-2">
                    <dt className="text-ink-dim">{t("analyzer.battery.required").replaceAll("{hours}", String(targetRuntimeHours))}</dt>
                    <dd className="shrink-0 font-mono font-semibold text-brand">{batteryEstimate.requiredCapacityMah == null ? "—" : `${formatIntegrated(batteryEstimate.requiredCapacityMah)} mAh`}</dd>
                  </div>
                  <div className="flex items-center justify-between gap-3 py-2">
                    <dt className="text-ink-dim">{t("analyzer.battery.repeats")}</dt>
                    <dd className="shrink-0 font-mono font-semibold text-ink">{formatCount(batteryEstimate.repeatCount)} ×</dd>
                  </div>
                </dl>
                {batteryEstimate.runtimeHours == null && <p className="mt-2 text-[10px] text-warn">{t("analyzer.battery.noLoad")}</p>}
              </div>
            </div>
            <div className="flex items-start gap-2 border-t border-line/60 bg-panel2/40 px-3 py-2.5 text-[10px] leading-relaxed text-ink-dim">
              <History size={13} className="mt-0.5 shrink-0 text-brand" />
              <span>{t("analyzer.battery.fullCaptureTip")}</span>
            </div></> : (
              <div className="m-3 rounded-lg border border-danger/30 bg-danger/10 px-3 py-2.5 text-[10px] leading-relaxed text-danger">
                {t("analyzer.battery.incomplete")}
              </div>
            )}
          </div>
        </section>
      )}
    </Container>
  );
}
