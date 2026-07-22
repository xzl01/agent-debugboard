/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

import type {
  LogicAnalyzerCapture,
  LogicAnalyzerCaptureConfig,
  LogicAnalyzerConfig,
  LogicAnalyzerSample,
  LogicAnalyzerTriggerType,
} from "./types";

export const LOGIC_ANALYZER_MAX_SAMPLES = 512;
export const LOGIC_ANALYZER_SAMPLE_RATES_HZ = [
  1000000,
  5000000,
  10000000,
  25000000,
  50000000,
  100000000,
  125000000,
] as const;
export const LOGIC_ANALYZER_MIN_SAMPLE_RATE_HZ = LOGIC_ANALYZER_SAMPLE_RATES_HZ[0];
export const LOGIC_ANALYZER_MAX_SAMPLE_RATE_HZ =
  LOGIC_ANALYZER_SAMPLE_RATES_HZ[LOGIC_ANALYZER_SAMPLE_RATES_HZ.length - 1];

function clampInteger(value: number, minimum: number, maximum: number): number {
  if (!Number.isFinite(value)) return minimum;
  return Math.min(maximum, Math.max(minimum, Math.trunc(value)));
}

function dedupeAndSortPins(pins: number[]): number[] {
  return [...new Set(pins)].sort((left, right) => left - right);
}

function normalizePositiveNumber(value: number | undefined): number | null {
  return typeof value === "number" && Number.isFinite(value) && value > 0 ? value : null;
}

function normalizeSampleRateFromPeriod(samplePeriodPs: number | undefined): number | null {
  const normalizedPeriodPs = normalizePositiveNumber(samplePeriodPs);
  if (normalizedPeriodPs == null) return null;
  return 1000000000000 / normalizedPeriodPs;
}

export function usesTrigger(triggerType: LogicAnalyzerTriggerType): boolean {
  return triggerType !== "none";
}

function downloadBlob(name: string, blob: Blob) {
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = name;
  anchor.click();
  URL.revokeObjectURL(url);
}

export function exportToCsv(capture: LogicAnalyzerCapture): void {
  if (!capture || !capture.config) return;

  const { config, samples, sampleCount } = capture;
  const lines: string[] = [];
  const channelLabels = Array.from({ length: config.pinCount }, (_, i) =>
    config.selectedPins?.[i] != null ? `GP${config.selectedPins[i]}` : `CH${i + 1}`
  );

  const header = channelLabels.join(",");
  lines.push(header);

  for (let i = 0; i < sampleCount; i++) {
    const sample = samples[i];
    if (!sample) continue;

    const values = Array.from({ length: config.pinCount }, (_, bit) =>
      (sample.values >> bit) & 1
    );
    lines.push(values.join(","));
  }

  const csv = lines.join("\n") + "\n";
  const blob = new Blob([csv], { type: "text/csv;charset=utf-8" });
  downloadBlob(`logic-capture-${Date.now()}.csv`, blob);
}

export function exportToSr(capture: LogicAnalyzerCapture): void {
  if (!capture || !capture.config) return;

  const { config, samples, sampleCount } = capture;
  const actualSampleRateHz = Math.max(1, Math.round(getLogicAnalyzerActualSampleRate(config)));
  const channelLabels = Array.from({ length: config.pinCount }, (_, i) =>
    config.selectedPins?.[i] != null ? `GP${config.selectedPins[i]}` : `CH${i + 1}`
  );

  const versionContent = "2";

  const metadataContent = [
    "[global]",
    "sigrok version=0.5.2",
    "",
    "[device 1]",
    `capturefile=logic-1`,
    `total probes=${config.pinCount}`,
    "total analog=0",
    `samplerate=${actualSampleRateHz}`,
    "unitsize=2",
    `starttime=${new Date().toISOString()}`,
    ...channelLabels.map((label, i) => `probe${i + 1}=${label}`),
    "",
  ].join("\n");

  const unitSize = 2;
  const logicData = new Uint8Array(sampleCount * unitSize);

  for (let i = 0; i < sampleCount; i++) {
    const sample = samples[i];
    if (!sample) continue;

    const offset = i * unitSize;
    logicData[offset] = sample.values & 0xff;
    if (unitSize > 1) {
      logicData[offset + 1] = (sample.values >> 8) & 0xff;
    }
  }

  import("jszip").then((JSZipModule) => {
    const JSZip = JSZipModule.default;
    const zip = new JSZip();
    zip.file("version", versionContent);
    zip.file("metadata", metadataContent);
    zip.file("logic-1", logicData);

    zip.generateAsync({ type: "blob" }).then((blob: Blob) => {
      downloadBlob(`logic-capture-${Date.now()}.sr`, blob);
    });
  });
}

export function extractLogicAnalyzerErrorMessage(payload: unknown): string | null {
  if (!payload || typeof payload !== "object") return null;

  const record = payload as Record<string, unknown>;
  if (typeof record.message === "string" && record.message.length > 0) {
    return record.message;
  }

  if (!record.error || typeof record.error !== "object") {
    return null;
  }

  const errorRecord = record.error as Record<string, unknown>;
  if (typeof errorRecord.message === "string" && errorRecord.message.length > 0) {
    return errorRecord.message;
  }

  return null;
}

export function calculateMaxSamples(): number {
  return LOGIC_ANALYZER_MAX_SAMPLES;
}

export function normalizeLogicAnalyzerSampleRate(requestedRate: number): number {
  if (!Number.isFinite(requestedRate)) {
    return LOGIC_ANALYZER_MIN_SAMPLE_RATE_HZ;
  }

  const truncatedRate = Math.trunc(requestedRate);
  if (truncatedRate <= LOGIC_ANALYZER_MIN_SAMPLE_RATE_HZ) {
    return LOGIC_ANALYZER_MIN_SAMPLE_RATE_HZ;
  }
  if (truncatedRate >= LOGIC_ANALYZER_MAX_SAMPLE_RATE_HZ) {
    return LOGIC_ANALYZER_MAX_SAMPLE_RATE_HZ;
  }

  let nearestRate: number = LOGIC_ANALYZER_SAMPLE_RATES_HZ[0];
  let nearestDistance = Math.abs(truncatedRate - nearestRate);

  for (const candidateRate of LOGIC_ANALYZER_SAMPLE_RATES_HZ.slice(1)) {
    const candidateDistance = Math.abs(truncatedRate - candidateRate);
    if (
      candidateDistance < nearestDistance ||
      (candidateDistance === nearestDistance && candidateRate < nearestRate)
    ) {
      nearestRate = candidateRate;
      nearestDistance = candidateDistance;
    }
  }

  return nearestRate;
}

function normalizeLogicAnalyzerSelectedPins(pins: number[]): number[] {
  return dedupeAndSortPins(
    pins.flatMap((pin) => {
      if (!Number.isFinite(pin)) return [];
      const normalizedPin = Math.trunc(pin);
      return AVAILABLE_PIN_SET.has(normalizedPin) ? [normalizedPin] : [];
    })
  );
}

export function normalizeLogicAnalyzerConfig(config: LogicAnalyzerConfig): LogicAnalyzerConfig {
  const selectedPins = normalizeLogicAnalyzerSelectedPins(config.selectedPins);
  const sampleRateHz = normalizeLogicAnalyzerSampleRate(config.sampleRateHz);
  const preSamples = config.triggerType === "none" ? 0 : clampInteger(config.preSamples, 0, LOGIC_ANALYZER_MAX_SAMPLES);
  const postSamples = clampInteger(config.postSamples, 1, LOGIC_ANALYZER_MAX_SAMPLES);
  const triggerPin = clampInteger(config.triggerPin, 0, Math.max(0, selectedPins.length - 1));

  return {
    ...config,
    selectedPins,
    sampleRateHz,
    preSamples,
    postSamples,
    triggerPin,
  };
}

export function buildLogicAnalyzerArmRequest(config: LogicAnalyzerConfig): {
  selected_pins: number[];
  pin_base: number;
  pin_count: number;
  sample_rate_hz: number;
  pre_samples: number;
  post_samples: number;
  trigger: LogicAnalyzerTriggerType;
  trigger_pin: number;
} {
  const normalizedConfig = normalizeLogicAnalyzerConfig(config);
  const selectedPins = normalizedConfig.selectedPins;

  return {
    selected_pins: selectedPins,
    pin_base: selectedPins[0] ?? 0,
    pin_count: selectedPins.length,
    sample_rate_hz: normalizedConfig.sampleRateHz,
    pre_samples: normalizedConfig.preSamples,
    post_samples: normalizedConfig.postSamples,
    trigger: normalizedConfig.triggerType,
    trigger_pin: normalizedConfig.triggerPin,
  };
}

export function calculateActualSampleRate(requestedRate: number): number {
  return normalizeLogicAnalyzerSampleRate(requestedRate);
}

export function getLogicAnalyzerRequestedSampleRate(
  config: Pick<LogicAnalyzerConfig, "sampleRateHz"> | LogicAnalyzerCaptureConfig
): number {
  return (
    normalizePositiveNumber(
      "requestedSampleRateHz" in config ? config.requestedSampleRateHz : undefined
    ) ??
    normalizePositiveNumber(config.sampleRateHz) ??
    LOGIC_ANALYZER_MIN_SAMPLE_RATE_HZ
  );
}

export function getLogicAnalyzerActualSampleRate(
  config: Pick<LogicAnalyzerConfig, "sampleRateHz"> | LogicAnalyzerCaptureConfig
): number {
  return (
    normalizePositiveNumber(
      "actualSampleRateHz" in config ? config.actualSampleRateHz : undefined
    ) ??
    normalizeSampleRateFromPeriod(
      "samplePeriodPs" in config ? config.samplePeriodPs : undefined
    ) ??
    normalizePositiveNumber(config.sampleRateHz) ??
    normalizePositiveNumber(
      "requestedSampleRateHz" in config ? config.requestedSampleRateHz : undefined
    ) ??
    LOGIC_ANALYZER_MIN_SAMPLE_RATE_HZ
  );
}

export function getLogicAnalyzerSamplePeriodPs(
  config: Pick<LogicAnalyzerConfig, "sampleRateHz"> | LogicAnalyzerCaptureConfig
): number | null {
  const normalizedPeriodPs =
    "samplePeriodPs" in config ? normalizePositiveNumber(config.samplePeriodPs) : null;
  if (normalizedPeriodPs != null) {
    return normalizedPeriodPs;
  }

  const actualSampleRateHz = getLogicAnalyzerActualSampleRate(config);
  return actualSampleRateHz > 0 ? 1000000000000 / actualSampleRateHz : null;
}

export function getLogicAnalyzerBackend(config: LogicAnalyzerCaptureConfig | null | undefined): string | null {
  return typeof config?.backend === "string" && config.backend.length > 0 ? config.backend : null;
}

function normalizeLogicAnalyzerCaptureConfig(
  config: LogicAnalyzerCaptureConfig
): LogicAnalyzerCaptureConfig {
  const selectedPins = config.selectedPins
    ? normalizeLogicAnalyzerSelectedPins(config.selectedPins)
    : undefined;
  const pinCountLimit = selectedPins?.length ?? AVAILABLE_PINS.length;
  const pinCount = clampInteger(config.pinCount, 1, Math.max(1, pinCountLimit));
  const pinBase = selectedPins?.[0] ?? clampInteger(config.pinBase, 7, 29);
  const triggerPin =
    config.triggerPin == null
      ? undefined
      : clampInteger(config.triggerPin, 0, Math.max(0, pinCount - 1));

  return {
    ...config,
    pinBase,
    pinCount,
    selectedPins,
    triggerPin,
  };
}

function normalizeLogicAnalyzerCaptureSamples(
  samples: LogicAnalyzerSample[],
  sampleCount: number
): LogicAnalyzerSample[] {
  return samples.slice(0, sampleCount).map((sample) => ({
    timestampUs: Number.isFinite(sample.timestampUs) ? sample.timestampUs : 0,
    values: Number.isFinite(sample.values) ? Math.trunc(sample.values) : 0,
  }));
}

export function normalizeLogicAnalyzerCapture(
  capture: LogicAnalyzerCapture
): LogicAnalyzerCapture {
  const config = normalizeLogicAnalyzerCaptureConfig(capture.config);
  const sampleCount = clampInteger(
    capture.sampleCount,
    0,
    capture.samples.length
  );
  const samples = normalizeLogicAnalyzerCaptureSamples(capture.samples, sampleCount);
  const triggerIndex =
    sampleCount === 0 ? 0 : clampInteger(capture.triggerIndex, 0, sampleCount - 1);

  return {
    ...capture,
    config,
    sampleCount,
    triggerIndex,
    samples,
  };
}

export function formatSampleRate(rateHz: number): string {
  if (rateHz >= 1000000) {
    return `${(rateHz / 1000000).toFixed(1)} MHz`;
  }
  if (rateHz >= 1000) {
    return `${(rateHz / 1000).toFixed(1)} kHz`;
  }
  return `${rateHz} Hz`;
}

export function formatSampleCount(count: number): string {
  if (count >= 1000000) {
    return `${(count / 1000000).toFixed(2)}M`;
  }
  if (count >= 1000) {
    return `${(count / 1000).toFixed(1)}K`;
  }
  return String(count);
}

export function formatSamplePeriod(samplePeriodPs: number | null): string | null {
  if (samplePeriodPs == null || !Number.isFinite(samplePeriodPs) || samplePeriodPs <= 0) {
    return null;
  }

  if (samplePeriodPs >= 1000000) {
    return `${(samplePeriodPs / 1000000).toFixed(3)} µs`;
  }
  if (samplePeriodPs >= 1000) {
    return `${(samplePeriodPs / 1000).toFixed(3)} ns`;
  }
  return `${samplePeriodPs.toFixed(0)} ps`;
}

export function formatDuration(sampleCount: number, sampleRateHz: number): string {
  if (!Number.isFinite(sampleCount) || sampleCount <= 0 || !Number.isFinite(sampleRateHz) || sampleRateHz <= 0) {
    return "0 µs";
  }

  const seconds = sampleCount / sampleRateHz;
  if (seconds >= 1) {
    return `${seconds.toFixed(2)} s`;
  }
  if (seconds >= 0.001) {
    return `${(seconds * 1000).toFixed(2)} ms`;
  }
  return `${(seconds * 1000000).toFixed(1)} µs`;
}

export const DEFAULT_CONFIG: LogicAnalyzerConfig = {
  selectedPins: [13],
  sampleRateHz: 1000000,
  preSamples: 0,
  postSamples: LOGIC_ANALYZER_MAX_SAMPLES,
  triggerType: "none",
  triggerPin: 0,
};

export const AVAILABLE_PINS = [
  { pin: 7, name: "GP7/CON_MAS" },
  { pin: 8, name: "GP8/CON_REST" },
  { pin: 9, name: "GP9/CON_USER" },
  { pin: 10, name: "GP10" },
  { pin: 11, name: "GP11" },
  { pin: 12, name: "GP12" },
  { pin: 13, name: "GP13" },
  { pin: 14, name: "GP14" },
  { pin: 15, name: "GP15" },
  { pin: 16, name: "GP16" },
  { pin: 17, name: "GP17" },
  { pin: 18, name: "GP18" },
  { pin: 19, name: "GP19" },
  { pin: 20, name: "GP20" },
  { pin: 29, name: "GP29/ADC3" },
];

const AVAILABLE_PIN_SET = new Set<number>(AVAILABLE_PINS.map(({ pin }) => pin));

export const SAMPLE_RATES = [
  { value: 1000000, label: "1 MHz" },
  { value: 5000000, label: "5 MHz" },
  { value: 10000000, label: "10 MHz" },
  { value: 25000000, label: "25 MHz" },
  { value: 50000000, label: "50 MHz" },
  { value: 100000000, label: "100 MHz" },
  { value: 125000000, label: "125 MHz" },
];
