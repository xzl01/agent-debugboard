import type { PowerMetric } from "@/lib/power";

export interface PowerChartAxisScale {
  maximum: number;
  unit: "A" | "mA" | "µA" | "W" | "mW" | "µW";
  multiplier: number;
  ticks: number[];
}

function niceMaximum(value: number): number {
  if (!Number.isFinite(value) || value <= 0) return 1;
  const exponent = 10 ** Math.floor(Math.log10(value));
  const fraction = value / exponent;
  const niceFraction = fraction <= 1 ? 1
    : fraction <= 2 ? 2
      : fraction <= 4 ? 4
        : fraction <= 5 ? 5
          : 10;
  return niceFraction * exponent;
}

export function buildPowerChartAxis(
  metric: PowerMetric,
  observedMaximum: number,
): PowerChartAxisScale {
  const minimum = metric === "current" ? 0.001 : 0.01;
  const maximum = niceMaximum(Math.max(minimum, observedMaximum) * 1.1);
  const [unit, multiplier] = metric === "current"
    ? maximum >= 1
      ? ["A", 1] as const
      : maximum >= 0.001
        ? ["mA", 1_000] as const
        : ["µA", 1_000_000] as const
    : maximum >= 1
      ? ["W", 1] as const
      : maximum >= 0.001
        ? ["mW", 1_000] as const
        : ["µW", 1_000_000] as const;

  return {
    maximum,
    unit,
    multiplier,
    ticks: [1, 0.75, 0.5, 0.25, 0].map((ratio) => maximum * ratio),
  };
}

export function formatPowerChartTick(value: number, multiplier: number): string {
  const scaled = value * multiplier;
  if (scaled === 0) return "0";
  const digits = Math.abs(scaled) >= 100 ? 0 : Math.abs(scaled) >= 10 ? 1 : 2;
  const formatted = scaled.toFixed(digits);
  return formatted.includes(".") ? formatted.replace(/0+$/u, "").replace(/\.$/u, "") : formatted;
}

export function PowerChartYAxis({
  axis,
  className = "h-44",
}: {
  axis: PowerChartAxisScale;
  className?: string;
}) {
  return (
    <div
      aria-hidden="true"
      className={`flex min-w-12 flex-col justify-between py-1 text-right font-mono text-[9px] tabular-nums text-ink-dim ${className}`}
    >
      {axis.ticks.map((tick, index) => (
        <span key={tick} className="inline-flex items-baseline justify-end gap-0.5 whitespace-nowrap">
          {formatPowerChartTick(tick, axis.multiplier)}
          {index === 0 && <span className="font-sans text-[8px] font-medium">{axis.unit}</span>}
        </span>
      ))}
    </div>
  );
}
