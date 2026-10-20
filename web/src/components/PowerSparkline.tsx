import { memo, useCallback, useEffect, useId, useMemo, useRef, useState, type RefObject } from "react";
import { Maximize2, RotateCcw, ZoomIn, ZoomOut } from "lucide-react";
import type { CurrentAdcReading, VoltageAdcReading } from "@/lib/types";
import { appendMeasurementHistory, MEASUREMENT_HISTORY_LIMIT, MEASUREMENT_PREVIEW_SAMPLES } from "@/lib/adc";
import { formatPowerMetric, powerRailLabel, readingMetric, type PowerMetric } from "@/lib/power";
import { useI18n } from "@/lib/i18n";
import { Button, Dialog } from "./ui";

type MeasurementSample = {
  // Host observation time: this preview intentionally does not retain every
  // device sample from a high-rate telemetry batch.
  readonly timestampMs: number;
  readonly gapBefore: boolean;
  readonly current: number | null;
  readonly power: number | null;
  readonly voltage: number | null;
  readonly voltageUv: number | null;
};

type MeasurementSparklineProps =
  | { readonly mode: "power"; readonly reading: CurrentAdcReading }
  | { readonly mode: "voltage"; readonly reading: VoltageAdcReading };

type ChartTrace = {
  readonly metric: PowerMetric;
  readonly values: readonly (number | null)[];
  readonly minimumScale: number;
  readonly maximumScale?: number;
  readonly color: string;
  readonly dash?: string;
  readonly label: string;
  readonly unit: string;
  readonly formatLatest: (value: number) => string;
};

const WIDTH = 320;
const HEIGHT = 58;
const EXPANDED_WIDTH = 960;
const EXPANDED_HEIGHT = 280;
const ADC3_MAX_UV = 3_300_000;
const POWER_TRACES = [
  { metric: "current", minimumScale: 0.001, color: "rgb(var(--c-brand))", unit: "A" },
  { metric: "voltage", minimumScale: 0.1, color: "rgb(var(--c-ok))", unit: "V", dash: "5 3" },
  { metric: "power", minimumScale: 0.01, color: "rgb(var(--c-warn))", unit: "W", dash: "2 3" },
] as const;

function historyOffset(seconds: number) {
  return `-${seconds.toFixed(seconds < 1 ? 3 : seconds < 10 ? 2 : 1)}s`;
}

function measurementSample(props: MeasurementSparklineProps, timestampMs: number): MeasurementSample {
  return props.mode === "power" ? {
    timestampMs,
    gapBefore: false,
    current: readingMetric(props.reading, "current"),
    power: readingMetric(props.reading, "power"),
    voltage: readingMetric(props.reading, "voltage"),
    voltageUv: null,
  } : {
    timestampMs,
    gapBefore: false,
    current: null,
    power: null,
    voltage: null,
    voltageUv: props.reading.value,
  };
}

function appendSample(previous: readonly MeasurementSample[], sample: MeasurementSample) {
  const latest = previous.at(-1);
  // A wall-clock reset must not draw backwards into a previous session.
  if (latest && sample.timestampMs < latest.timestampMs) return [sample];
  const recent = previous.slice(-9);
  const intervals = recent.flatMap((item, index) => {
    const interval = index > 0 ? item.timestampMs - recent[index - 1].timestampMs : 0;
    return !item.gapBefore && interval > 0 ? [interval] : [];
  }).sort((a, b) => a - b);
  // Cover both the 60 Hz preview and the 2 s HTTP polling cadence. Exclude
  // known gaps from the median so a pause never becomes the new normal rate.
  const gapThresholdMs = Math.max(250, (intervals[Math.floor(intervals.length / 2)] ?? 2000) * 3);
  return appendMeasurementHistory(previous, {
    ...sample,
    gapBefore: latest !== undefined && sample.timestampMs - latest.timestampMs > gapThresholdMs,
  });
}

function plotTrace(trace: ChartTrace, history: readonly MeasurementSample[], width: number, height: number) {
  const validValues = trace.values.filter((value): value is number => value !== null);
  // Each trace has its own range: volts must not share a numeric Y axis with amps or watts.
  const extent = Math.max(trace.minimumScale, ...validValues.map(Math.abs)) * 1.15;
  // Symmetric signed domains keep zero aligned across independently scaled
  // overlay traces, without suggesting that volts, amps and watts share units.
  // ADC3 alone retains its explicitly fixed 0–3.3 V range.
  const minimum = trace.maximumScale === undefined ? -extent : 0;
  const maximum = trace.maximumScale ?? extent;
  const yForValue = (value: number) => height - 4 -
    ((Math.min(maximum, Math.max(minimum, value)) - minimum) / (maximum - minimum)) * (height - 8);
  const segments: { x: number; y: number }[][] = [];
  let segment: (typeof segments)[number] = [];
  const firstMs = history[0]?.timestampMs ?? 0;
  const durationMs = (history.at(-1)?.timestampMs ?? firstMs) - firstMs;
  trace.values.forEach((value, index) => {
    if (history[index].gapBefore) segment = [];
    if (value === null) {
      segment = [];
      return;
    }
    if (segment.length === 0) segments.push(segment);
    const x = durationMs > 0 ? ((history[index].timestampMs - firstMs) / durationMs) * width : width;
    segment.push({ x, y: yForValue(value) });
  });
  const latest = trace.values.at(-1) ?? null;
  return { ...trace, minimum, maximum, segments, latest, latestY: latest === null ? null : yForValue(latest) };
}

export function MeasurementSparkline(props: MeasurementSparklineProps) {
  // A reused component must never join samples from different rails or sensor modes.
  return <MeasurementSparklineHistory key={`${props.mode}:${props.reading.name}`} {...props} />;
}

function MeasurementSparklineHistory(props: MeasurementSparklineProps) {
  const { t } = useI18n();
  const [expanded, setExpanded] = useState(false);
  const closeExpanded = useCallback(() => setExpanded(false), []);
  const [history, setHistory] = useState<readonly MeasurementSample[]>([]);
  const lastReadingRef = useRef<MeasurementSparklineProps["reading"] | null>(null);

  useEffect(() => {
    if (lastReadingRef.current === props.reading) return;
    lastReadingRef.current = props.reading;
    const sample = measurementSample(props, Date.now());
    setHistory((previous) => appendSample(previous, sample));
  }, [props.mode, props.reading]);

  // One bounded history per rail. Only an open dialog plots the full buffer;
  // previews remain cheap and never duplicate the retained sample objects.
  const preview = useMemo(() => history.slice(-MEASUREMENT_PREVIEW_SAMPLES), [history]);
  const name = props.mode === "power" ? powerRailLabel(props.reading.name) : t("adc3.title");
  return (
    <div className="min-w-0">
      <div className="mb-1 flex items-center justify-between gap-2 text-[10px] text-ink-dim">
        <span>{t("power.chart.preview", { count: MEASUREMENT_PREVIEW_SAMPLES })}</span>
        <button
          type="button"
          aria-label={`${name} · ${t("power.chart.enlarge")}`}
          aria-haspopup="dialog"
          onClick={() => setExpanded(true)}
          className="inline-flex min-h-8 shrink-0 items-center gap-1.5 rounded-lg px-2 text-[11px] font-medium text-brand transition-colors hover:bg-brand/10 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40"
        >
          <Maximize2 size={13} aria-hidden="true" />{t("power.chart.enlarge")}
        </button>
      </div>
      <MeasurementChart mode={props.mode} name={name} history={preview} />
      {expanded && (
        <Dialog
          title={`${name} · ${t("power.chart.title")}`}
          description={t("power.chart.history", { count: history.length, limit: MEASUREMENT_HISTORY_LIMIT })}
          closeLabel={t("power.chart.close")}
          onClose={closeExpanded}
        >
          <ExpandedMeasurementChart mode={props.mode} name={name} history={history} />
        </Dialog>
      )}
    </div>
  );
}

type MeasurementChartProps = {
  readonly mode: MeasurementSparklineProps["mode"];
  readonly name: string;
  readonly history: readonly MeasurementSample[];
};

function ExpandedMeasurementChart(props: MeasurementChartProps) {
  const { t } = useI18n();
  const [zoom, setZoom] = useState(1);
  const viewportRef = useRef<HTMLDivElement>(null);
  return (
    <>
      <div className="mb-4 flex flex-wrap items-center justify-between gap-3">
        <p className="max-w-xl text-xs leading-relaxed text-ink-dim">{t("power.chart.zoomHint")}</p>
        <div role="group" aria-label={t("power.chart.zoomControls")} className="flex items-center gap-1.5">
          <Button type="button" aria-label={t("power.chart.zoomOut")} disabled={zoom === 1} onClick={() => setZoom((value) => value / 2)} className="min-h-9 rounded-lg px-2.5">
            <ZoomOut size={15} aria-hidden="true" />
          </Button>
          <output aria-label={t("power.chart.zoomLevel")} aria-live="polite" className="min-w-9 text-center font-mono text-sm font-medium text-ink">{zoom}×</output>
          <Button type="button" aria-label={t("power.chart.zoomIn")} disabled={zoom === 8} onClick={() => setZoom((value) => value * 2)} className="min-h-9 rounded-lg px-2.5">
            <ZoomIn size={15} aria-hidden="true" />
          </Button>
          <Button type="button" disabled={zoom === 1} onClick={() => {
            setZoom(1);
            if (viewportRef.current) viewportRef.current.scrollLeft = 0;
          }} className="min-h-9 rounded-lg px-2.5 text-xs">
            <RotateCcw size={13} aria-hidden="true" />{t("power.chart.zoomReset")}
          </Button>
        </div>
      </div>
      <MeasurementChart {...props} zoom={zoom} viewportRef={viewportRef} />
    </>
  );
}

const MeasurementChart = memo(function MeasurementChart({ mode, name, history, zoom, viewportRef }: MeasurementChartProps & {
  readonly zoom?: number;
  readonly viewportRef?: RefObject<HTMLDivElement>;
}) {
  const { t } = useI18n();
  const scrollHintId = useId();
  const large = zoom !== undefined;
  const width = large ? EXPANDED_WIDTH : WIDTH;
  const height = large ? EXPANDED_HEIGHT : HEIGHT;
  const traces: readonly ChartTrace[] = mode === "power" ? POWER_TRACES.map((trace) => ({
    ...trace,
    values: history.map((sample) => sample[trace.metric]),
    label: t(trace.metric === "voltage" && large ? "power.chart.measuredVoltage" : `power.chart.${trace.metric}`),
    formatLatest: (value: number) => formatPowerMetric(value, trace.metric),
  })) : [{
    metric: "voltage",
    values: history.map((sample) => sample.voltageUv),
    minimumScale: ADC3_MAX_UV,
    maximumScale: ADC3_MAX_UV,
    color: "rgb(var(--c-brand))",
    label: t("adc3.title"),
    unit: "V",
    formatLatest: (value: number) => `${(value / 1_000_000).toFixed(3)} V`,
  }];
  const plotted = traces.map((trace) => plotTrace(trace, history, width, height));
  const ariaLabel = mode === "power"
    ? `${name} · ${t("power.chart.overlaid")} · ${t("power.chart.independentScales")} · ${t("power.chart.zeroBaseline")}`
    : t("adc3.chart.aria");
  const hasSamples = plotted.some((trace) => trace.segments.length > 0);
  const noVoltage = mode === "power" && plotted.find((trace) => trace.metric === "voltage")?.latest === null;
  const firstSample = history[0];
  const latestSample = history.at(-1);
  const elapsedSeconds = firstSample && latestSample ? (latestSample.timestampMs - firstSample.timestampMs) / 1000 : 0;
  const duration = history.length > 1 ? Math.max(1, Math.round(elapsedSeconds)) : 0;

  const graphic = hasSamples ? (
        <svg
          viewBox={`0 0 ${width} ${height}`}
          preserveAspectRatio="none"
          role="img"
          aria-label={ariaLabel}
          className={large ? "block h-[clamp(200px,40vh,420px)] w-full" : "block h-16 w-full"}
        >
          {mode === "power" ? <g aria-hidden="true">
            <line data-reference="zero" x1="0" x2={width} y1={height / 2} y2={height / 2}
              stroke="rgb(var(--c-line))" strokeDasharray="3 5" />
            <text x="2" y={height / 2 - 3} fill="rgb(var(--c-ink-dim))" fontSize={large ? 12 : 8}>0</text>
          </g> : [0.33, 0.66].map((ratio) => (
            <line key={ratio} x1="0" x2={width} y1={height * ratio} y2={height * ratio}
              stroke="rgb(var(--c-line))" strokeOpacity="0.55" strokeDasharray="3 5" />
          ))}
          {plotted.map((trace) => (
            <g key={trace.metric} data-metric={trace.metric}>
              {trace.segments.map((points, index) => points.length === 1 ? (
                <circle key={index} cx={points[0].x} cy={points[0].y} r="2" fill={trace.color} />
              ) : (
                <polyline
                  key={index}
                  points={points.map(({ x, y }) => `${x},${y}`).join(" ")}
                  fill="none"
                  stroke={trace.color}
                  strokeDasharray={trace.dash}
                  strokeWidth="2"
                  strokeLinecap="round"
                  strokeLinejoin="round"
                  vectorEffect="non-scaling-stroke"
                />
              ))}
              {trace.latestY !== null && <circle cx={width} cy={trace.latestY} r="2.5" fill={trace.color} />}
            </g>
          ))}
        </svg>
      ) : <div role="status" aria-label={ariaLabel} className={large ? "flex h-64 items-center justify-center text-xs text-ink-dim" : "flex h-16 items-center justify-center text-[11px] text-ink-dim"}>{t("power.chart.collecting")}</div>;

  return (
    <div data-measurement-view={large ? "expanded" : "preview"} className={large ? "min-w-0 rounded-xl border border-line/70 bg-panel2/35 p-3 sm:p-4" : "min-w-0 overflow-hidden rounded-lg border border-line/50 bg-panel2/35 px-2 py-1.5"}>
      {mode === "power" && (
        <ul aria-label={t("power.chart.legend")} className={large ? "mb-4 grid grid-cols-3 gap-3" : "mb-1 grid grid-cols-3 gap-1.5 sm:gap-2"}>
          {plotted.map((trace) => (
            <li key={trace.metric} data-metric={trace.metric} className="min-w-0">
              <div className={large ? "flex min-h-8 items-center gap-1 text-[11px] font-medium sm:min-h-0" : "flex min-w-0 items-center gap-1 text-[10px] font-medium leading-4 sm:text-[11px]"} style={{ color: trace.color }}>
                <span
                  aria-hidden="true"
                  className="w-3 shrink-0 border-t-2"
                  style={{
                    color: trace.color,
                    borderTopColor: trace.color,
                    borderStyle: trace.metric === "voltage" ? "dashed" : trace.metric === "power" ? "dotted" : "solid",
                  }}
                />
                <span className={large ? "min-w-0" : "whitespace-nowrap"}>{trace.label} <span className="font-mono font-normal text-ink-dim">({trace.unit})</span></span>
              </div>
              <div className={large ? "mt-1 font-mono text-sm font-medium text-ink sm:text-lg" : "mt-0.5 font-mono text-xs text-ink"}>
                {trace.latest === null ? "—" : trace.formatLatest(trace.latest)}
              </div>
              {large && <div className="mt-1 text-[10px] text-ink-dim">{t("power.chart.range")} <span className="font-mono">±{trace.formatLatest(trace.maximum)}</span></div>}
            </li>
          ))}
        </ul>
      )}
      {large ? (
        <div
          ref={viewportRef}
          role="region"
          aria-label={t("power.chart.pan")}
          aria-describedby={scrollHintId}
          tabIndex={0}
          className="max-w-full overflow-x-scroll overscroll-x-contain pb-1 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40 [&::-webkit-scrollbar]:h-3"
          style={{ scrollbarWidth: "auto", scrollbarGutter: "stable" }}
        >
          <div data-chart-content style={{ width: `${zoom * 100}%`, minWidth: "100%" }}>
            {graphic}
            <div aria-hidden="true" className="mt-2 flex justify-between gap-2 pb-2 font-mono text-[10px] text-ink-dim">
              {[0, 0.25, 0.5, 0.75, 1].map((ratio) => <span key={ratio}>{ratio === 1 ? t("power.chart.now") : historyOffset(elapsedSeconds * (1 - ratio))}</span>)}
            </div>
          </div>
        </div>
      ) : graphic}
      <div className="mt-1 flex flex-wrap items-center justify-between gap-x-3 gap-y-0.5 text-[10px] text-ink-dim">
        <span>{mode === "power" ? t("power.chart.independentScales") : duration ? `-${duration}s` : ""}</span>
        {mode === "voltage" && <span className="font-mono text-ink">{plotted[0].latest === null ? "—" : plotted[0].formatLatest(plotted[0].latest)}</span>}
        {mode === "power" && duration > 0 && <span>{`${large ? historyOffset(elapsedSeconds) : `-${duration}s`} → ${t("power.chart.now")}`}</span>}
        {noVoltage && <span className="basis-full">{t("power.chart.noVoltage")}</span>}
      </div>
      {large && <p id={scrollHintId} className="mt-3 text-[11px] text-ink-dim">{t("power.chart.scrollHint")}</p>}
    </div>
  );
});
