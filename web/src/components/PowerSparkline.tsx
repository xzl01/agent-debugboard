import { useEffect, useMemo, useState } from "react";
import type { AdcReading } from "@/lib/types";
import {
  formatPowerMetric,
  powerRailLabel,
  readingMetric,
  type PowerMetric,
} from "@/lib/power";
import { useI18n } from "@/lib/i18n";

type Sample = { timestamp: number; current: number; power: number | null };

const MAX_SAMPLES = 90;
const WIDTH = 320;
const HEIGHT = 58;

export function PowerSparkline({
  reading,
  metric,
}: {
  reading: AdcReading;
  metric: PowerMetric;
}) {
  const { t } = useI18n();
  const [history, setHistory] = useState<Sample[]>([]);

  useEffect(() => {
    setHistory((previous) => [
      ...previous,
      {
        timestamp: Date.now(),
        current: readingMetric(reading, "current") ?? 0,
        power: readingMetric(reading, "power"),
      },
    ].slice(-MAX_SAMPLES));
  }, [reading]);

  const values = useMemo(
    () => history.flatMap((sample) => (sample[metric] == null ? [] : [sample[metric]])),
    [history, metric]
  );
  if (values.length === 0) return null;

  const max = Math.max(metric === "current" ? 0.001 : 0.01, ...values) * 1.15;
  const points = values
    .map((value, index) => {
      const x = values.length === 1 ? WIDTH : (index / (values.length - 1)) * WIDTH;
      const y = HEIGHT - 4 - (value / max) * (HEIGHT - 8);
      return `${x},${y}`;
    })
    .join(" ");
  const latest = values.at(-1)!;
  const latestX = values.length === 1 ? WIDTH : WIDTH;
  const latestY = HEIGHT - 4 - (latest / max) * (HEIGHT - 8);
  const duration =
    history.length > 1
      ? Math.max(1, Math.round((history.at(-1)!.timestamp - history[0].timestamp) / 1000))
      : 0;
  const color = metric === "current" ? "rgb(var(--c-brand))" : "rgb(var(--c-warn))";

  return (
    <div className="relative overflow-hidden rounded-lg border border-line/50 bg-panel2/35 px-2 py-1.5">
      <svg
        viewBox={`0 0 ${WIDTH} ${HEIGHT}`}
        preserveAspectRatio="none"
        role="img"
        aria-label={`${powerRailLabel(reading.name)} · ${t(`power.chart.${metric}`)} · ${t("power.chart.trend")}`}
        className="block h-12 w-full"
      >
        {[0.33, 0.66].map((ratio) => (
          <line
            key={ratio}
            x1="0"
            x2={WIDTH}
            y1={HEIGHT * ratio}
            y2={HEIGHT * ratio}
            stroke="rgb(var(--c-line))"
            strokeOpacity="0.55"
            strokeDasharray="3 5"
          />
        ))}
        <polyline
          points={points}
          fill="none"
          stroke={color}
          strokeWidth="2"
          strokeLinecap="round"
          strokeLinejoin="round"
          vectorEffect="non-scaling-stroke"
        />
        <circle cx={latestX} cy={latestY} r="2.5" fill={color} />
      </svg>
      <div className="pointer-events-none absolute inset-x-2 bottom-1 flex justify-between text-[9px] text-ink-dim">
        <span>{duration ? `-${duration}s` : ""}</span>
        <span className="font-mono text-ink">{formatPowerMetric(latest, metric)}</span>
      </div>
    </div>
  );
}
