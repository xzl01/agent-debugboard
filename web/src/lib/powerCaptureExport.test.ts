import assert from "node:assert/strict";
import test from "node:test";
import { IDBKeyRange, indexedDB } from "fake-indexeddb";

import { streamPowerCaptureExport } from "./powerCaptureExport.ts";
import {
  appendPowerCaptureChunk,
  beginPowerCaptureArchive,
  clearPowerCaptureArchives,
  finishPowerCaptureArchive,
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
    sampleSequence: offset + 10,
    deviceTimeUs: 1_000_000 + offset * 10_000,
    readings: ["5v_out", "12v_out", "20v_out"].map((name, index) => ({
      name,
      signal: "",
      power_enabled: index === 0,
      raw: null,
      mv: 0,
      sensor_channel: "current",
      unit: "uA",
      current_ua: (offset + 1) * 1000,
    })),
  };
}

function capture(archiveId: string): PowerCapture {
  return {
    id: 42,
    trigger: "manual",
    source: "5v_out",
    edge: "either",
    thresholdUa: 0,
    rateHz: 100,
    preSamples: 1,
    postSamples: 2,
    triggerOffset: 1,
    triggerDeviceTimeUs: 1_010_000,
    samples: [],
    sampleCount: 3,
    capturedAt: Date.now(),
    archiveId,
  };
}

test("streams archived CSV one chunk at a time with progress", async () => {
  await clearPowerCaptureArchives({ includeRecording: true });
  const archived = capture("export-csv");
  await beginPowerCaptureArchive(archived);
  await appendPowerCaptureChunk(archived.archiveId!, 0, [sample(0), sample(1)]);
  await appendPowerCaptureChunk(archived.archiveId!, 1, [sample(2)]);
  await finishPowerCaptureArchive(archived, 2);

  const writes: string[] = [];
  let closed = false;
  const progress: number[] = [];
  const result = await streamPowerCaptureExport(archived, "csv", {
    async write(data) {
      writes.push(data);
    },
    async close() {
      closed = true;
    },
  }, (value) => progress.push(value.writtenSamples));

  const output = writes.join("");
  assert.equal(closed, true);
  assert.equal(result.writtenSamples, 3);
  assert.deepEqual(progress, [2, 3]);
  assert.equal(output.trimEnd().split("\n").length, 4);
  assert.match(output, /capture_id,trigger,source/);
  assert.match(output, /42,manual,5v_out/);
});

test("streams NDJSON without building an all-sample rows array", async () => {
  const archived = capture("export-ndjson");
  await beginPowerCaptureArchive(archived);
  await appendPowerCaptureChunk(archived.archiveId!, 0, [sample(0), sample(1), sample(2)]);
  await finishPowerCaptureArchive(archived, 1);

  const writes: string[] = [];
  await streamPowerCaptureExport(archived, "ndjson", {
    async write(data) {
      writes.push(data);
    },
    async close() {},
  });
  const rows = writes.join("").trimEnd().split("\n").map((line) => JSON.parse(line));
  assert.equal(rows.length, 3);
  assert.deepEqual(rows.map((row) => row.relative_us), [-10_000, 0, 10_000]);
});

test("streams caller metadata for startup-stage exports", async () => {
  const archived = capture("export-stage-metadata");
  await beginPowerCaptureArchive(archived);
  await appendPowerCaptureChunk(archived.archiveId!, 0, [sample(0), sample(1)]);
  await finishPowerCaptureArchive(archived, 1);

  const csvWrites: string[] = [];
  await streamPowerCaptureExport(archived, "csv", {
    async write(data) { csvWrites.push(data); },
    async close() {},
  }, undefined, {
    extraColumns: ["stage", "stage_elapsed_ms"],
    extraValues: (value) => ({
      stage: value.triggered ? "kernel" : "boot",
      stage_elapsed_ms: value.offset * 10,
    }),
  });
  const csv = csvWrites.join("");
  assert.match(csv, /relative_us,stage,stage_elapsed_ms/);
  assert.match(csv, /-10000,boot,0/);
  assert.match(csv, /0,kernel,10/);
});
