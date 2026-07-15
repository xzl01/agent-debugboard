import { useMemo, useState } from "react";
import { Activity, BatteryCharging, Clock3, Download, Play, Radio, Square, Trash2, Zap } from "lucide-react";
import { Badge, Button } from "./ui";
import type {
  CaptureConfig,
  CaptureTrigger,
  PowerCapture,
  SafeGpio,
} from "@/lib/types";
import {
  nominalVoltage,
  powerRailLabel,
  USER_POWER_RAILS,
  type PowerMetric,
} from "@/lib/power";
import { useI18n } from "@/lib/i18n";

const COLORS = ["#4f7cff", "#f59e0b", "#22c55e", "#ef4444"];
const WIDTH = 720;
const HEIGHT = 180;

function sampleCurrentA(capture: PowerCapture, sampleIndex: number, rail: string) {
  const reading = capture.samples[sampleIndex]?.readings.find((item) => item.name === rail);
  return reading?.power_enabled ? Math.max(0, reading.current_ua / 1_000_000) : 0;
}

function integrateCapture(capture: PowerCapture, rail: string) {
  const voltage = nominalVoltage(rail) ?? 0;
  let ampHours = 0;
  let wattHours = 0;
  for (let index = 1; index < capture.samples.length; index += 1) {
    const previous = capture.samples[index - 1];
    const current = capture.samples[index];
    const hours = Math.max(0, current.deviceTimeUs - previous.deviceTimeUs) / 3_600_000_000;
    const averageCurrent = (sampleCurrentA(capture, index - 1, rail) + sampleCurrentA(capture, index, rail)) / 2;
    ampHours += averageCurrent * hours;
    wattHours += averageCurrent * voltage * hours;
  }
  const first = capture.samples[0]?.deviceTimeUs ?? 0;
  const last = capture.samples.at(-1)?.deviceTimeUs ?? first;
  return {
    milliampHours: ampHours * 1000,
    wattHours,
    durationMs: Math.max(0, last - first) / 1000,
  };
}

function formatIntegrated(value: number) {
  if (value >= 100) return value.toFixed(1);
  if (value >= 1) return value.toFixed(3);
  if (value >= 0.01) return value.toFixed(4);
  return value.toFixed(6);
}

function download(name: string, content: string, type: string) {
  const url = URL.createObjectURL(new Blob([content], { type }));
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = name;
  anchor.click();
  URL.revokeObjectURL(url);
}

function exportCapture(capture: PowerCapture, format: "csv" | "ndjson") {
  const triggerTime = capture.samples[capture.triggerOffset]?.deviceTimeUs ?? 0;
  const rows = capture.samples.map((sample) => ({
    capture_id: capture.id,
    trigger: capture.trigger,
    source: capture.source,
    edge: capture.edge,
    threshold_ua: capture.thresholdUa,
    rate_hz: capture.rateHz,
    pre_samples: capture.preSamples,
    post_samples: capture.postSamples,
    offset: sample.offset,
    triggered: sample.triggered,
    device_t_mono_us: sample.deviceTimeUs,
    relative_us: sample.deviceTimeUs - triggerTime,
    readings: sample.readings.map((reading) => {
      const voltage = nominalVoltage(reading.name) ?? 0;
      const currentUa = reading.power_enabled ? reading.current_ua : 0;
      return {
        name: reading.name,
        power_enabled: reading.power_enabled,
        current_ua: currentUa,
        power_w: (currentUa / 1_000_000) * voltage,
      };
    }),
  }));
  const base = `linkr-power-capture-${capture.id}`;
  if (format === "ndjson") {
    download(`${base}.ndjson`, rows.map((row) => JSON.stringify(row)).join("\n") + "\n", "application/x-ndjson");
    return;
  }
  const rails = [...USER_POWER_RAILS];
  const header = ["capture_id", "trigger", "source", "edge", "threshold_ua", "rate_hz",
    "pre_samples", "post_samples", "offset", "triggered", "device_t_mono_us", "relative_us",
    ...rails.flatMap((rail) => [`${rail}_current_ua`, `${rail}_power_w`])];
  const lines = rows.map((row) => {
    const values = new Map(row.readings.map((reading) => [reading.name, reading]));
    return [row.capture_id, row.trigger, row.source, row.edge, row.threshold_ua, row.rate_hz,
      row.pre_samples, row.post_samples, row.offset, row.triggered, row.device_t_mono_us, row.relative_us,
      ...rails.flatMap((rail) => [values.get(rail)?.current_ua ?? 0, values.get(rail)?.power_w ?? 0])].join(",");
  });
  download(`${base}.csv`, [header.join(","), ...lines].join("\n") + "\n", "text/csv");
}

export function PowerAnalyzer({
  metric,
  gpios,
  state,
  progress,
  captures,
  onArm,
  onTrigger,
  onCancel,
  onClear,
  capacity,
}: {
  metric: PowerMetric;
  gpios: SafeGpio[];
  state: "idle" | "connecting" | "armed" | "receiving";
  progress: { received: number; total: number } | null;
  captures: PowerCapture[];
  onArm: (config: CaptureConfig) => Promise<void>;
  onTrigger: () => void;
  onCancel: () => void;
  onClear: () => void;
  capacity: number;
}) {
  const { t } = useI18n();
  const [trigger, setTrigger] = useState<CaptureTrigger>("manual");
  const [source, setSource] = useState<string>(USER_POWER_RAILS[0]);
  const [gpio, setGpio] = useState("");
  const [thresholdMa, setThresholdMa] = useState(500);
  const [rateHz, setRateHz] = useState(100);
  const [preSamples, setPreSamples] = useState(100);
  const [postSamples, setPostSamples] = useState(300);
  const [rail, setRail] = useState<string>(USER_POWER_RAILS[0]);

  const series = useMemo(() => captures.map((capture, captureIndex) => {
    const triggerTime = capture.samples[capture.triggerOffset]?.deviceTimeUs ?? 0;
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
  const maxY = Math.max(metric === "current" ? 0.001 : 0.01, ...allPoints.map((point) => point.value)) * 1.1;
  const latestCapture = captures.at(-1);
  const latestSummary = useMemo(
    () => latestCapture ? integrateCapture(latestCapture, rail) : null,
    [latestCapture, rail]
  );

  const arm = () => void onArm({
    trigger,
    source: trigger === "gpio" ? (gpio || gpios[0]?.name || "") : source,
    edge: "either",
    thresholdUa: Math.round(thresholdMa * 1000),
    rateHz: Math.max(1, Math.min(1000, rateHz)),
    preSamples: Math.max(0, preSamples),
    postSamples: Math.max(1, postSamples),
  }).catch(() => {
    // useBoard exposes the actionable error through the shared board status.
  });

  return (
    <details className="mt-4 rounded-xl border border-line/60 bg-panel2/25">
      <summary className="flex cursor-pointer list-none items-center gap-2 px-3 py-2.5 text-sm font-medium text-ink">
        <Activity size={15} className="text-brand" />
        <span className="flex-1">{t("analyzer.title")}</span>
        <Badge tone={state === "idle" ? "neutral" : state === "armed" ? "warn" : "brand"}>
          {t(`analyzer.state.${state}`)}
        </Badge>
      </summary>
      <div className="border-t border-line/60 p-3">
        <div className="grid gap-2 sm:grid-cols-3">
          <label className="text-[11px] text-ink-dim">{t("analyzer.trigger")}
            <select value={trigger} onChange={(event) => setTrigger(event.target.value as CaptureTrigger)} disabled={state !== "idle"}
              className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-2 text-xs text-ink">
              <option value="manual">{t("analyzer.trigger.manual")}</option>
              <option value="current">{t("analyzer.trigger.current")}</option>
              <option value="power_on">{t("analyzer.trigger.powerOn")}</option>
              <option value="gpio">{t("analyzer.trigger.gpio")}</option>
            </select>
          </label>
          {trigger === "gpio" ? (
            <label className="text-[11px] text-ink-dim">GPIO
              <select value={gpio} onChange={(event) => setGpio(event.target.value)} disabled={state !== "idle"}
                className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-2 text-xs text-ink">
                {gpios.map((item) => <option key={item.name} value={item.name}>{item.name} · {item.note}</option>)}
              </select>
            </label>
          ) : (
            <label className="text-[11px] text-ink-dim">{t("analyzer.source")}
              <select value={source} onChange={(event) => setSource(event.target.value)} disabled={state !== "idle"}
                className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-2 text-xs text-ink">
                {USER_POWER_RAILS.map((name) => <option key={name} value={name}>{powerRailLabel(name)}</option>)}
              </select>
            </label>
          )}
          <label className="text-[11px] text-ink-dim">{t("analyzer.rate")}
            <input type="number" min="1" max="1000" value={rateHz} onChange={(event) => setRateHz(Number(event.target.value))} disabled={state !== "idle"}
              className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-2 text-xs text-ink" />
          </label>
          {trigger === "current" && <label className="text-[11px] text-ink-dim">{t("analyzer.threshold")}
            <input type="number" min="0" value={thresholdMa} onChange={(event) => setThresholdMa(Number(event.target.value))} disabled={state !== "idle"}
              className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-2 text-xs text-ink" />
          </label>}
          <label className="text-[11px] text-ink-dim">{t("analyzer.pre")}
            <input type="number" min="0" max={capacity - 1} value={preSamples} onChange={(event) => setPreSamples(Number(event.target.value))} disabled={state !== "idle"}
              className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-2 text-xs text-ink" />
          </label>
          <label className="text-[11px] text-ink-dim">{t("analyzer.post")}
            <input type="number" min="1" max={capacity - 1} value={postSamples} onChange={(event) => setPostSamples(Number(event.target.value))} disabled={state !== "idle"}
              className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-2 text-xs text-ink" />
          </label>
        </div>
        <div className="mt-3 flex flex-wrap gap-2">
          {state === "idle" ? <Button variant="primary" onClick={arm} disabled={preSamples + postSamples + 1 > capacity || (trigger === "gpio" && gpios.length === 0)}><Radio size={15} />{t("analyzer.arm")}</Button> : <Button onClick={onCancel}><Square size={15} />{t("analyzer.cancel")}</Button>}
          {state === "armed" && trigger === "manual" && <Button variant="primary" onClick={onTrigger}><Play size={15} />{t("analyzer.fire")}</Button>}
          {progress && <span className="self-center text-xs text-ink-dim">{progress.received}/{progress.total}</span>}
        </div>

        {captures.length > 0 && <div className="mt-4">
          <div className="mb-2 flex flex-wrap items-center gap-2">
            <select value={rail} onChange={(event) => setRail(event.target.value)} className="rounded-lg border border-line bg-panel px-2 py-1.5 text-xs text-ink">
              {USER_POWER_RAILS.map((name) => <option key={name} value={name}>{powerRailLabel(name)}</option>)}
            </select>
            {captures.map((capture, index) => <span key={capture.id} className="text-[10px] text-ink-dim"><i className="mr-1 inline-block h-2 w-2 rounded-full" style={{ background: COLORS[index % COLORS.length] }} />#{capture.id}</span>)}
            <span className="flex-1" />
            <Button variant="ghost" className="min-h-8 px-2 py-1 text-xs" onClick={() => exportCapture(captures.at(-1)!, "csv")}><Download size={13} />CSV</Button>
            <Button variant="ghost" className="min-h-8 px-2 py-1 text-xs" onClick={() => exportCapture(captures.at(-1)!, "ndjson")}><Download size={13} />NDJSON</Button>
            <Button variant="ghost" className="min-h-8 px-2 py-1" onClick={onClear} aria-label={t("analyzer.clear")}><Trash2 size={13} /></Button>
          </div>
          {latestCapture && latestSummary && (
            <div className="mb-3 grid gap-2 rounded-lg border border-line/60 bg-panel/70 p-2.5 sm:grid-cols-3">
              <div className="min-w-0">
                <div className="flex items-center gap-1.5 text-[10px] text-ink-dim"><BatteryCharging size={12} />{t("analyzer.charge")}</div>
                <div className="mt-0.5 font-mono text-sm font-semibold text-brand">{formatIntegrated(latestSummary.milliampHours)} mAh</div>
              </div>
              <div className="min-w-0">
                <div className="flex items-center gap-1.5 text-[10px] text-ink-dim"><Zap size={12} />{t("analyzer.energy")}</div>
                <div className="mt-0.5 font-mono text-sm font-semibold text-warn">{formatIntegrated(latestSummary.wattHours)} Wh</div>
              </div>
              <div className="min-w-0">
                <div className="flex items-center gap-1.5 text-[10px] text-ink-dim"><Clock3 size={12} />{t("analyzer.duration")}</div>
                <div className="mt-0.5 font-mono text-sm font-semibold text-ink">{(latestSummary.durationMs / 1000).toFixed(3)} s</div>
              </div>
              <div className="text-[9px] text-ink-dim sm:col-span-3">
                {t("analyzer.latestCapture").replaceAll("{id}", String(latestCapture.id))} · {powerRailLabel(rail)}
              </div>
            </div>
          )}
          <svg viewBox={`0 0 ${WIDTH} ${HEIGHT}`} preserveAspectRatio="none" className="h-44 w-full rounded-lg border border-line/60 bg-panel">
            {[0.25, 0.5, 0.75].map((ratio) => <line key={ratio} x1="0" x2={WIDTH} y1={HEIGHT * ratio} y2={HEIGHT * ratio} stroke="rgb(var(--c-line))" strokeDasharray="3 5" />)}
            <line x1={((-minX) / (maxX - minX)) * WIDTH} x2={((-minX) / (maxX - minX)) * WIDTH} y1="0" y2={HEIGHT} stroke="rgb(var(--c-danger))" strokeDasharray="4 3" />
            {series.map(({ capture, color, points }) => <polyline key={capture.id} fill="none" stroke={color} strokeWidth="1.7" vectorEffect="non-scaling-stroke"
              points={points.map((point) => `${((point.x - minX) / (maxX - minX)) * WIDTH},${HEIGHT - (point.value / maxY) * (HEIGHT - 8) - 4}`).join(" ")} />)}
          </svg>
          <div className="mt-1 flex justify-between text-[9px] text-ink-dim"><span>{minX.toFixed(0)} ms</span><span>{t("analyzer.triggerAt")}</span><span>{maxX.toFixed(0)} ms</span></div>
        </div>}
      </div>
    </details>
  );
}
