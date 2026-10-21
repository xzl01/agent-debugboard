import type { AdcReading, CurrentAdcReading, PowerOutput } from "./types";

export type PowerMetric = "current" | "voltage" | "power";

export const USER_POWER_RAILS = ["5v_out", "12v_out", "20v_out"] as const;

export interface PowerRailRow {
  readonly name: string;
  readonly output?: PowerOutput;
  readonly reading?: CurrentAdcReading;
}

/** Outputs retain firmware order; measurement-only rails follow in ADC order. */
export function powerRailRows(outputs: readonly PowerOutput[], readings: readonly AdcReading[]): PowerRailRow[] {
  const rows = new Map<string, PowerRailRow>();
  for (const output of outputs) {
    if (!rows.has(output.name)) rows.set(output.name, { name: output.name, output });
  }
  for (const reading of readings) {
    if (reading.kind !== "current") continue;
    const row = rows.get(reading.name);
    if (!row?.reading) rows.set(reading.name, { ...row, name: reading.name, reading });
  }
  return [...rows.values()];
}

export function powerRailCatalog(outputs: readonly PowerOutput[], readings: readonly AdcReading[]) {
  const unique = (names: string[]) => [...new Set(names)];
  return {
    switchable: unique(outputs.filter((output) => output.controllable).map((output) => output.name)),
    current: unique(readings.filter((reading) => reading.kind === "current").map((reading) => reading.name)),
    measurements: unique(readings.map((reading) => reading.name)),
  };
}

// Live status polling runs every 10 s; three missed power snapshots make control unsafe.
export const POWER_STATE_STALE_MS = 30_000;

export function powerOutputIsOn(output?: PowerOutput): boolean {
  return output?.state === "on" || output?.value === 1;
}

export function powerRailLabel(name: string): string {
  const voltage = nominalVoltage(name);
  if (voltage == null) return name;
  const suffix = name.replace(/^\d+(?:\.\d+)?v_?/iu, "");
  return !suffix || suffix.toLowerCase() === "out" ? `${voltage}V` : `${voltage}V · ${suffix.replaceAll("_", " ")}`;
}

export function nominalVoltage(name: string): number | null {
  const match = name.match(/^(\d+(?:\.\d+)?)v(?:_|$)/i);
  return match ? Number(match[1]) : null;
}

export function readingMetric(reading: CurrentAdcReading, metric: PowerMetric): number | null {
  if (metric === "voltage") return measuredVoltage(reading);
  // Rail enable state is status only; it must not override sensor measurements.
  const current = reading.value / 1_000_000;
  if (metric === "current") return current;
  const voltage = measuredVoltage(reading);
  return voltage == null ? null : current * voltage;
}

export function measuredVoltage(reading: CurrentAdcReading): number | null {
  if (reading.voltage_valid === true && reading.voltage_uv != null && Number.isFinite(reading.voltage_uv)) {
    return reading.voltage_uv / 1_000_000;
  }
  return null;
}

export function formatPowerMetric(value: number, metric: PowerMetric): string {
  if (metric === "voltage") return `${value.toFixed(3)} V`;
  if (metric === "power") {
    if (Math.abs(value) < 0.01) return `${(value * 1000).toFixed(1)} mW`;
    return `${value.toFixed(2)} W`;
  }
  if (Math.abs(value) < 0.01) return `${(value * 1000).toFixed(1)} mA`;
  return `${value.toFixed(3)} A`;
}
