import assert from "node:assert/strict";
import test from "node:test";
import { IDBKeyRange, indexedDB } from "fake-indexeddb";

import {
  beginPowerCaptureArchive,
  readPowerCaptureSamples,
} from "./powerCaptureStore.ts";
import {
  createStreamingCaptureSession,
  POWER_ARCHIVE_MAX_QUEUED_CHUNKS,
  streamingCaptureLease,
  streamingCaptureRecord,
  type StreamingCaptureSession,
} from "./streamingCaptureModel.ts";
import {
  flushStreamingCaptureChunk,
  queueStreamingLeaseRenewal,
  queueStreamingSamples,
} from "./streamingCapturePersistence.ts";
import type { CaptureConfig, CaptureSample } from "./types.ts";

Object.defineProperty(globalThis, "indexedDB", {
  configurable: true,
  value: indexedDB,
});
Object.defineProperty(globalThis, "IDBKeyRange", {
  configurable: true,
  value: IDBKeyRange,
});

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

function sample(sequence: number, deviceTimeUs = sequence * 10_000): CaptureSample {
  return {
    offset: 0,
    triggered: false,
    sampleSequence: sequence,
    deviceTimeUs,
    readings: [{
      kind: "current",
      unit: "uA",
      name: "5v_out",
      signal: "5v_out-signal",
      value: 1000,
      power_enabled: true,
      current_ua: 1000,
      raw: 123,
      mv: 12,
      sensor_channel: "current",
    }],
  };
}

function samples(from: number, count: number): CaptureSample[] {
  return Array.from({ length: count }, (_, index) => sample(from + index));
}

async function startedSession(): Promise<StreamingCaptureSession> {
  const session = createStreamingCaptureSession(CONFIG, "owner-1");
  session.captureId = 1;
  session.triggered = true;
  session.archiveStarted = true;
  await beginPowerCaptureArchive(streamingCaptureRecord(session), streamingCaptureLease(session));
  session.writeChain = Promise.resolve();
  return session;
}

function recordingWriteError(): { calls: Error[]; onWriteError: (error: Error) => void } {
  const calls: Error[] = [];
  return { calls, onWriteError: (error: Error) => calls.push(error) };
}

test("queueStreamingSamples ignores duplicate and stale sequences", async () => {
  // Given a session that already stored sequence 5
  const session = await startedSession();
  session.lastStoredSequence = 5;
  const { onWriteError } = recordingWriteError();

  // When stale, duplicate, and new sequences arrive together
  queueStreamingSamples(session, samples(4, 4), onWriteError);

  // Then only the new samples are normalized with fresh offsets
  assert.equal(session.totalSamples, 2);
  assert.equal(session.lastStoredSequence, 7);
  assert.equal(session.pendingChunk.length, 2);
  assert.deepEqual(session.pendingChunk.map((entry) => entry.sampleSequence), [6, 7]);
  assert.deepEqual(session.pendingChunk.map((entry) => entry.offset), [0, 1]);
});

test("queueStreamingSamples assigns the trigger to the first sample at the trigger sequence", async () => {
  // Given a session waiting for trigger sequence 10
  const session = await startedSession();
  session.triggerSampleSequence = 10;
  const { onWriteError } = recordingWriteError();

  // When samples straddling the trigger arrive
  queueStreamingSamples(session, samples(9, 3), onWriteError);

  // Then only the first matching sample is marked and the offset is recorded
  assert.equal(session.triggerOffset, 1);
  assert.deepEqual(session.preview.map((entry) => entry.triggered), [false, true, false]);
});

test("queueStreamingSamples falls back to device time when no trigger sequence exists", async () => {
  // Given a session with only a trigger device timestamp
  const session = await startedSession();
  session.triggerDeviceTimeUs = 100_000;
  const { onWriteError } = recordingWriteError();

  // When samples arrive around that timestamp
  queueStreamingSamples(session, [sample(1, 90_000), sample(2, 100_000)], onWriteError);

  // Then the first sample at or after the timestamp carries the trigger
  assert.equal(session.triggerOffset, 1);
  assert.deepEqual(session.preview.map((entry) => entry.triggered), [false, true]);
});

test("queueStreamingSamples persists a 200-sample chunk synchronously ahead of the async write", async () => {
  // Given a recording session
  const session = await startedSession();
  const { onWriteError } = recordingWriteError();

  // When exactly one chunk worth of samples arrives
  queueStreamingSamples(session, samples(1, 200), onWriteError);

  // Then ingestion, summary, preview, and queueing happened synchronously
  assert.equal(session.totalSamples, 200);
  assert.equal(session.preview.length, 200);
  assert.equal(session.pendingChunk.length, 0);
  assert.equal(session.chunkIndex, 1);
  assert.equal(session.queuedChunks, 1);

  // And when the write chain settles, counters reflect the persisted chunk
  await session.writeChain;
  assert.equal(session.queuedChunks, 0);
  assert.equal(session.persistedSamples, 200);
  assert.equal(session.lastPersistedSequence, 200);
  assert.ok(session.persistedBytes > 0);
  const persisted = await readPowerCaptureSamples(session.archiveId);
  assert.equal(persisted.length, 200);
});

test("queueStreamingSamples stops with the exact backpressure error when the queue saturates", async () => {
  // Given a session whose write queue is already at the limit
  const session = await startedSession();
  session.queuedChunks = POWER_ARCHIVE_MAX_QUEUED_CHUNKS;
  const { calls, onWriteError } = recordingWriteError();

  // When another chunk worth of samples arrives
  queueStreamingSamples(session, samples(1, 200), onWriteError);

  // Then recording stops with the documented storage error
  assert.equal(calls.length, 1);
  assert.equal(
    calls[0]?.message,
    "Host storage is not keeping up with the capture stream; recording was stopped before browser memory could grow without limit",
  );
  assert.equal(session.writeError, calls[0]);
  assert.equal(session.queuedChunks, POWER_ARCHIVE_MAX_QUEUED_CHUNKS);

  // And later samples are ignored while the error sticks
  queueStreamingSamples(session, samples(201, 5), onWriteError);
  assert.equal(session.totalSamples, 200);
  assert.equal(calls.length, 1);
});

test("queueStreamingSamples keeps the first write error and reports it once", async () => {
  // Given a session whose archive was never started, so chunk writes fail
  const session = createStreamingCaptureSession(CONFIG, "owner-1");
  const { calls, onWriteError } = recordingWriteError();

  // When a chunk is queued and the write fails
  queueStreamingSamples(session, samples(1, 200), onWriteError);
  await assert.rejects(session.writeChain);

  // Then the first error is retained and surfaced exactly once
  assert.equal(calls.length, 1);
  assert.equal(session.writeError, calls[0]);
  assert.equal(session.queuedChunks, 0);

  // And subsequent ingestion is skipped entirely
  queueStreamingSamples(session, samples(201, 200), onWriteError);
  assert.equal(calls.length, 1);
  assert.equal(session.totalSamples, 200);
});

test("queueStreamingSamples skips ingestion while the session is finishing", async () => {
  // Given a session that is already finalizing
  const session = await startedSession();
  session.finishing = true;
  const { onWriteError } = recordingWriteError();

  // When late samples arrive
  queueStreamingSamples(session, samples(1, 3), onWriteError);

  // Then nothing is ingested
  assert.equal(session.totalSamples, 0);
  assert.equal(session.pendingChunk.length, 0);
});

test("flushStreamingCaptureChunk persists the pending tail and idles on empty or failed sessions", async () => {
  // Given a session with a sub-chunk pending tail
  const session = await startedSession();
  const { onWriteError } = recordingWriteError();
  queueStreamingSamples(session, samples(1, 5), onWriteError);
  assert.equal(session.pendingChunk.length, 5);
  assert.equal(session.chunkIndex, 0);

  // When the final pending chunk is flushed
  flushStreamingCaptureChunk(session);

  // Then the tail is queued immediately with the next chunk index
  assert.equal(session.pendingChunk.length, 0);
  assert.equal(session.chunkIndex, 1);
  assert.equal(session.queuedChunks, 1);
  await session.writeChain;
  assert.equal(session.persistedSamples, 5);
  assert.equal(session.lastPersistedSequence, 5);

  // And a second flush with an empty tail is a no-op
  flushStreamingCaptureChunk(session);
  assert.equal(session.chunkIndex, 1);

  // And a session with a write error never flushes
  session.writeError = new Error("storage failed");
  session.pendingChunk.push(sample(6));
  flushStreamingCaptureChunk(session);
  assert.equal(session.pendingChunk.length, 1);
  assert.equal(session.chunkIndex, 1);
});

test("queueStreamingLeaseRenewal renews only for a live archive past the renewal interval", async () => {
  // Given a session whose archive has not started
  const session = createStreamingCaptureSession(CONFIG, "owner-1");
  const { calls, onWriteError } = recordingWriteError();
  queueStreamingLeaseRenewal(session, onWriteError);
  assert.equal(session.lastLeaseRenewedAt, 0);

  // When the archive is recording and the interval has elapsed
  const started = await startedSession();
  queueStreamingLeaseRenewal(started, onWriteError);
  await started.writeChain;

  // Then the renewal timestamp advances without an error
  const renewedAt = started.lastLeaseRenewedAt;
  assert.ok(renewedAt > 0);
  assert.equal(calls.length, 0);

  // And an immediate second call is held back by the interval gate
  queueStreamingLeaseRenewal(started, onWriteError);
  assert.equal(started.lastLeaseRenewedAt, renewedAt);

  // And a finishing session never renews
  const finishing = await startedSession();
  finishing.finishing = true;
  queueStreamingLeaseRenewal(finishing, onWriteError);
  assert.equal(finishing.lastLeaseRenewedAt, 0);
});
