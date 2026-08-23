import {
  createPowerCaptureAccumulator,
  finalizePowerCaptureSummaries,
  type PowerCaptureAccumulator,
} from "./powerCapture.ts";
import { POWER_CAPTURE_LEASE_DURATION_MS } from "./powerCaptureStore.ts";
import type { PowerCaptureStopHandshake } from "./powerCaptureStop.ts";
import type {
  CaptureConfig,
  CaptureSample,
  PowerCapture,
} from "./types.ts";

export const MAX_WEB_STREAMING_RATE_HZ = 500;
export const POWER_ARCHIVE_CHUNK_SAMPLES = 200;
export const POWER_PREVIEW_MAX_SAMPLES = 3000;
export const POWER_ARCHIVE_MAX_QUEUED_CHUNKS = 8;
export const POWER_CAPTURE_LEASE_RENEW_INTERVAL_MS = 10_000;

export interface StreamingCaptureSession {
  config: CaptureConfig;
  archiveId: string;
  ownerId: string;
  captureId: number;
  capturedAt: number;
  triggerDeviceTimeUs: number;
  triggerSampleSequence: number;
  triggerOffset: number;
  triggered: boolean;
  finishing: boolean;
  requestedIncomplete: boolean;
  requestedInterruptionReason?: string;
  stopHandshake: PowerCaptureStopHandshake | null;
  finalizePromise: Promise<void> | null;
  archiveStarted: boolean;
  preBuffer: CaptureSample[];
  pendingChunk: CaptureSample[];
  preview: CaptureSample[];
  previewStride: number;
  totalSamples: number;
  droppedSamples: number;
  lastStoredSequence: number;
  chunkIndex: number;
  writeChain: Promise<void>;
  queuedChunks: number;
  persistedSamples: number;
  persistedBytes: number;
  lastPersistedSequence: number;
  writeError: Error | null;
  accumulator: PowerCaptureAccumulator;
  stopTimer: ReturnType<typeof setTimeout> | null;
  lastProgressAt: number;
  lastLeaseRenewedAt: number;
}

export function createArchiveId(): string {
  if (typeof crypto !== "undefined" && typeof crypto.randomUUID === "function") {
    return crypto.randomUUID();
  }
  return `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
}

export function createStreamingCaptureSession(
  config: CaptureConfig,
  ownerId: string,
): StreamingCaptureSession {
  return {
    config,
    archiveId: createArchiveId(),
    ownerId,
    captureId: 0,
    capturedAt: 0,
    triggerDeviceTimeUs: 0,
    triggerSampleSequence: 0,
    triggerOffset: -1,
    triggered: false,
    finishing: false,
    requestedIncomplete: false,
    stopHandshake: null,
    finalizePromise: null,
    archiveStarted: false,
    preBuffer: [],
    pendingChunk: [],
    preview: [],
    previewStride: 1,
    totalSamples: 0,
    droppedSamples: 0,
    lastStoredSequence: 0,
    chunkIndex: 0,
    writeChain: Promise.resolve(),
    queuedChunks: 0,
    persistedSamples: 0,
    persistedBytes: 0,
    lastPersistedSequence: 0,
    writeError: null,
    accumulator: createPowerCaptureAccumulator(),
    stopTimer: null,
    lastProgressAt: 0,
    lastLeaseRenewedAt: 0,
  };
}

export function streamingCaptureRecord(
  session: StreamingCaptureSession,
  incomplete = false,
  interruptionReason?: string,
): PowerCapture {
  const hasDroppedSamples = session.droppedSamples > 0;
  const finalIncomplete = incomplete || hasDroppedSamples;
  const finalInterruptionReason = interruptionReason ?? (hasDroppedSamples
    ? `The debugger reported ${session.droppedSamples} dropped samples`
    : undefined);
  return {
    id: session.captureId,
    trigger: session.config.trigger,
    source: session.config.source,
    edge: session.config.edge,
    thresholdUa: session.config.thresholdUa,
    rateHz: session.config.rateHz,
    preSamples: session.triggerOffset,
    postSamples: Math.max(0, session.totalSamples - session.triggerOffset - 1),
    triggerOffset: session.preview.findIndex((sample) => sample.triggered),
    samples: session.preview,
    capturedAt: session.capturedAt,
    archiveId: session.archiveId,
    sampleCount: session.totalSamples,
    droppedSamples: session.droppedSamples,
    triggerDeviceTimeUs: session.triggerDeviceTimeUs,
    incomplete: finalIncomplete,
    interruptionReason: finalInterruptionReason,
    summaries: finalizePowerCaptureSummaries(session.accumulator),
  };
}

export function streamingCaptureLease(session: StreamingCaptureSession) {
  return {
    ownerId: session.ownerId,
    leaseDurationMs: POWER_CAPTURE_LEASE_DURATION_MS,
    droppedSamples: session.droppedSamples,
  };
}
