import { useEffect, useState } from "react";
import type { CurrentAdcReading, VoltageAdcReading } from "@/lib/types";
import { appendMeasurementHistory } from "@/lib/adc";
import {
  formatPowerMetric,
  powerRailLabel,
  readingMetric,
  type PowerMetric,
} from "@/lib/power";
import { useI18n } from "@/lib/i18n";

type MeasurementSample = {
  readonly timestampMs: number;
  readonly current: number | null;
  readonly power: number | null;
  readonly voltageUv: number | null;
};

type MeasurementSparklineProps =
  | {
      readonly mode: "power";
      readonly reading: CurrentAdcReading;
      readonly metric: PowerMetric;
    }
  | {
      readonly mode: "voltage";
      readonly reading: VoltageAdcReading;
    };

type ChartPresentation = {
  readonly values: readonly number[];
  readonly minimumScale: number;
  readonly maximumScale?: number;
  readonly color: string;
  readonly ariaLabel: string;
  readonly formatLatest: (value: number) => string;
};

const WIDTH = 320;
const HEIGHT = 58;
const ADC3_MAX_UV = 3_300_000;

function measurementSample(
  props: MeasurementSparklineProps,
  timestampMs: number,
): MeasurementSample {
  switch (props.mode) {
    case "power":
      return {
        timestampMs,
        current: readingMetric(props.reading, "current"),
        power: readingMetric(props.reading, "power"),
        voltageUv: null,
      };
    case "voltage":
      return {
        timestampMs,
        current: null,
        power: null,
        voltageUv: props.reading.value,
      };
    default: {
      const exhaustive: never = props;
      return exhaustive;
    }
  }
}

function chartPresentation(
  props: MeasurementSparklineProps,
  history: readonly MeasurementSample[],
  translate: (key: string) => string,
): ChartPresentation {
  switch (props.mode) {
    case "power":
      return {
        values: history.flatMap((sample) => {
          const value = sample[props.metric];
          return value === null ? [] : [value];
        }),
        minimumScale: props.metric === "current" ? 0.001 : 0.01,
        color: props.metric === "current"
          ? "rgb(var(--c-brand))"
          : "rgb(var(--c-warn))",
        ariaLabel: `${powerRailLabel(props.reading.name)} · ${translate(`power.chart.${props.metric}`)} · ${translate("power.chart.trend")}`,
        formatLatest: (value) => formatPowerMetric(value, props.metric),
      };
    case "voltage":
      return {
        values: history.flatMap((sample) => sample.voltageUv === null ? [] : [sample.voltageUv]),
        minimumScale: ADC3_MAX_UV,
        maximumScale: ADC3_MAX_UV,
        color: "rgb(var(--c-brand))",
        ariaLabel: translate("adc3.chart.aria"),
        formatLatest: (value) => `${(value / 1_000_000).toFixed(3)} V`,
      };
    default: {
      const exhaustive: never = props;
      return exhaustive;
    }
  }
}

export function MeasurementSparkline(props: MeasurementSparklineProps) {
  const { t } = useI18n();
  const [history, setHistory] = useState<readonly MeasurementSample[]>([]);

  useEffect(() => {
    setHistory((previous) => appendMeasurementHistory(
      previous,
      measurementSample(props, Date.now()),
    ));
  }, [props.mode, props.reading]);

  const presentation = chartPresentation(props, history, t);
  if (presentation.values.length === 0) return null;

  const maximum = presentation.maximumScale ??
    Math.max(presentation.minimumScale, ...presentation.values) * 1.15;
  const points = presentation.values
    .map((value, index) => {
      const x = presentation.values.length === 1
        ? WIDTH
        : (index / (presentation.values.length - 1)) * WIDTH;
      const plottedValue = Math.min(maximum, Math.max(0, value));
      const y = HEIGHT - 4 - (plottedValue / maximum) * (HEIGHT - 8);
      return `${x},${y}`;
    })
    .join(" ");
  const latest = presentation.values.at(-1);
  const firstSample = history[0];
  const latestSample = history.at(-1);
  if (latest === undefined) return null;
  const latestY = HEIGHT - 4 -
    (Math.min(maximum, Math.max(0, latest)) / maximum) * (HEIGHT - 8);
  const duration = firstSample && latestSample && history.length > 1
    ? Math.max(1, Math.round((latestSample.timestampMs - firstSample.timestampMs) / 1000))
    : 0;

  return (
    <div className="relative overflow-hidden rounded-lg border border-line/50 bg-panel2/35 px-2 py-1.5">
      <svg
        viewBox={`0 0 ${WIDTH} ${HEIGHT}`}
        preserveAspectRatio="none"
        role="img"
        aria-label={presentation.ariaLabel}
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
          stroke={presentation.color}
          strokeWidth="2"
          strokeLinecap="round"
          strokeLinejoin="round"
          vectorEffect="non-scaling-stroke"
        />
        <circle cx={WIDTH} cy={latestY} r="2.5" fill={presentation.color} />
      </svg>
      <div className="pointer-events-none absolute inset-x-2 bottom-1 flex justify-between text-[11px] text-ink-dim">
        <span>{duration ? `-${duration}s` : ""}</span>
        <span className="font-mono text-ink">{presentation.formatLatest(latest)}</span>
      </div>
    </div>
  );
}
