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
import {
  SIGROK_SAMPLE_INDEX_MODULO,
  SigrokModeId,
  SigrokModeFlag,
  SigrokTriggerType,
  type SigrokCapsResp,
  type SigrokDataMeta,
  type SigrokConfigReq,
} from "./sigrokClient.ts";

export const LOGIC_ANALYZER_MAX_SAMPLES = 0xffff;
export const LOGIC_ANALYZER_CONFIG_V2_EXACT_WIDE11_100MHZ_SAMPLES = 100000;
export const LOGIC_ANALYZER_HIGH_RATE_MAX_SAMPLES =
  LOGIC_ANALYZER_CONFIG_V2_EXACT_WIDE11_100MHZ_SAMPLES;
export const LOGIC_ANALYZER_MIN_SAMPLE_RATE_HZ = 100000;
export const LOGIC_ANALYZER_WEB_STREAM_MAX_SAMPLE_RATE_HZ = 25000000;
export const LOGIC_ANALYZER_MIN_PRE_TRIGGER_SAMPLE_RATE_HZ = 1000000;
export const LOGIC_ANALYZER_MAX_PRE_TRIGGER_SAMPLE_RATE_HZ =
  LOGIC_ANALYZER_WEB_STREAM_MAX_SAMPLE_RATE_HZ;
export const LOGIC_ANALYZER_MAX_PRE_TRIGGER_TOTAL_SAMPLES = 512;
export const LOGIC_ANALYZER_MAX_PRE_TRIGGER_SAMPLES =
  LOGIC_ANALYZER_MAX_PRE_TRIGGER_TOTAL_SAMPLES - 1;
export const LOGIC_ANALYZER_PRE_TRIGGER_SINGLE_USABLE_SAMPLES = 260096;
export const LOGIC_ANALYZER_PRE_TRIGGER_FAST8_USABLE_SAMPLES = 30720;
export const LOGIC_ANALYZER_PRE_TRIGGER_WIDE11_USABLE_SAMPLES = 14336;
const LOGIC_ANALYZER_PRE_TRIGGER_MINIMUM_POLL_INTERVALS = 2;
export const LOGIC_ANALYZER_SAMPLE_RATES_HZ = [
  100000,
  500000,
  1000000,
  2000000,
  5000000,
  10000000,
  25000000,
  50000000,
  100000000,
  125000000,
] as const;
export const LOGIC_ANALYZER_MAX_SAMPLE_RATE_HZ =
  LOGIC_ANALYZER_SAMPLE_RATES_HZ[LOGIC_ANALYZER_SAMPLE_RATES_HZ.length - 1];

const LOGIC_ANALYZER_NORMALIZED_SAMPLE_RATES_HZ = LOGIC_ANALYZER_SAMPLE_RATES_HZ;

export interface LogicAnalyzerSigrokRequestOptions {
  stream?: boolean;
  supportsConfigV2?: boolean;
  supportsGenericPackedBurst?: boolean;
  caps?: SigrokCapsResp | null;
}

export type LogicAnalyzerStreamStopReason =
  | "manual"
  | "auto_buffer_full"
  | "unexpected_stop";

export interface SigrokBoundedCaptureFrame {
  meta: Pick<SigrokDataMeta, "sampleIndex" | "sampleCount" | "channelMask">;
  samples: Uint8Array;
}

export interface SigrokBoundedCaptureAssembly {
  firstSampleIndex: number;
  sampleCount: number;
  triggerIndex: number;
  values: number[];
}

function clampInteger(value: number, minimum: number, maximum: number): number {
  if (!Number.isFinite(value)) return minimum;
  return Math.min(maximum, Math.max(minimum, Math.trunc(value)));
}

export const LOGIC_ANALYZER_SIGROK_DISABLED_PINS = [7, 8, 9] as const;

export const LOGIC_ANALYZER_CONFIG_V2_EXACT_WIDE11_PINS = [
  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
] as const;
const WEB_SIGROK_SUPPORTED_PINS = LOGIC_ANALYZER_CONFIG_V2_EXACT_WIDE11_PINS;
const WEB_SIGROK_SUPPORTED_PIN_SET = new Set<number>(WEB_SIGROK_SUPPORTED_PINS);
const CAPTURE_AVAILABLE_PIN_SET = new Set<number>([
  ...LOGIC_ANALYZER_SIGROK_DISABLED_PINS,
  ...WEB_SIGROK_SUPPORTED_PINS,
]);

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

function countChannelBits(channelMask: number): number {
  let count = 0;
  let remaining = channelMask >>> 0;
  while (remaining > 0) {
    count += remaining & 1;
    remaining >>>= 1;
  }
  return count;
}

function normalizeSigrokSampleIndex(sampleIndex: number): number {
  if (!Number.isFinite(sampleIndex)) {
    return 0;
  }
  const truncated = Math.trunc(sampleIndex);
  const modulo = truncated % SIGROK_SAMPLE_INDEX_MODULO;
  return modulo < 0 ? modulo + SIGROK_SAMPLE_INDEX_MODULO : modulo;
}

function unwrapSigrokSampleIndex(sampleIndex: number, referenceAbsoluteIndex: number): number {
  const normalizedIndex = normalizeSigrokSampleIndex(sampleIndex);
  const baseCycle = Math.floor(referenceAbsoluteIndex / SIGROK_SAMPLE_INDEX_MODULO);
  const candidateCycles = [baseCycle - 1, baseCycle, baseCycle + 1];
  let bestCandidate = normalizedIndex + candidateCycles[0] * SIGROK_SAMPLE_INDEX_MODULO;
  let bestDistance = Math.abs(bestCandidate - referenceAbsoluteIndex);

  for (const cycle of candidateCycles.slice(1)) {
    const candidate = normalizedIndex + cycle * SIGROK_SAMPLE_INDEX_MODULO;
    const distance = Math.abs(candidate - referenceAbsoluteIndex);
    if (distance < bestDistance) {
      bestCandidate = candidate;
      bestDistance = distance;
    }
  }

  return bestCandidate;
}

export function unpackSigrokSamples(packed: Uint8Array, channelMask: number): number[] {
  const bytesPerSample = Math.max(1, Math.ceil(countChannelBits(channelMask) / 8));
  const values: number[] = [];
  for (let offset = 0; offset + bytesPerSample <= packed.length; offset += bytesPerSample) {
    let sampleValue = packed[offset] ?? 0;
    for (let byteIndex = 1; byteIndex < bytesPerSample; byteIndex += 1) {
      sampleValue |= (packed[offset + byteIndex] ?? 0) << (byteIndex * 8);
    }
    values.push(sampleValue);
  }
  return values;
}

export function assembleBoundedSigrokCapture({
  frames,
  channelMask,
  triggerSampleIndex,
}: {
  frames: readonly SigrokBoundedCaptureFrame[];
  channelMask: number;
  triggerSampleIndex: number | null;
}): SigrokBoundedCaptureAssembly {
  if (frames.length === 0) {
    throw new Error("No DATA frames received before STOPPED");
  }

  let firstSampleIndex = 0;
  let expectedNextIndex = 0;
  const values: number[] = [];

  frames.forEach((frame, frameIndex) => {
    if (frame.meta.channelMask !== channelMask) {
      throw new Error("DATA frame channel mask changed mid-capture");
    }

    const frameValues = unpackSigrokSamples(frame.samples, channelMask);
    if (frameValues.length !== frame.meta.sampleCount) {
      throw new Error("DATA frame sample count does not match payload length");
    }

    const frameStartIndex =
      frameIndex === 0
        ? normalizeSigrokSampleIndex(frame.meta.sampleIndex)
        : unwrapSigrokSampleIndex(frame.meta.sampleIndex, expectedNextIndex);

    if (frameIndex === 0) {
      firstSampleIndex = frameStartIndex;
    } else if (frameStartIndex !== expectedNextIndex) {
      throw new Error(
        frameStartIndex < expectedNextIndex
          ? "Out-of-order DATA frame detected"
          : "Gap detected between DATA frames"
      );
    }

    values.push(...frameValues);
    expectedNextIndex = frameStartIndex + frame.meta.sampleCount;
  });

  let triggerIndex = 0;
  if (triggerSampleIndex != null) {
    const triggerAbsoluteIndex = unwrapSigrokSampleIndex(triggerSampleIndex, firstSampleIndex);
    triggerIndex = triggerAbsoluteIndex - firstSampleIndex;
    if (triggerIndex < 0 || triggerIndex >= values.length) {
      throw new Error("Trigger sample index lies outside captured data window");
    }
  }

  return {
    firstSampleIndex,
    sampleCount: values.length,
    triggerIndex,
    values,
  };
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

export function calculateMaxSamplesForCapabilities(options: { supportsConfigV2?: boolean } = {}): number {
  return options.supportsConfigV2
    ? LOGIC_ANALYZER_HIGH_RATE_MAX_SAMPLES
    : LOGIC_ANALYZER_MAX_SAMPLES;
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

  let nearestRate: number = LOGIC_ANALYZER_NORMALIZED_SAMPLE_RATES_HZ[0];
  let nearestDistance = Math.abs(truncatedRate - nearestRate);

  for (const candidateRate of LOGIC_ANALYZER_NORMALIZED_SAMPLE_RATES_HZ.slice(1)) {
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
      return WEB_SIGROK_SUPPORTED_PIN_SET.has(normalizedPin) ? [normalizedPin] : [];
    })
  );
}

function normalizeCaptureSelectedPins(pins: number[]): number[] {
  return dedupeAndSortPins(
    pins.flatMap((pin) => {
      if (!Number.isFinite(pin)) return [];
      const normalizedPin = Math.trunc(pin);
      return CAPTURE_AVAILABLE_PIN_SET.has(normalizedPin) ? [normalizedPin] : [];
    })
  );
}

export function isWebSigrokPinSupported(pin: number): boolean {
  return WEB_SIGROK_SUPPORTED_PIN_SET.has(pin);
}

export function hasExactWide11PinSelection(pins: readonly number[]): boolean {
  return (
    pins.length === LOGIC_ANALYZER_CONFIG_V2_EXACT_WIDE11_PINS.length &&
    LOGIC_ANALYZER_CONFIG_V2_EXACT_WIDE11_PINS.every((pin, index) => pins[index] === pin)
  );
}

export function isFast8PhysicalSpanSelection(selectedPins: readonly number[]): boolean {
  return selectedPins.length > 0 && selectedPins.every((pin) => pin >= 10 && pin <= 17);
}

function isWide11PhysicalSpanSelection(selectedPins: readonly number[]): boolean {
  return (
    selectedPins.length > 0 &&
    selectedPins.every((pin) => pin >= 10 && pin <= 20) &&
    !isFast8PhysicalSpanSelection(selectedPins)
  );
}

export function getLogicAnalyzerPreTriggerUsableSampleCapacity(
  selectedPins: readonly number[]
): number | null {
  if (selectedPins.length === 1) {
    return LOGIC_ANALYZER_PRE_TRIGGER_SINGLE_USABLE_SAMPLES;
  }
  if (isFast8PhysicalSpanSelection(selectedPins)) {
    return LOGIC_ANALYZER_PRE_TRIGGER_FAST8_USABLE_SAMPLES;
  }
  if (isWide11PhysicalSpanSelection(selectedPins)) {
    return LOGIC_ANALYZER_PRE_TRIGGER_WIDE11_USABLE_SAMPLES;
  }
  return null;
}

export function supportsLegacyConfigV2ExactWide11PackedBurst(
  config: Pick<LogicAnalyzerConfig, "selectedPins" | "sampleRateHz">
): boolean {
  return (
    config.sampleRateHz === 100000000 &&
    hasExactWide11PinSelection(config.selectedPins)
  );
}

export function supportsHighRatePackedBurst(
  config: Pick<LogicAnalyzerConfig, "selectedPins" | "sampleRateHz">
): boolean {
  if (config.selectedPins.length === 0) {
    return false;
  }

  if (config.sampleRateHz === 100000000) {
    return true;
  }

  if (config.sampleRateHz === 125000000) {
    return isFast8PhysicalSpanSelection(config.selectedPins);
  }

  return false;
}

function supportsBoundedPackedBurst(
  config: Pick<LogicAnalyzerConfig, "selectedPins" | "sampleRateHz">
): boolean {
  return (
    config.selectedPins.length > 0 &&
    getLogicAnalyzerUnsupportedRateReason(config) == null
  );
}

export function getLogicAnalyzerUnsupportedRateReason(
  config: Pick<LogicAnalyzerConfig, "selectedPins" | "sampleRateHz">
): string | null {
  if (config.selectedPins.length === 0) {
    return null;
  }

  if (
    config.sampleRateHz === 125000000 &&
    !isFast8PhysicalSpanSelection(config.selectedPins)
  ) {
    return "WIDE11 at 125 MHz is not supported; use GP10-GP17 only or drop to 100 MHz";
  }

  return null;
}

export function getLogicAnalyzerSelectionMaxSamples(
  config: Pick<LogicAnalyzerConfig, "selectedPins" | "sampleRateHz">
): number {
  return supportsBoundedPackedBurst(config)
    ? LOGIC_ANALYZER_HIGH_RATE_MAX_SAMPLES
    : LOGIC_ANALYZER_MAX_SAMPLES;
}

export function getLogicAnalyzerMaxSamplesForConfig(
  config: Pick<LogicAnalyzerConfig, "selectedPins" | "sampleRateHz">,
  options: {
    supportsConfigV2?: boolean;
    supportsGenericPackedBurst?: boolean;
  } = {}
): number {
  if (
    options.supportsGenericPackedBurst &&
    options.supportsConfigV2 &&
    supportsBoundedPackedBurst(config)
  ) {
    return getLogicAnalyzerSelectionMaxSamples(config);
  }

  if (
    options.supportsConfigV2 &&
    supportsLegacyConfigV2ExactWide11PackedBurst(config)
  ) {
    return LOGIC_ANALYZER_HIGH_RATE_MAX_SAMPLES;
  }

  return LOGIC_ANALYZER_MAX_SAMPLES;
}

export function getLogicAnalyzerStreamLimitReason(
  config: Pick<LogicAnalyzerConfig, "selectedPins" | "sampleRateHz">
): string | null {
  const unsupportedRateReason = getLogicAnalyzerUnsupportedRateReason(config);
  if (unsupportedRateReason != null) {
    return unsupportedRateReason;
  }

  if (config.sampleRateHz <= LOGIC_ANALYZER_WEB_STREAM_MAX_SAMPLE_RATE_HZ) {
    return null;
  }

  return supportsHighRatePackedBurst(config)
    ? null
    : "Streaming above 25 MHz is only available for 100/125 MHz FAST8 or 100 MHz WIDE11 packed burst";
}

export function getLogicAnalyzerNegotiatedCapabilityReason(
  config: Pick<LogicAnalyzerConfig, "selectedPins" | "sampleRateHz" | "postSamples">,
  options: LogicAnalyzerSigrokRequestOptions = {}
): string | null {
  const isPotentialHighRatePackedBurst = supportsHighRatePackedBurst(config);
  const isPotentialBoundedPackedBurst = supportsBoundedPackedBurst(config);
  const isLegacyExactWide11 = supportsLegacyConfigV2ExactWide11PackedBurst(config);

  if (options.stream) {
    if (config.sampleRateHz <= LOGIC_ANALYZER_WEB_STREAM_MAX_SAMPLE_RATE_HZ) {
      return null;
    }

    if (!isPotentialHighRatePackedBurst) {
      return null;
    }

    if (!options.supportsGenericPackedBurst) {
      return "Connected firmware did not advertise GENERIC_PACKED_BURST; high-rate Stream requires HELLO server_flags bit1";
    }

    return null;
  }

  if (config.postSamples <= LOGIC_ANALYZER_MAX_SAMPLES) {
    if (
      isPotentialHighRatePackedBurst &&
      isFast8PhysicalSpanSelection(config.selectedPins) &&
      !options.supportsGenericPackedBurst
    ) {
      return "Connected firmware did not advertise GENERIC_PACKED_BURST; generic FAST8 high-rate capture requires HELLO server_flags bit1";
    }

    return null;
  }

  if (!options.supportsConfigV2) {
    return "Requested post samples above 65535 require CONFIG_V2 support from firmware";
  }

  if (isLegacyExactWide11 && config.postSamples === LOGIC_ANALYZER_HIGH_RATE_MAX_SAMPLES) {
    return null;
  }

  if (!isPotentialBoundedPackedBurst) {
    return "Post-trigger samples above 65535 are unsupported for the selected packed capture plan";
  }

  if (!options.supportsGenericPackedBurst) {
    return "Connected firmware did not advertise GENERIC_PACKED_BURST; generic bounded post-trigger samples above 65535 require HELLO server_flags bit1";
  }

  if (config.postSamples > LOGIC_ANALYZER_HIGH_RATE_MAX_SAMPLES) {
    return `High-rate packed burst supports at most ${LOGIC_ANALYZER_HIGH_RATE_MAX_SAMPLES} post-trigger samples`;
  }

  return null;
}

type LogicAnalyzerPreTriggerConfig = Pick<
  LogicAnalyzerConfig,
  "selectedPins" | "sampleRateHz" | "triggerType" | "preSamples" | "postSamples"
>;

function getLogicAnalyzerPreTriggerGenericReason(
  config: LogicAnalyzerPreTriggerConfig,
  enforceTotal: boolean
): string | null {
  if (config.preSamples === 0) {
    return null;
  }
  if (!Number.isInteger(config.preSamples) || config.preSamples < 1) {
    return "Pre-trigger samples must be at least 1";
  }
  if (config.preSamples > LOGIC_ANALYZER_MAX_PRE_TRIGGER_SAMPLES) {
    return `Pre-trigger samples must be at most ${LOGIC_ANALYZER_MAX_PRE_TRIGGER_SAMPLES}`;
  }
  if (config.triggerType === "none") {
    return "Pre-trigger capture requires a rising, falling, or either trigger";
  }
  if (
    !Number.isFinite(config.sampleRateHz) ||
    config.sampleRateHz < LOGIC_ANALYZER_MIN_PRE_TRIGGER_SAMPLE_RATE_HZ ||
    config.sampleRateHz > LOGIC_ANALYZER_MAX_PRE_TRIGGER_SAMPLE_RATE_HZ
  ) {
    return "Pre-trigger capture requires a requested sample rate from 1 MHz through 25 MHz";
  }
  if (!Number.isInteger(config.postSamples) || config.postSamples < 1) {
    return "Pre-trigger capture requires at least 1 post-trigger sample";
  }
  if (
    enforceTotal &&
    config.preSamples + config.postSamples > LOGIC_ANALYZER_MAX_PRE_TRIGGER_TOTAL_SAMPLES
  ) {
    return `Pre-trigger capture supports at most ${LOGIC_ANALYZER_MAX_PRE_TRIGGER_TOTAL_SAMPLES} total samples`;
  }

  const usableCapacity = getLogicAnalyzerPreTriggerUsableSampleCapacity(config.selectedPins);
  if (usableCapacity == null) {
    return "Pre-trigger capture is unsupported for the selected pin plan";
  }

  const minimumRetentionSamples =
    LOGIC_ANALYZER_PRE_TRIGGER_MINIMUM_POLL_INTERVALS *
    Math.ceil(config.sampleRateHz / 1000);
  if (usableCapacity < minimumRetentionSamples) {
    const modeName = isFast8PhysicalSpanSelection(config.selectedPins)
      ? "FAST8"
      : isWide11PhysicalSpanSelection(config.selectedPins)
        ? "WIDE11"
        : "the selected plan";
    return `Pre-trigger capture is infeasible for ${modeName}: usable capacity ${usableCapacity} samples cannot retain two 1 ms poll intervals at ${config.sampleRateHz / 1000000} MHz (requires ${minimumRetentionSamples} samples)`;
  }

  return null;
}

function getSigrokModeName(modeId: SigrokModeId): string {
  switch (modeId) {
    case SigrokModeId.FAST8:
      return "FAST8";
    case SigrokModeId.WIDE11:
      return "WIDE11";
  }
}

export function getLogicAnalyzerPreTriggerReason(
  config: LogicAnalyzerPreTriggerConfig,
  options: LogicAnalyzerSigrokRequestOptions = {}
): string | null {
  const genericReason = getLogicAnalyzerPreTriggerGenericReason(config, true);
  if (genericReason != null || config.preSamples === 0) {
    return genericReason;
  }

  const modeId = getSigrokModeForPins(config.selectedPins);
  const modeCaps = options.caps?.modes.find((mode) => mode.modeId === modeId);
  if (modeCaps == null || (modeCaps.modeFlags & SigrokModeFlag.PRE_TRIGGER) === 0) {
    return `Connected firmware CAPS did not advertise PRE_TRIGGER for Sigrok mode ${getSigrokModeName(modeId)}`;
  }

  return null;
}

export function classifyLogicAnalyzerStreamStop({
  config,
  sampleCount,
  userInitiated,
}: {
  config: Pick<LogicAnalyzerConfig, "selectedPins" | "sampleRateHz">;
  sampleCount: number;
  userInitiated: boolean;
}): LogicAnalyzerStreamStopReason {
  if (userInitiated) {
    return "manual";
  }

  if (
    supportsHighRatePackedBurst(config) &&
    sampleCount === LOGIC_ANALYZER_HIGH_RATE_MAX_SAMPLES
  ) {
    return "auto_buffer_full";
  }

  return "unexpected_stop";
}

function getLogicAnalyzerConfigValidationError(
  config: LogicAnalyzerPreTriggerConfig,
  options: LogicAnalyzerSigrokRequestOptions = {}
): string | null {
  const unsupportedRateReason = getLogicAnalyzerUnsupportedRateReason(config);
  if (unsupportedRateReason != null) {
    return unsupportedRateReason;
  }

  const streamLimitReason = options.stream ? getLogicAnalyzerStreamLimitReason(config) : null;
  if (streamLimitReason != null) {
    return streamLimitReason;
  }

  const preTriggerReason = getLogicAnalyzerPreTriggerReason(config, options);
  if (preTriggerReason != null) {
    return preTriggerReason;
  }

  return getLogicAnalyzerNegotiatedCapabilityReason(config, options);
}

export function getSigrokModeForPins(selectedPins: readonly number[]): SigrokModeId {
  return selectedPins.some((pin) => pin >= 18)
    ? SigrokModeId.WIDE11
    : SigrokModeId.FAST8;
}

export function mapSigrokLogicalChannel(pin: number, modeId: SigrokModeId): number | null {
  if (modeId === SigrokModeId.FAST8) {
    return pin >= 10 && pin <= 17 ? pin - 10 : null;
  }
  if (pin >= 10 && pin <= 20) {
    return pin - 10;
  }
  return null;
}

function buildSigrokChannelMask(selectedPins: readonly number[], modeId: SigrokModeId): number {
  let mask = 0;
  for (const pin of selectedPins) {
    const logicalChannel = mapSigrokLogicalChannel(pin, modeId);
    if (logicalChannel == null) {
      throw new Error(`Pin GP${pin} is not available in sigrok mode ${modeId}`);
    }
    mask |= 1 << logicalChannel;
  }
  return mask;
}

function mapTriggerType(triggerType: LogicAnalyzerTriggerType): SigrokTriggerType {
  switch (triggerType) {
    case "rising":
      return SigrokTriggerType.RISING;
    case "falling":
      return SigrokTriggerType.FALLING;
    case "either":
      return SigrokTriggerType.EITHER;
    default:
      return SigrokTriggerType.NONE;
  }
}

export function buildSigrokCaptureRequest(
  config: LogicAnalyzerConfig,
  options: LogicAnalyzerSigrokRequestOptions = {}
): SigrokConfigReq {
  const normalizedConfig = normalizeLogicAnalyzerConfig(
    options.stream ? { ...config, preSamples: 0, postSamples: 1 } : config,
    options
  );
  if (normalizedConfig.selectedPins.length === 0) {
    throw new Error("Select at least one supported pin");
  }
  const validationError = getLogicAnalyzerConfigValidationError(normalizedConfig, options);
  if (validationError != null) {
    throw new Error(validationError);
  }

  const modeId = getSigrokModeForPins(normalizedConfig.selectedPins);
  const triggerPinNumber =
    normalizedConfig.triggerType === "none"
      ? null
      : normalizedConfig.selectedPins[normalizedConfig.triggerPin] ?? null;
  const triggerChannel =
    triggerPinNumber == null ? 0 : mapSigrokLogicalChannel(triggerPinNumber, modeId);
  if (triggerPinNumber != null && triggerChannel == null) {
    throw new Error(`Trigger pin GP${triggerPinNumber} is not available in sigrok mode ${modeId}`);
  }

  return {
    modeId,
    triggerType: mapTriggerType(normalizedConfig.triggerType),
    triggerChannel: triggerChannel ?? 0,
    channelMask: buildSigrokChannelMask(normalizedConfig.selectedPins, modeId),
    samplerateKhz: Math.max(1, Math.round(normalizedConfig.sampleRateHz / 1000)),
    preSamples: options.stream ? 0 : normalizedConfig.preSamples,
    postSamples: options.stream ? 0 : normalizedConfig.postSamples,
  };
}

function normalizeLogicAnalyzerConfigBase(config: LogicAnalyzerConfig): LogicAnalyzerConfig {
  const selectedPins = normalizeLogicAnalyzerSelectedPins(config.selectedPins);
  const sampleRateHz = normalizeLogicAnalyzerSampleRate(config.sampleRateHz);
  const preSamples = Number.isFinite(config.preSamples) ? Math.trunc(config.preSamples) : 0;
  const postSamples = clampInteger(config.postSamples, 1, Number.MAX_SAFE_INTEGER);
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

export function normalizeLogicAnalyzerLocalConfig(config: LogicAnalyzerConfig): LogicAnalyzerConfig {
  const normalizedConfig = normalizeLogicAnalyzerConfigBase(config);
  if (
    normalizedConfig.preSamples <= 0 ||
    getLogicAnalyzerPreTriggerGenericReason(normalizedConfig, false) != null
  ) {
    return { ...normalizedConfig, preSamples: 0 };
  }

  return {
    ...normalizedConfig,
    postSamples: Math.min(
      normalizedConfig.postSamples,
      LOGIC_ANALYZER_MAX_PRE_TRIGGER_TOTAL_SAMPLES - normalizedConfig.preSamples
    ),
  };
}

export function normalizeLogicAnalyzerConfig(config: LogicAnalyzerConfig): LogicAnalyzerConfig;
export function normalizeLogicAnalyzerConfig(
  config: LogicAnalyzerConfig,
  options: LogicAnalyzerSigrokRequestOptions
): LogicAnalyzerConfig;

export function normalizeLogicAnalyzerConfig(
  config: LogicAnalyzerConfig,
  options: LogicAnalyzerSigrokRequestOptions = {}
): LogicAnalyzerConfig {
  const normalizedConfig = normalizeLogicAnalyzerConfigBase(config);
  const validationConfig =
    normalizedConfig.preSamples > 0
      ? {
          ...normalizedConfig,
          postSamples: Number.isFinite(config.postSamples) ? Math.trunc(config.postSamples) : 0,
        }
      : normalizedConfig;
  const validationError = getLogicAnalyzerConfigValidationError(validationConfig, options);
  if (validationError != null) {
    throw new Error(validationError);
  }

  return normalizedConfig;
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
    ? normalizeCaptureSelectedPins(config.selectedPins)
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
];

export const SAMPLE_RATES = [
  { value: 100000, label: "100 kHz" },
  { value: 500000, label: "500 kHz" },
  { value: 1000000, label: "1 MHz" },
  { value: 2000000, label: "2 MHz" },
  { value: 5000000, label: "5 MHz" },
  { value: 10000000, label: "10 MHz" },
  { value: 25000000, label: "25 MHz" },
  { value: 50000000, label: "50 MHz" },
  { value: 100000000, label: "100 MHz" },
  { value: 125000000, label: "125 MHz" },
];
