import { useEffect, useRef, useState } from "react";
import { Activity, ChartLine, Clock3, List, Power, Zap } from "lucide-react";
import { Badge, Card, Toggle } from "./ui";
import type { AdcReading, PowerOutput } from "@/lib/types";
import { useI18n } from "@/lib/i18n";
import { cn } from "@/lib/utils";
import { formatUptime } from "@/lib/utils";
import {
  formatPowerMetric,
  powerRailLabel,
  readingMetric,
  USER_POWER_RAILS,
  type PowerMetric,
} from "@/lib/power";
import { PowerSparkline } from "./PowerSparkline";

const POWER_TRENDS_STORAGE_KEY = "linkr-power-trends-expanded";

function initialTrendsExpanded() {
  if (typeof localStorage === "undefined") return false;
  return localStorage.getItem(POWER_TRENDS_STORAGE_KEY) === "true";
}

export function PowerCard({
  outputs,
  readings,
  onSet,
}: {
  outputs: PowerOutput[];
  readings: AdcReading[];
  onSet: (name: string, on: boolean) => void;
}) {
  const { t } = useI18n();
  const [metric, setMetric] = useState<PowerMetric>("current");
  const [trendsExpanded, setTrendsExpanded] = useState(initialTrendsExpanded);
  const [clockMs, setClockMs] = useState(() => Date.now());
  const railTimersRef = useRef(new Map<string, { startedAtMs: number; approximate: boolean }>());
  const observedRailsRef = useRef(new Set<string>());
  const rows = USER_POWER_RAILS.map((name) => ({
    name,
    output: outputs.find((output) => output.name === name),
    reading: readings.find((reading) => reading.name === name),
  })).filter(({ output, reading }) => output || reading);

  useEffect(() => {
    const now = Date.now();
    for (const name of USER_POWER_RAILS) {
      const output = outputs.find((item) => item.name === name);
      if (!output) continue;
      const firstObservation = !observedRailsRef.current.has(name);
      observedRailsRef.current.add(name);
      if (output.state === "on") {
        if (!railTimersRef.current.has(name)) {
          railTimersRef.current.set(name, { startedAtMs: now, approximate: firstObservation });
        }
      } else {
        railTimersRef.current.delete(name);
      }
    }
    setClockMs(now);
  }, [outputs]);

  useEffect(() => {
    const timer = window.setInterval(() => {
      if (railTimersRef.current.size > 0) setClockMs(Date.now());
    }, 1000);
    return () => window.clearInterval(timer);
  }, []);

  useEffect(() => {
    localStorage.setItem(POWER_TRENDS_STORAGE_KEY, String(trendsExpanded));
  }, [trendsExpanded]);

  return (
    <Card
      title={t("power.combined.title")}
      subtitle={t("power.combined.subtitle")}
      icon={Power}
      right={
        <div className="flex max-w-full flex-wrap items-center justify-end gap-1.5">
          {trendsExpanded && (
            <div
              role="tablist"
              aria-label={t("power.chart.metric")}
              className="grid grid-cols-2 rounded-lg border border-line/70 bg-panel2/60 p-0.5"
            >
              {(["current", "power"] as const).map((item) => (
                <button
                  key={item}
                  type="button"
                  role="tab"
                  aria-selected={metric === item}
                  onClick={() => setMetric(item)}
                  className={cn(
                    "inline-flex min-h-8 items-center gap-1 rounded-md px-2 text-xs font-medium transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40",
                    metric === item ? "bg-brand text-white" : "text-ink-dim hover:text-ink"
                  )}
                >
                  {item === "current" ? <Activity size={12} /> : <Zap size={12} />}
                  {t(`power.chart.${item}`)}
                </button>
              ))}
            </div>
          )}
          <button
            type="button"
            aria-pressed={trendsExpanded}
            onClick={() => setTrendsExpanded((expanded) => !expanded)}
            className="inline-flex min-h-8 cursor-pointer items-center gap-1.5 rounded-lg border border-line/70 bg-panel2/60 px-2.5 text-xs font-medium text-ink-dim transition-colors hover:border-brand/30 hover:text-ink focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-brand/40"
          >
            {trendsExpanded ? <List size={13} /> : <ChartLine size={13} />}
            {t(trendsExpanded ? "power.chart.compact" : "power.chart.expand")}
          </button>
        </div>
      }
    >
      {rows.length === 0 ? (
        <p className="text-sm text-ink-dim">{t("power.combined.none")}</p>
      ) : (
        <>
          <ul className="divide-y divide-line/50">
            {rows.map(({ name, output, reading }) => {
              const on = output?.state === "on";
              const locked = output ? !output.controllable || output.state === "locked" : false;
              const currentValue = reading ? readingMetric(reading, "current") : null;
              const powerValue = reading ? readingMetric(reading, "power") : null;
              const onTiming = on ? railTimersRef.current.get(name) : undefined;
              return (
                <li
                  key={name}
                  className={cn(
                    "grid grid-cols-[minmax(0,1fr)_auto] gap-x-3 gap-y-1",
                    trendsExpanded ? "py-3" : "py-2.5"
                  )}
                >
                <div className="min-w-0">
                  <div className="flex flex-wrap items-center gap-2">
                    <span className="font-medium text-ink">{powerRailLabel(name)}</span>
                    {locked && <Badge tone="warn">{t("power.locked")}</Badge>}
                    {!output && <Badge tone="neutral">{t("power.monitorOnly")}</Badge>}
                  </div>
                  <div className="text-xs text-ink-dim">
                    {output?.signal ?? reading?.signal ?? `GP${output?.gp ?? "?"}`}
                    {output?.value != null && !locked && (
                      <span className="ml-2 text-ink-dim">
                        {on ? t("power.on") : t("power.off")} · {output.value}
                      </span>
                    )}
                  </div>
                  {onTiming && (
                    <div
                      className="mt-1 inline-flex items-center gap-1 text-[10px] text-ink-dim"
                      title={t("power.onDurationHint")}
                    >
                      <Clock3 size={11} />
                      {t("power.onDuration")} {onTiming.approximate ? "≥ " : ""}{formatUptime(Math.floor((clockMs - onTiming.startedAtMs) / 1000))}
                    </div>
                  )}
                </div>
                <div className="flex items-center gap-3">
                  <div className="min-w-[82px] text-right">
                    <div className="inline-flex items-center gap-1 font-mono text-sm font-medium text-brand">
                      <Activity size={13} />
                      {currentValue == null ? "—" : formatPowerMetric(currentValue, "current")}
                    </div>
                    <div className="flex items-center justify-end gap-1 font-mono text-[11px] font-medium text-warn">
                      <Zap size={11} />
                      {powerValue == null ? "—" : formatPowerMetric(powerValue, "power")}
                    </div>
                  </div>
                  {output && (
                    <Toggle
                      checked={on}
                      disabled={locked}
                      onChange={(value) => onSet(output.name, value)}
                    />
                  )}
                </div>
                {reading && !reading.power_enabled && (
                  <div className="col-span-2 text-[11px] text-warn">{t("adc.disabled")}</div>
                )}
                {reading && trendsExpanded && (
                  <div className="col-span-2 mt-1">
                    <PowerSparkline reading={reading} metric={metric} />
                  </div>
                )}
                </li>
              );
            })}
          </ul>
        </>
      )}
    </Card>
  );
}
