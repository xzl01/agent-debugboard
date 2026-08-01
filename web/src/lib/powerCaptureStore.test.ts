import assert from "node:assert/strict";
import test from "node:test";
import { IDBKeyRange, indexedDB } from "fake-indexeddb";

import {
  appendPowerCaptureChunk,
  beginPowerCaptureArchive,
  clearPowerCaptureArchives,
  deletePowerCaptureArchive,
  estimatePowerCaptureBytes,
  finishPowerCaptureArchive,
  iteratePowerCaptureChunks,
  listPowerCaptureArchives,
  listRecentPowerCaptures,
  readPowerCaptureSamples,
  recoverStalePowerCaptureArchives,
  renewPowerCaptureArchiveLease,
} from "./powerCaptureStore.ts";
import type { CaptureSample, PowerCapture } from "./types";

Object.defineProperty(globalThis, "indexedDB", {
  configurable: true,
  value: indexedDB,
});
Object.defineProperty(globalThis, "IDBKeyRange", {
  configurable: true,
  value: IDBKeyRange,
});

function sample(offset: number): CaptureSample {
  return {
    offset,
    triggered: offset === 1,
    sampleSequence: 100 + offset,
    deviceTimeUs: 1_000_000 + offset * 10_000,
    readings: ["5v_out", "12v_out", "20v_out"].map((name, index) => ({
      name,
      signal: `${name}-signal`,
      kind: "current",
      power_enabled: index !== 1,
      raw: index === 2 ? null : 123 + index,
      mv: 12 + index,
      sensor_channel: "current",
      unit: "uA",
      current_ua: offset * 1000 + index,
      value: offset * 1000 + index,
    })),
  };
}

function capture(archiveId: string): PowerCapture {
  return {
    id: 7,
    trigger: "manual",
    source: "5v_out",
    edge: "either",
    thresholdUa: 0,
    rateHz: 100,
    preSamples: 1,
    postSamples: 2,
    triggerOffset: 1,
    samples: [],
    capturedAt: 123,
    archiveId,
  };
}

test("stores long capture data in ordered compact chunks", async () => {
  await clearPowerCaptureArchives({ includeRecording: true });
  const metadata = capture("archive-ordered");
  await beginPowerCaptureArchive(metadata);
  const second = await appendPowerCaptureChunk(metadata.archiveId!, 1, [sample(2), sample(3)]);
  const first = await appendPowerCaptureChunk(metadata.archiveId!, 0, [sample(0), sample(1)]);
  await finishPowerCaptureArchive(metadata, 2);

  assert.equal(second.persistedSamples, 2);
  assert.equal(first.persistedSamples, 4);
  assert.equal(first.chunkCount, 2);
  assert.ok(first.estimatedBytes > 0);

  const restored = await readPowerCaptureSamples(metadata.archiveId!);
  assert.deepEqual(restored.map((item) => item.offset), [0, 1, 2, 3]);
  assert.equal(restored[1].triggered, true);
  assert.equal(restored[3].readings[0].current_ua, 3000);
  assert.equal(restored[3].readings[1].power_enabled, false);
  assert.deepEqual(restored[3].readings, sample(3).readings);
  assert.deepEqual((await listRecentPowerCaptures()).map((item) => item.archiveId), [
    "archive-ordered",
  ]);
});

test("iterates archived chunks in order without materializing the whole capture", async () => {
  await clearPowerCaptureArchives({ includeRecording: true });
  const metadata = capture("archive-iterate");
  await beginPowerCaptureArchive(metadata);
  await appendPowerCaptureChunk(metadata.archiveId!, 0, [sample(0), sample(1)]);
  await appendPowerCaptureChunk(metadata.archiveId!, 1, [sample(2)]);

  const batches: number[][] = [];
  await iteratePowerCaptureChunks(metadata.archiveId!, async (samples) => {
    batches.push(samples.map((item) => item.offset));
  });
  assert.deepEqual(batches, [[0, 1], [2]]);
});

test("replacing a chunk keeps persisted sample accounting idempotent", async () => {
  await clearPowerCaptureArchives({ includeRecording: true });
  const metadata = capture("archive-replace");
  await beginPowerCaptureArchive(metadata);
  await appendPowerCaptureChunk(metadata.archiveId!, 0, [sample(0), sample(1)]);
  const result = await appendPowerCaptureChunk(metadata.archiveId!, 0, [sample(2)]);
  assert.equal(result.chunkCount, 1);
  assert.equal(result.persistedSamples, 1);
  assert.deepEqual(
    (await readPowerCaptureSamples(metadata.archiveId!)).map((item) => item.offset),
    [2],
  );
});

test("clear preserves active recordings while removing completed history", async () => {
  await clearPowerCaptureArchives({ includeRecording: true });
  const active = capture("archive-active");
  const completed = capture("archive-completed");
  await beginPowerCaptureArchive(active);
  await beginPowerCaptureArchive(completed);
  await appendPowerCaptureChunk(completed.archiveId!, 0, [sample(0)]);
  await finishPowerCaptureArchive(completed, 1);

  const removed = await clearPowerCaptureArchives({ activeArchiveId: active.archiveId });
  assert.equal(removed, 1);
  assert.deepEqual((await listPowerCaptureArchives()).map((item) => item.archiveId), [
    "archive-active",
  ]);
});

test("recovers stale recording metadata as an exportable interrupted capture", async () => {
  await clearPowerCaptureArchives({ includeRecording: true });
  const metadata = capture("archive-stale");
  await beginPowerCaptureArchive(metadata);
  await appendPowerCaptureChunk(metadata.archiveId!, 0, [sample(0), sample(1)]);
  const recovered = await recoverStalePowerCaptureArchives(0);
  assert.equal(recovered.length, 1);
  assert.equal(recovered[0].status, "interrupted");
  assert.equal(recovered[0].capture.incomplete, true);
  assert.equal(recovered[0].capture.sampleCount, 2);
  assert.equal(recovered[0].capture.samples.length, 2);
  assert.equal(recovered[0].capture.triggerOffset, 1);
  assert.ok((recovered[0].capture.summaries?.["5v_out"]?.wattHours ?? 0) > 0);
  assert.deepEqual((await listRecentPowerCaptures()).map((item) => item.archiveId), [
    "archive-stale",
  ]);
});

test("does not recover an archive while another browser session holds its lease", async () => {
  await clearPowerCaptureArchives({ includeRecording: true });
  const metadata = capture("archive-leased");
  const startedAt = Date.now();
  await beginPowerCaptureArchive(metadata, {
    ownerId: "tab-a",
    leaseDurationMs: 5_000,
  });
  assert.deepEqual(await recoverStalePowerCaptureArchives(0, startedAt + 1_000), []);

  await renewPowerCaptureArchiveLease(metadata.archiveId!, {
    ownerId: "tab-a",
    leaseDurationMs: 5_000,
  });
  assert.deepEqual(await recoverStalePowerCaptureArchives(0, Date.now() + 1_000), []);
  const recovered = await recoverStalePowerCaptureArchives(0, Date.now() + 6_000);
  assert.equal(recovered.length, 1);
  assert.equal(recovered[0].status, "interrupted");
});

test("rejects writes from a browser session that does not own the archive", async () => {
  await clearPowerCaptureArchives({ includeRecording: true });
  const metadata = capture("archive-owned");
  await beginPowerCaptureArchive(metadata, { ownerId: "tab-a" });
  await assert.rejects(
    appendPowerCaptureChunk(metadata.archiveId!, 0, [sample(0)], { ownerId: "tab-b" }),
    /owned by another browser session/,
  );
  const written = await appendPowerCaptureChunk(
    metadata.archiveId!,
    0,
    [sample(0)],
    { ownerId: "tab-a" },
  );
  assert.equal(written.persistedSamples, 1);
});

test("marks a persisted capture with dropped samples as incomplete", async () => {
  await clearPowerCaptureArchives({ includeRecording: true });
  const metadata = capture("archive-dropped");
  await beginPowerCaptureArchive(metadata);
  await appendPowerCaptureChunk(metadata.archiveId!, 0, [sample(0), sample(1)]);
  const archived = await finishPowerCaptureArchive({
    ...metadata,
    sampleCount: 2,
    droppedSamples: 3,
  }, 1);
  assert.equal(archived.status, "interrupted");
  assert.equal(archived.capture.incomplete, true);
  assert.match(archived.capture.interruptionReason ?? "", /3 dropped samples/);
});

test("estimates bounded binary archive capacity with a safety margin", () => {
  assert.equal(estimatePowerCaptureBytes(0), 0);
  assert.equal(estimatePowerCaptureBytes(1000), 76_800);
});

test("continues to read version 2 chunks without inventing ADC diagnostics", async () => {
  await clearPowerCaptureArchives({ includeRecording: true });
  const metadata = capture("archive-v2");
  await beginPowerCaptureArchive(metadata);

  const database = await new Promise<IDBDatabase>((resolve, reject) => {
    const request = indexedDB.open("radxa-linkr-debugger-power-captures", 1);
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });
  const transaction = database.transaction("chunks", "readwrite");
  transaction.objectStore("chunks").put({
    archiveId: metadata.archiveId,
    index: 0,
    version: 2,
    channels: ["5v_out"],
    sampleCount: 1,
    offsets: new Uint32Array([7]),
    flags: new Uint8Array([1]),
    sequences: new Uint32Array([107]),
    deviceTimesUs: new Float64Array([1_070_000]),
    enabledMasks: new Uint32Array([1]),
    currentsUa: new Int32Array([12_345]),
    estimatedBytes: 64,
  });
  await new Promise<void>((resolve, reject) => {
    transaction.oncomplete = () => resolve();
    transaction.onerror = () => reject(transaction.error);
    transaction.onabort = () => reject(transaction.error);
  });
  database.close();

  const [restored] = await readPowerCaptureSamples(metadata.archiveId!);
  assert.equal(restored.sampleSequence, 107);
  assert.equal(restored.readings[0].current_ua, 12_345);
  assert.equal(restored.readings[0].raw, null);
  assert.equal(restored.readings[0].mv, 0);
  assert.equal(restored.readings[0].signal, "");
});

test("deletes capture metadata and all raw chunks", async () => {
  const metadata = capture("archive-delete");
  await beginPowerCaptureArchive(metadata);
  await appendPowerCaptureChunk(metadata.archiveId!, 0, [sample(0)]);
  await deletePowerCaptureArchive(metadata.archiveId!);
  assert.deepEqual(await readPowerCaptureSamples(metadata.archiveId!), []);
});
