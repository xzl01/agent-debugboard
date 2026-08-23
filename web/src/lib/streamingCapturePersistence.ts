import {
  appendPowerCapturePreview,
  appendPowerCaptureSummary,
} from "./powerCapture.ts";
import {
  appendPowerCaptureChunk,
  renewPowerCaptureArchiveLease,
} from "./powerCaptureStore.ts";
import {
  POWER_ARCHIVE_CHUNK_SAMPLES,
  POWER_ARCHIVE_MAX_QUEUED_CHUNKS,
  POWER_CAPTURE_LEASE_RENEW_INTERVAL_MS,
  POWER_PREVIEW_MAX_SAMPLES,
  streamingCaptureLease,
  type StreamingCaptureSession,
} from "./streamingCaptureModel.ts";
import type { CaptureSample } from "./types.ts";

export function queueStreamingLeaseRenewal(
  session: StreamingCaptureSession,
  onWriteError: (error: Error) => void,
): void {
  const now = Date.now();
  if (
    !session.archiveStarted ||
    session.finishing ||
    session.writeError ||
    now - session.lastLeaseRenewedAt < POWER_CAPTURE_LEASE_RENEW_INTERVAL_MS
  ) return;

  session.lastLeaseRenewedAt = now;
  session.writeChain = session.writeChain
    .then(() => renewPowerCaptureArchiveLease(session.archiveId, streamingCaptureLease(session)))
    .catch((reason: unknown) => {
      const error = reason instanceof Error ? reason : new Error(String(reason));
      if (!session.writeError) {
        session.writeError = error;
        onWriteError(error);
      }
      throw error;
    });
}

function queueStreamingChunk(
  session: StreamingCaptureSession,
  chunk: CaptureSample[],
  index: number,
  onWriteError?: (error: Error) => void,
): void {
  session.queuedChunks += 1;
  session.writeChain = session.writeChain
    .then(() => appendPowerCaptureChunk(
      session.archiveId,
      index,
      chunk,
      streamingCaptureLease(session),
    ))
    .then((result) => {
      session.queuedChunks = Math.max(0, session.queuedChunks - 1);
      session.persistedSamples = result.persistedSamples;
      session.persistedBytes = result.estimatedBytes;
      session.lastPersistedSequence = result.lastSequence;
    })
    .catch((reason: unknown) => {
      session.queuedChunks = Math.max(0, session.queuedChunks - 1);
      const error = reason instanceof Error ? reason : new Error(String(reason));
      if (!session.writeError) {
        session.writeError = error;
        onWriteError?.(error);
      }
      throw error;
    });
}

export function flushStreamingCaptureChunk(session: StreamingCaptureSession): void {
  if (session.writeError || session.pendingChunk.length === 0) return;
  const chunk = session.pendingChunk.splice(0);
  const index = session.chunkIndex++;
  queueStreamingChunk(session, chunk, index);
}

export function queueStreamingSamples(
  session: StreamingCaptureSession,
  incoming: readonly CaptureSample[],
  onWriteError: (error: Error) => void,
): void {
  if (session.finishing || session.writeError) return;
  const normalized: CaptureSample[] = [];
  for (const sample of incoming) {
    if (sample.sampleSequence <= session.lastStoredSequence) continue;
    const offset = session.totalSamples;
    const triggered = session.triggerOffset < 0 && (session.triggerSampleSequence > 0
      ? sample.sampleSequence >= session.triggerSampleSequence
      : sample.deviceTimeUs >= session.triggerDeviceTimeUs);
    if (triggered) session.triggerOffset = offset;
    const next = { ...sample, offset, triggered };
    session.lastStoredSequence = sample.sampleSequence;
    session.totalSamples += 1;
    normalized.push(next);
  }
  if (normalized.length === 0) return;

  appendPowerCaptureSummary(session.accumulator, normalized);
  session.previewStride = appendPowerCapturePreview(
    session.preview,
    normalized,
    session.previewStride,
    POWER_PREVIEW_MAX_SAMPLES,
  );
  session.pendingChunk.push(...normalized);
  while (session.pendingChunk.length >= POWER_ARCHIVE_CHUNK_SAMPLES) {
    const chunk = session.pendingChunk.splice(0, POWER_ARCHIVE_CHUNK_SAMPLES);
    const index = session.chunkIndex++;
    if (session.queuedChunks >= POWER_ARCHIVE_MAX_QUEUED_CHUNKS) {
      const error = new Error(
        "Host storage is not keeping up with the capture stream; recording was stopped before browser memory could grow without limit",
      );
      session.writeError = error;
      onWriteError(error);
      return;
    }
    queueStreamingChunk(session, chunk, index, onWriteError);
  }
}
