import type { CurrentAdcReading, PowerOutput } from "./types";

export type PowerMetric = "current" | "voltage" | "power";

export const USER_POWER_RAILS = ["5v_out", "12v_out", "20v_out"] as const;

// Power rails that clients may switch on/off. vdd_5v has INA226 telemetry too,
// but USER_POWER_RAILS remains the three target-facing comparison/capture rails.
export const POWER_SWITCH_RAILS = [...USER_POWER_RAILS, "vdd_5v"] as const;

export function powerOutputIsOn(output?: PowerOutput): boolean {
  return output?.state === "on" || output?.value === 1;
}

export function powerRailLabel(name: string): string {
  const voltage = nominalVoltage(name);
  return voltage == null ? name : `${voltage}V`;
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
