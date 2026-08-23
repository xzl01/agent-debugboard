import assert from "node:assert/strict";
import test from "node:test";
import {
  createArchiveId,
  createStreamingCaptureSession,
  MAX_WEB_STREAMING_RATE_HZ,
  POWER_ARCHIVE_CHUNK_SAMPLES,
  POWER_ARCHIVE_MAX_QUEUED_CHUNKS,
  POWER_CAPTURE_LEASE_RENEW_INTERVAL_MS,
  POWER_PREVIEW_MAX_SAMPLES,
  streamingCaptureLease,
  streamingCaptureRecord,
  type StreamingCaptureSession,
} from "./streamingCaptureModel.ts";
import { POWER_CAPTURE_LEASE_DURATION_MS } from "./powerCaptureStore.ts";
import type { CaptureConfig, CaptureSample } from "./types.ts";

const CONFIG: CaptureConfig = {
  trigger: "manual",
  source: "5v_out",
  edge: "either",
  thresholdUa: 0,
  rateHz: 100,
  preSamples: 1,
  postSamples: 2,
  streaming: true,
};

function previewSample(offset: number, triggered: boolean): CaptureSample {
  return {
    offset,
    triggered,
    sampleSequence: 100 + offset,
    deviceTimeUs: 1_000_000 + offset * 10_000,
    readings: [],
  };
}

test("streaming capture constants keep their protocol values", () => {
  // Given the extracted model module
  // Then every limit keeps the exact value the wire protocol and UI rely on
  assert.equal(MAX_WEB_STREAMING_RATE_HZ, 500);
  assert.equal(POWER_ARCHIVE_CHUNK_SAMPLES, 200);
  assert.equal(POWER_PREVIEW_MAX_SAMPLES, 3000);
  assert.equal(POWER_ARCHIVE_MAX_QUEUED_CHUNKS, 8);
  assert.equal(POWER_CAPTURE_LEASE_RENEW_INTERVAL_MS, 10_000);
});

test("createArchiveId returns distinct non-empty identifiers", () => {
  // Given two archive id generations
  const first = createArchiveId();
  const second = createArchiveId();

  // Then both are usable unique archive keys
  assert.equal(typeof first, "string");
  assert.notEqual(first.length, 0);
  assert.notEqual(first, second);
});

test("createStreamingCaptureSession initializes the mutable accumulator state", () => {
  // Given a streaming config and an owner id
  // When a session is created
  const session = createStreamingCaptureSession(CONFIG, "owner-1");

  // Then every field starts from the exact initial state armCapture used inline
  assert.equal(session.config, CONFIG);
  assert.equal(session.ownerId, "owner-1");
  assert.equal(typeof session.archiveId, "string");
  assert.notEqual(session.archiveId.length, 0);
  assert.equal(session.captureId, 0);
  assert.equal(session.capturedAt, 0);
  assert.equal(session.triggerDeviceTimeUs, 0);
  assert.equal(session.triggerSampleSequence, 0);
  assert.equal(session.triggerOffset, -1);
  assert.equal(session.triggered, false);
  assert.equal(session.finishing, false);
  assert.equal(session.requestedIncomplete, false);
  assert.equal(session.stopHandshake, null);
  assert.equal(session.finalizePromise, null);
  assert.equal(session.archiveStarted, false);
  assert.deepEqual(session.preBuffer, []);
  assert.deepEqual(session.pendingChunk, []);
  assert.deepEqual(session.preview, []);
  assert.equal(session.previewStride, 1);
  assert.equal(session.totalSamples, 0);
  assert.equal(session.droppedSamples, 0);
  assert.equal(session.lastStoredSequence, 0);
  assert.equal(session.chunkIndex, 0);
  assert.equal(session.queuedChunks, 0);
  assert.equal(session.persistedSamples, 0);
  assert.equal(session.persistedBytes, 0);
  assert.equal(session.lastPersistedSequence, 0);
  assert.equal(session.writeError, null);
  assert.equal(session.stopTimer, null);
  assert.equal(session.lastProgressAt, 0);
  assert.equal(session.lastLeaseRenewedAt, 0);
});

function recordedSession(overrides: Partial<StreamingCaptureSession> = {}): StreamingCaptureSession {
  const session = createStreamingCaptureSession(CONFIG, "owner-1");
  session.captureId = 42;
  session.capturedAt = 1_700_000_000_000;
  session.triggerDeviceTimeUs = 1_010_000;
  session.triggerOffset = 1;
  session.totalSamples = 4;
  session.preview.push(
    previewSample(0, false),
    previewSample(1, true),
    previewSample(2, false),
    previewSample(4, false),
  );
  return Object.assign(session, overrides);
}

test("streamingCaptureRecord maps session state into the public capture shape", () => {
  // Given a session with a triggered preview window
  // When the public record is built without interruption
  const record = streamingCaptureRecord(recordedSession());

  // Then counts derive from totals and the preview trigger position
  assert.equal(record.id, 42);
  assert.equal(record.trigger, "manual");
  assert.equal(record.source, "5v_out");
  assert.equal(record.edge, "either");
  assert.equal(record.preSamples, 1);
  assert.equal(record.postSamples, 2);
  assert.equal(record.triggerOffset, 1);
  assert.equal(record.sampleCount, 4);
  assert.equal(record.droppedSamples, 0);
  assert.equal(record.triggerDeviceTimeUs, 1_010_000);
  assert.equal(record.incomplete, false);
  assert.equal(record.interruptionReason, undefined);
  assert.deepEqual(record.summaries, {});
  assert.notEqual(record.archiveId, undefined);
});

test("streamingCaptureRecord marks dropped-sample records incomplete with the exact reason", () => {
  // Given a session where the firmware reported dropped samples
  // When the public record is built
  const record = streamingCaptureRecord(recordedSession({ droppedSamples: 3 }));

  // Then the record is incomplete with the firmware-drop explanation
  assert.equal(record.incomplete, true);
  assert.equal(record.interruptionReason, "The debugger reported 3 dropped samples");
});

test("streamingCaptureRecord prefers an explicit interruption reason over the dropped-sample text", () => {
  // Given a session with both an explicit reason and dropped samples
  // When the public record is built as incomplete
  const record = streamingCaptureRecord(
    recordedSession({ droppedSamples: 2 }),
    true,
    "Live WebSocket disconnected",
  );

  // Then the explicit reason wins
  assert.equal(record.incomplete, true);
  assert.equal(record.interruptionReason, "Live WebSocket disconnected");
});

test("streamingCaptureLease carries owner, lease duration, and drop count", () => {
  // Given a session with dropped samples
  // When the archive lease payload is built
  const lease = streamingCaptureLease(recordedSession({ droppedSamples: 5 }));

  // Then it matches the store lease contract
  assert.deepEqual(lease, {
    ownerId: "owner-1",
    leaseDurationMs: POWER_CAPTURE_LEASE_DURATION_MS,
    droppedSamples: 5,
  });
});
