import type { AdcReading } from "./types";

export type PowerMetric = "current" | "power";

export const USER_POWER_RAILS = ["5v_out", "12v_out", "20v_out"] as const;
// Power-capture ring exported by every supported RP235x firmware target.
export const POWER_CAPTURE_SAMPLE_CAPACITY = 2048;

export function powerRailLabel(name: string): string {
  const voltage = nominalVoltage(name);
  return voltage == null ? name : `${voltage}V`;
}

export function nominalVoltage(name: string): number | null {
  const match = name.match(/^(\d+(?:\.\d+)?)v(?:_|$)/i);
  return match ? Number(match[1]) : null;
}

export function readingMetric(reading: AdcReading, metric: PowerMetric): number | null {
  if (!reading.power_enabled) return 0;
  const current = Math.max(0, reading.current_ua / 1_000_000);
  if (metric === "current") return current;
  const voltage = nominalVoltage(reading.name);
  return voltage == null ? null : current * voltage;
}

export function formatPowerMetric(value: number, metric: PowerMetric): string {
  if (metric === "power") {
    if (value < 0.01) return `${(value * 1000).toFixed(1)} mW`;
    return `${value.toFixed(2)} W`;
  }
  if (value < 0.01) return `${(value * 1000).toFixed(1)} mA`;
  return `${value.toFixed(3)} A`;
}
