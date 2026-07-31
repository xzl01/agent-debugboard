import { nominalVoltage } from "./power.ts";
import type {
  CaptureSample,
  PowerCapture,
  PowerCaptureAggregate,
} from "./types";

export interface CaptureWindowSummary {
  rateHz: number;
  intervalMs: number;
  preSamples: number;
  postSamples: number;
  preDurationMs: number;
  postDurationMs: number;
  totalDurationMs: number;
  totalSamples: number;
  overCapacity: boolean;
}

export type PowerCaptureSummary = PowerCaptureAggregate;

export const POWER_CAPTURE_PROTOCOL = "host-stream-v1";

// Pre-trigger history is held as rich JavaScript objects until the trigger
// arrives. Keep it bounded independently from the hours-long post-trigger
// stream, which is persisted to IndexedDB in compact binary chunks.
export const MAX_POWER_CAPTURE_PRE_TRIGGER_SAMPLES = 60_000;

interface RailAccumulator {
  firstTimeUs: number | null;
  lastTimeUs: number | null;
  previousTimeUs: number | null;
  previousCurrentA: number;
  ampHours: number;
  peakCurrentA: number;
}

export interface PowerCaptureAccumulator {
  rails: Record<string, RailAccumulator>;
}

export interface FiveVoltBatteryEstimate {
  efficiency: number;
  availableEnergyWh: number;
  equivalentCapacityMah: number;
  rechargeInputWh: number;
  runtimeHours: number | null;
  requiredCapacityMah: number | null;
  repeatCount: number | null;
}

function clampFinite(value: number, minimum: number, maximum: number): number {
  if (!Number.isFinite(value)) return minimum;
  return Math.min(maximum, Math.max(minimum, value));
}

export function powerCapturePreTriggerLimitSeconds(rateHz: number): number {
  const normalizedRateHz = Math.round(clampFinite(rateHz, 1, 1000));
  return MAX_POWER_CAPTURE_PRE_TRIGGER_SAMPLES / normalizedRateHz;
}

export function powerCapturePreTriggerSamples(
  rateHz: number,
  durationSeconds: number,
): number {
  const normalizedRateHz = Math.round(clampFinite(rateHz, 1, 1000));
  return Math.min(
    MAX_POWER_CAPTURE_PRE_TRIGGER_SAMPLES,
    Math.max(0, Math.round(clampFinite(
      durationSeconds,
      0,
      powerCapturePreTriggerLimitSeconds(normalizedRateHz),
    ) * normalizedRateHz)),
  );
}

function sampleCurrentA(capture: PowerCapture, sampleIndex: number, rail: string): number {
  const reading = capture.samples[sampleIndex]?.readings.find((item) => item.name === rail);
  return reading?.power_enabled ? Math.max(0, reading.current_ua / 1_000_000) : 0;
}

function readingCurrentA(sample: CaptureSample, rail: string): number {
  const reading = sample.readings.find((item) => item.name === rail);
  return reading?.power_enabled ? Math.max(0, reading.current_ua / 1_000_000) : 0;
}

export function createPowerCaptureAccumulator(): PowerCaptureAccumulator {
  return { rails: {} };
}

export function appendPowerCaptureSummary(
  accumulator: PowerCaptureAccumulator,
  samples: CaptureSample[],
): void {
  for (const sample of samples) {
    for (const reading of sample.readings) {
      const rail = accumulator.rails[reading.name] ?? {
        firstTimeUs: null,
        lastTimeUs: null,
        previousTimeUs: null,
        previousCurrentA: 0,
        ampHours: 0,
        peakCurrentA: 0,
      };
      const currentA = reading.power_enabled
        ? Math.max(0, reading.current_ua / 1_000_000)
        : 0;
      if (rail.firstTimeUs == null) rail.firstTimeUs = sample.deviceTimeUs;
      if (rail.previousTimeUs != null && sample.deviceTimeUs >= rail.previousTimeUs) {
        const hours = (sample.deviceTimeUs - rail.previousTimeUs) / 3_600_000_000;
        rail.ampHours += (rail.previousCurrentA + currentA) / 2 * hours;
      }
      rail.lastTimeUs = sample.deviceTimeUs;
      rail.previousTimeUs = sample.deviceTimeUs;
      rail.previousCurrentA = currentA;
      rail.peakCurrentA = Math.max(rail.peakCurrentA, currentA);
      accumulator.rails[reading.name] = rail;
    }
  }
}

export function finalizePowerCaptureSummaries(
  accumulator: PowerCaptureAccumulator,
): Record<string, PowerCaptureSummary> {
  return Object.fromEntries(Object.entries(accumulator.rails).map(([rail, value]) => {
    const voltage = nominalVoltage(rail) ?? 0;
    const durationMs = value.firstTimeUs == null || value.lastTimeUs == null
      ? 0
      : Math.max(0, value.lastTimeUs - value.firstTimeUs) / 1000;
    const durationHours = durationMs / 3_600_000;
    const averageCurrentA = durationHours > 0 ? value.ampHours / durationHours : 0;
    return [rail, {
      nominalVoltageV: voltage,
      durationMs,
      averageCurrentA,
      peakCurrentA: value.peakCurrentA,
      averagePowerW: averageCurrentA * voltage,
      peakPowerW: value.peakCurrentA * voltage,
      milliampHours: value.ampHours * 1000,
      wattHours: value.ampHours * voltage,
    }];
  }));
}

export function appendPowerCapturePreview(
  preview: CaptureSample[],
  samples: CaptureSample[],
  stride: number,
  maximum = 3000,
): number {
  let nextStride = Math.max(1, stride);
  for (const sample of samples) {
    if (sample.triggered || sample.offset % nextStride === 0) preview.push(sample);
  }
  while (preview.length > maximum) {
    nextStride *= 2;
    const compacted = preview.filter(
      (sample) => sample.triggered || sample.offset % nextStride === 0,
    );
    preview.splice(0, preview.length, ...compacted);
  }
  return nextStride;
}

export function calculateCaptureWindow(
  rateHz: number,
  preSamples: number,
  postSamples: number,
  capacity: number,
): CaptureWindowSummary {
  const normalizedRateHz = Math.round(clampFinite(rateHz, 1, 1000));
  const normalizedPreSamples = Math.floor(clampFinite(preSamples, 0, Number.MAX_SAFE_INTEGER));
  const normalizedPostSamples = Math.floor(clampFinite(postSamples, 1, Number.MAX_SAFE_INTEGER));
  const totalSamples = normalizedPreSamples + normalizedPostSamples + 1;

  return {
    rateHz: normalizedRateHz,
    intervalMs: 1000 / normalizedRateHz,
    preSamples: normalizedPreSamples,
    postSamples: normalizedPostSamples,
    preDurationMs: normalizedPreSamples * 1000 / normalizedRateHz,
    postDurationMs: normalizedPostSamples * 1000 / normalizedRateHz,
    totalDurationMs: (normalizedPreSamples + normalizedPostSamples) * 1000 / normalizedRateHz,
    totalSamples,
    overCapacity: totalSamples > Math.max(1, capacity),
  };
}

export function calculateTimedCaptureWindow(
  rateHz: number,
  preDurationSeconds: number,
  recordDurationSeconds: number,
  capacity: number,
): CaptureWindowSummary {
  const normalizedRateHz = Math.round(clampFinite(rateHz, 1, 1000));
  const preSamples = powerCapturePreTriggerSamples(normalizedRateHz, preDurationSeconds);
  const postSamples = Math.max(1, Math.round(
    clampFinite(recordDurationSeconds, 0, Number.MAX_SAFE_INTEGER) * normalizedRateHz,
  ));
  return calculateCaptureWindow(normalizedRateHz, preSamples, postSamples, capacity);
}

export function calculateManualCaptureWindow(
  rateHz: number,
  preDurationSeconds: number,
  capacity: number,
): CaptureWindowSummary {
  const normalizedRateHz = Math.round(clampFinite(rateHz, 1, 1000));
  const preSamples = powerCapturePreTriggerSamples(normalizedRateHz, preDurationSeconds);
  const postSamples = Math.max(1, capacity - preSamples - 1);
  return calculateCaptureWindow(normalizedRateHz, preSamples, postSamples, capacity);
}

export function summarizePowerCapture(capture: PowerCapture, rail: string): PowerCaptureSummary {
  const precomputed = capture.summaries?.[rail];
  if (precomputed) return precomputed;
  const voltage = nominalVoltage(rail) ?? 0;
  let ampHours = 0;
  let wattHours = 0;
  let peakCurrentA = 0;

  for (let index = 0; index < capture.samples.length; index += 1) {
    peakCurrentA = Math.max(peakCurrentA, sampleCurrentA(capture, index, rail));
    if (index === 0) continue;

    const previous = capture.samples[index - 1];
    const current = capture.samples[index];
    const hours = Math.max(0, current.deviceTimeUs - previous.deviceTimeUs) / 3_600_000_000;
    const averageCurrent = (
      sampleCurrentA(capture, index - 1, rail) + sampleCurrentA(capture, index, rail)
    ) / 2;
    ampHours += averageCurrent * hours;
    wattHours += averageCurrent * voltage * hours;
  }

  const first = capture.samples[0]?.deviceTimeUs ?? 0;
  const last = capture.samples.at(-1)?.deviceTimeUs ?? first;
  const durationMs = Math.max(0, last - first) / 1000;
  const durationHours = durationMs / 3_600_000;
  const averageCurrentA = durationHours > 0 ? ampHours / durationHours : 0;

  return {
    nominalVoltageV: voltage,
    durationMs,
    averageCurrentA,
    peakCurrentA,
    averagePowerW: averageCurrentA * voltage,
    peakPowerW: peakCurrentA * voltage,
    milliampHours: ampHours * 1000,
    wattHours,
  };
}

export function estimateFiveVoltBattery(
  summary: PowerCaptureSummary,
  capacityMah: number,
  efficiencyPercent: number,
  targetRuntimeHours: number,
): FiveVoltBatteryEstimate {
  const normalizedCapacityMah = clampFinite(capacityMah, 0, 100_000_000);
  const efficiency = clampFinite(efficiencyPercent, 1, 100) / 100;
  const normalizedTargetRuntimeHours = clampFinite(targetRuntimeHours, 0, 1_000_000);
  const availableEnergyWh = normalizedCapacityMah / 1000 * 5 * efficiency;
  const hasAverageLoad = summary.averagePowerW > 0;
  const hasRecordedEnergy = summary.wattHours > 0;

  return {
    efficiency,
    availableEnergyWh,
    equivalentCapacityMah: hasRecordedEnergy
      ? summary.wattHours / (5 * efficiency) * 1000
      : 0,
    rechargeInputWh: hasRecordedEnergy ? summary.wattHours / efficiency : 0,
    runtimeHours: hasAverageLoad ? availableEnergyWh / summary.averagePowerW : null,
    requiredCapacityMah: hasAverageLoad
      ? summary.averagePowerW * normalizedTargetRuntimeHours / (5 * efficiency) * 1000
      : null,
    repeatCount: hasRecordedEnergy ? availableEnergyWh / summary.wattHours : null,
  };
}
