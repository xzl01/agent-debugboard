import {
  appendPowerCapturePreview,
  appendPowerCaptureSummary,
  createPowerCaptureAccumulator,
  finalizePowerCaptureSummaries,
} from "./powerCapture.ts";
import type { CaptureSample, PowerCapture } from "./types";

const DB_NAME = "radxa-linkr-debugger-power-captures";
const DB_VERSION = 1;
const CAPTURE_STORE = "captures";
const CHUNK_STORE = "chunks";

type CompactSample = [
  offset: number,
  triggered: 0 | 1,
  sampleSequence: number,
  deviceTimeUs: number,
  values: Array<[enabled: 0 | 1, currentUa: number]>,
];

export type PowerCaptureArchiveStatus =
  | "recording"
  | "complete"
  | "interrupted"
  | "failed";

interface CaptureArchiveRecord {
  archiveId: string;
  status: PowerCaptureArchiveStatus;
  chunkCount: number;
  persistedSamples: number;
  estimatedBytes: number;
  lastSequence: number;
  startedAt: number;
  updatedAt: number;
  completedAt?: number;
  error?: string;
  pinned?: boolean;
  ownerId?: string;
  leaseExpiresAt?: number;
  capture: PowerCapture;
}

interface LegacyCaptureChunkRecord {
  archiveId: string;
  index: number;
  channels: string[];
  samples: CompactSample[];
}

interface BinaryCaptureChunkRecordV2 {
  archiveId: string;
  index: number;
  version: 2;
  channels: string[];
  sampleCount: number;
  offsets: Uint32Array;
  flags: Uint8Array;
  sequences: Uint32Array;
  deviceTimesUs: Float64Array;
  enabledMasks: Uint32Array;
  currentsUa: Int32Array;
  estimatedBytes: number;
}

interface CaptureChannelMetadata {
  name: string;
  signal: string;
  sensorChannel: string;
  unit: string;
}

interface BinaryCaptureChunkRecordV3 {
  archiveId: string;
  index: number;
  version: 3;
  channels: CaptureChannelMetadata[];
  sampleCount: number;
  offsets: Uint32Array;
  flags: Uint8Array;
  sequences: Uint32Array;
  deviceTimesUs: Float64Array;
  enabled: Uint8Array;
  rawValid: Uint8Array;
  raw: Int32Array;
  millivolts: Int32Array;
  currentsUa: Int32Array;
  estimatedBytes: number;
}

type CaptureChunkRecord =
  | LegacyCaptureChunkRecord
  | BinaryCaptureChunkRecordV2
  | BinaryCaptureChunkRecordV3;

export interface PowerCaptureChunkWriteResult {
  chunkCount: number;
  persistedSamples: number;
  estimatedBytes: number;
  lastSequence: number;
}

export interface PowerCaptureArchiveLease {
  ownerId: string;
  leaseDurationMs?: number;
  droppedSamples?: number;
}

export interface PowerCaptureStoragePlan {
  sampleCount: number | null;
  projectedBytes: number | null;
  usageBytes: number | null;
  quotaBytes: number | null;
  availableBytes: number | null;
  reserveBytes: number | null;
  sufficient: boolean | null;
  persisted: boolean | null;
}

export interface PowerCaptureArchiveInfo {
  archiveId: string;
  status: PowerCaptureArchiveStatus;
  chunkCount: number;
  persistedSamples: number;
  estimatedBytes: number;
  updatedAt: number;
  pinned: boolean;
  capture: PowerCapture;
  error?: string;
}

const ESTIMATED_SAMPLE_BYTES = 64;
const STORAGE_SAFETY_FACTOR = 1.2;
const MINIMUM_STORAGE_RESERVE_BYTES = 100 * 1024 * 1024;
const STORAGE_RESERVE_RATIO = 0.1;
const EXPORT_CURSOR_BATCH_CHUNKS = 32;
const RECOVERY_PREVIEW_MAX_SAMPLES = 3000;
export const POWER_CAPTURE_LEASE_DURATION_MS = 60_000;

let databasePromise: Promise<IDBDatabase> | null = null;

function openDatabase(): Promise<IDBDatabase> {
  if (typeof indexedDB === "undefined") {
    return Promise.reject(new Error("IndexedDB is unavailable"));
  }
  if (databasePromise) return databasePromise;
  databasePromise = new Promise((resolve, reject) => {
    const request = indexedDB.open(DB_NAME, DB_VERSION);
    request.onupgradeneeded = () => {
      const database = request.result;
      if (!database.objectStoreNames.contains(CAPTURE_STORE)) {
        database.createObjectStore(CAPTURE_STORE, { keyPath: "archiveId" });
      }
      if (!database.objectStoreNames.contains(CHUNK_STORE)) {
        const chunks = database.createObjectStore(CHUNK_STORE, {
          keyPath: ["archiveId", "index"],
        });
        chunks.createIndex("archiveId", "archiveId", { unique: false });
      }
    };
    request.onsuccess = () => {
      const database = request.result;
      database.onversionchange = () => {
        database.close();
        databasePromise = null;
      };
      resolve(database);
    };
    request.onerror = () => {
      databasePromise = null;
      reject(request.error ?? new Error("Failed to open power capture archive"));
    };
    request.onblocked = () => {
      // An IndexedDB upgrade resumes automatically after older tabs close their
      // connections. Keep this Promise pending so callers recover without a
      // page reload instead of caching a permanently rejected open attempt.
    };
  });
  return databasePromise;
}

function transactionComplete(transaction: IDBTransaction): Promise<void> {
  return new Promise((resolve, reject) => {
    transaction.oncomplete = () => resolve();
    transaction.onerror = () => reject(
      transaction.error ?? new Error("Power capture archive transaction failed"),
    );
    transaction.onabort = () => reject(
      transaction.error ?? new Error("Power capture archive transaction was aborted"),
    );
  });
}

function requestResult<T>(request: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error("IndexedDB request failed"));
  });
}

function isBinaryChunkV2(chunk: CaptureChunkRecord): chunk is BinaryCaptureChunkRecordV2 {
  return "version" in chunk && chunk.version === 2;
}

function isBinaryChunkV3(chunk: CaptureChunkRecord): chunk is BinaryCaptureChunkRecordV3 {
  return "version" in chunk && chunk.version === 3;
}

function compactSamples(
  archiveId: string,
  index: number,
  samples: CaptureSample[],
): BinaryCaptureChunkRecordV3 {
  const channels = samples[0]?.readings.map((reading) => ({
    name: reading.name,
    signal: reading.signal,
    sensorChannel: reading.sensor_channel,
    unit: reading.unit,
  })) ?? [];
  const offsets = new Uint32Array(samples.length);
  const flags = new Uint8Array(samples.length);
  const sequences = new Uint32Array(samples.length);
  const deviceTimesUs = new Float64Array(samples.length);
  const valueCount = samples.length * channels.length;
  const enabled = new Uint8Array(valueCount);
  const rawValid = new Uint8Array(valueCount);
  const raw = new Int32Array(valueCount);
  const millivolts = new Int32Array(valueCount);
  const currentsUa = new Int32Array(samples.length * channels.length);

  samples.forEach((sample, sampleIndex) => {
    offsets[sampleIndex] = Math.max(0, Math.round(sample.offset));
    flags[sampleIndex] = sample.triggered ? 1 : 0;
    sequences[sampleIndex] = Math.max(0, Math.round(sample.sampleSequence));
    deviceTimesUs[sampleIndex] = sample.deviceTimeUs;
    channels.forEach((channel, channelIndex) => {
      const reading = sample.readings.find((item) => item.name === channel.name);
      const valueIndex = sampleIndex * channels.length + channelIndex;
      enabled[valueIndex] = reading?.power_enabled ? 1 : 0;
      if (reading?.raw != null) {
        rawValid[valueIndex] = 1;
        raw[valueIndex] = Math.round(reading.raw);
      }
      millivolts[valueIndex] = Math.round(reading?.mv ?? 0);
      currentsUa[valueIndex] = Math.round(reading?.current_ua ?? 0);
    });
  });

  const estimatedBytes = offsets.byteLength + flags.byteLength + sequences.byteLength +
    deviceTimesUs.byteLength + enabled.byteLength + rawValid.byteLength + raw.byteLength +
    millivolts.byteLength + currentsUa.byteLength + channels.reduce(
      (total, channel) => total +
        (channel.name.length + channel.signal.length + channel.sensorChannel.length + channel.unit.length) * 2,
      0,
    ) + 192;
  return {
    archiveId,
    index,
    version: 3,
    channels,
    sampleCount: samples.length,
    offsets,
    flags,
    sequences,
    deviceTimesUs,
    enabled,
    rawValid,
    raw,
    millivolts,
    currentsUa,
    estimatedBytes,
  };
}

function expandChunk(chunk: CaptureChunkRecord): CaptureSample[] {
  if (isBinaryChunkV3(chunk)) {
    return Array.from({ length: chunk.sampleCount }, (_, sampleIndex) => ({
      offset: chunk.offsets[sampleIndex] ?? 0,
      triggered: (chunk.flags[sampleIndex] ?? 0) !== 0,
      sampleSequence: chunk.sequences[sampleIndex] ?? 0,
      deviceTimeUs: chunk.deviceTimesUs[sampleIndex] ?? 0,
      readings: chunk.channels.map((channel, channelIndex) => {
        const valueIndex = sampleIndex * chunk.channels.length + channelIndex;
        return {
          name: channel.name,
          signal: channel.signal,
          power_enabled: (chunk.enabled[valueIndex] ?? 0) !== 0,
          raw: (chunk.rawValid[valueIndex] ?? 0) !== 0 ? chunk.raw[valueIndex] ?? 0 : null,
          mv: chunk.millivolts[valueIndex] ?? 0,
          sensor_channel: channel.sensorChannel,
          unit: channel.unit,
          current_ua: chunk.currentsUa[valueIndex] ?? 0,
        };
      }),
    }));
  }
  if (isBinaryChunkV2(chunk)) {
    return Array.from({ length: chunk.sampleCount }, (_, sampleIndex) => ({
      offset: chunk.offsets[sampleIndex] ?? 0,
      triggered: (chunk.flags[sampleIndex] ?? 0) !== 0,
      sampleSequence: chunk.sequences[sampleIndex] ?? 0,
      deviceTimeUs: chunk.deviceTimesUs[sampleIndex] ?? 0,
      readings: chunk.channels.map((name, channelIndex) => ({
        name,
        signal: "",
        power_enabled: channelIndex < 32 &&
          (((chunk.enabledMasks[sampleIndex] ?? 0) >>> channelIndex) & 1) === 1,
        raw: null,
        mv: 0,
        sensor_channel: "current",
        unit: "uA",
        current_ua: chunk.currentsUa[sampleIndex * chunk.channels.length + channelIndex] ?? 0,
      })),
    }));
  }
  return chunk.samples.map(([offset, triggered, sampleSequence, deviceTimeUs, values]) => ({
    offset,
    triggered: triggered === 1,
    sampleSequence,
    deviceTimeUs,
    readings: chunk.channels.map((name, index) => ({
      name,
      signal: "",
      power_enabled: values[index]?.[0] === 1,
      raw: null,
      mv: 0,
      sensor_channel: "current",
      unit: "uA",
      current_ua: values[index]?.[1] ?? 0,
    })),
  }));
}

function normalizeArchiveRecord(record: Partial<CaptureArchiveRecord> & {
  archiveId: string;
  capture: PowerCapture;
}): CaptureArchiveRecord {
  const timestamp = record.capture.capturedAt || Date.now();
  return {
    archiveId: record.archiveId,
    status: record.status ?? "recording",
    chunkCount: record.chunkCount ?? 0,
    persistedSamples: record.persistedSamples ?? record.capture.sampleCount ?? 0,
    estimatedBytes: record.estimatedBytes ?? 0,
    lastSequence: record.lastSequence ?? 0,
    startedAt: record.startedAt ?? timestamp,
    updatedAt: record.updatedAt ?? timestamp,
    completedAt: record.completedAt,
    error: record.error,
    pinned: record.pinned ?? false,
    ownerId: record.ownerId,
    leaseExpiresAt: record.leaseExpiresAt,
    capture: record.capture,
  };
}

function archiveInfo(record: CaptureArchiveRecord): PowerCaptureArchiveInfo {
  return {
    archiveId: record.archiveId,
    status: record.status,
    chunkCount: record.chunkCount,
    persistedSamples: record.persistedSamples,
    estimatedBytes: record.estimatedBytes,
    updatedAt: record.updatedAt,
    pinned: record.pinned ?? false,
    capture: record.capture,
    error: record.error,
  };
}

function chunkSampleCount(chunk: CaptureChunkRecord | undefined): number {
  if (!chunk) return 0;
  return isBinaryChunkV2(chunk) || isBinaryChunkV3(chunk)
    ? chunk.sampleCount
    : chunk.samples.length;
}

function chunkEstimatedBytes(chunk: CaptureChunkRecord | undefined): number {
  if (!chunk) return 0;
  if (isBinaryChunkV2(chunk) || isBinaryChunkV3(chunk)) return chunk.estimatedBytes;
  return chunk.samples.length * ESTIMATED_SAMPLE_BYTES * 3;
}

function normalizedLeaseDuration(durationMs?: number): number {
  if (!Number.isFinite(durationMs)) return POWER_CAPTURE_LEASE_DURATION_MS;
  return Math.max(5_000, Math.round(durationMs!));
}

function assertArchiveOwner(record: CaptureArchiveRecord, lease?: PowerCaptureArchiveLease): void {
  if (record.ownerId && record.ownerId !== lease?.ownerId) {
    throw new Error(`Power capture archive ${record.archiveId} is owned by another browser session`);
  }
}

function applyArchiveLease(record: CaptureArchiveRecord, lease?: PowerCaptureArchiveLease): void {
  if (!lease) return;
  record.ownerId = lease.ownerId;
  record.updatedAt = Date.now();
  record.leaseExpiresAt = record.updatedAt + normalizedLeaseDuration(lease.leaseDurationMs);
  if (lease.droppedSamples != null) {
    record.capture = {
      ...record.capture,
      droppedSamples: Math.max(
        Number(record.capture.droppedSamples ?? 0),
        Math.max(0, Math.round(lease.droppedSamples)),
      ),
    };
  }
}

function clearArchiveLease(record: CaptureArchiveRecord): void {
  delete record.ownerId;
  delete record.leaseExpiresAt;
}

export async function beginPowerCaptureArchive(
  capture: PowerCapture,
  lease?: PowerCaptureArchiveLease,
): Promise<void> {
  if (!capture.archiveId) throw new Error("Power capture archive id is required");
  const database = await openDatabase();
  const transaction = database.transaction(CAPTURE_STORE, "readwrite");
  const now = Date.now();
  const record: CaptureArchiveRecord = {
    archiveId: capture.archiveId,
    status: "recording",
    chunkCount: 0,
    persistedSamples: 0,
    estimatedBytes: 0,
    lastSequence: 0,
    startedAt: now,
    updatedAt: now,
    pinned: false,
    capture,
  };
  applyArchiveLease(record, lease);
  transaction.objectStore(CAPTURE_STORE).put(record);
  await transactionComplete(transaction);
}

export async function renewPowerCaptureArchiveLease(
  archiveId: string,
  lease: PowerCaptureArchiveLease,
): Promise<void> {
  const database = await openDatabase();
  const transaction = database.transaction(CAPTURE_STORE, "readwrite");
  const completion = transactionComplete(transaction);
  const store = transaction.objectStore(CAPTURE_STORE);
  const rawRecord = await requestResult(
    store.get(archiveId) as IDBRequest<CaptureArchiveRecord | undefined>,
  );
  if (!rawRecord) {
    transaction.abort();
    await completion.catch(() => undefined);
    throw new Error(`Power capture archive ${archiveId} does not exist`);
  }
  const record = normalizeArchiveRecord(rawRecord);
  if (record.status !== "recording") {
    transaction.abort();
    await completion.catch(() => undefined);
    throw new Error(`Power capture archive ${archiveId} is not recording`);
  }
  assertArchiveOwner(record, lease);
  applyArchiveLease(record, lease);
  store.put(record);
  await completion;
}

export async function appendPowerCaptureChunk(
  archiveId: string,
  index: number,
  samples: CaptureSample[],
  lease?: PowerCaptureArchiveLease,
): Promise<PowerCaptureChunkWriteResult> {
  if (samples.length === 0) {
    return { chunkCount: 0, persistedSamples: 0, estimatedBytes: 0, lastSequence: 0 };
  }
  const database = await openDatabase();
  const transaction = database.transaction([CAPTURE_STORE, CHUNK_STORE], "readwrite");
  const completion = transactionComplete(transaction);
  const captureStore = transaction.objectStore(CAPTURE_STORE);
  const chunkStore = transaction.objectStore(CHUNK_STORE);
  const [rawRecord, previousChunk] = await Promise.all([
    requestResult(captureStore.get(archiveId) as IDBRequest<CaptureArchiveRecord | undefined>),
    requestResult(chunkStore.get([archiveId, index]) as IDBRequest<CaptureChunkRecord | undefined>),
  ]);
  if (!rawRecord) {
    transaction.abort();
    await completion.catch(() => undefined);
    throw new Error(`Power capture archive ${archiveId} does not exist`);
  }
  const record = normalizeArchiveRecord(rawRecord);
  if (record.status !== "recording") {
    transaction.abort();
    await completion.catch(() => undefined);
    throw new Error(`Power capture archive ${archiveId} is not recording`);
  }
  assertArchiveOwner(record, lease);

  const compacted = compactSamples(archiveId, index, samples);
  chunkStore.put(compacted);
  const previousSamples = chunkSampleCount(previousChunk);
  const previousBytes = chunkEstimatedBytes(previousChunk);
  record.chunkCount += previousChunk ? 0 : 1;
  record.persistedSamples = Math.max(0, record.persistedSamples - previousSamples + samples.length);
  record.estimatedBytes = Math.max(0, record.estimatedBytes - previousBytes + compacted.estimatedBytes);
  record.lastSequence = Math.max(record.lastSequence, ...samples.map((sample) => sample.sampleSequence));
  record.updatedAt = Date.now();
  record.capture = {
    ...record.capture,
    sampleCount: record.persistedSamples,
  };
  applyArchiveLease(record, lease);
  captureStore.put(record);
  await completion;
  return {
    chunkCount: record.chunkCount,
    persistedSamples: record.persistedSamples,
    estimatedBytes: record.estimatedBytes,
    lastSequence: record.lastSequence,
  };
}

export async function finishPowerCaptureArchive(
  capture: PowerCapture,
  chunkCount: number,
): Promise<PowerCaptureArchiveInfo> {
  if (!capture.archiveId) throw new Error("Power capture archive id is required");
  const database = await openDatabase();
  const transaction = database.transaction(CAPTURE_STORE, "readwrite");
  const completion = transactionComplete(transaction);
  const store = transaction.objectStore(CAPTURE_STORE);
  const rawRecord = await requestResult(
    store.get(capture.archiveId) as IDBRequest<CaptureArchiveRecord | undefined>,
  );
  if (!rawRecord) {
    transaction.abort();
    await completion.catch(() => undefined);
    throw new Error(`Power capture archive ${capture.archiveId} does not exist`);
  }
  const record = normalizeArchiveRecord(rawRecord);
  const expectedSamples = capture.sampleCount ?? record.persistedSamples;
  const persistedComplete = record.chunkCount === chunkCount &&
    record.persistedSamples >= expectedSamples;
  const droppedSamples = Math.max(0, Number(capture.droppedSamples ?? 0));
  const complete = persistedComplete && !capture.incomplete && droppedSamples === 0;
  const now = Date.now();
  record.status = complete ? "complete" : "interrupted";
  record.updatedAt = now;
  record.completedAt = now;
  record.error = complete
    ? undefined
    : capture.interruptionReason ?? (droppedSamples > 0
      ? `The debugger reported ${droppedSamples} dropped samples`
      : "Not every received sample was persisted");
  record.capture = {
    ...capture,
    sampleCount: record.persistedSamples,
    incomplete: !complete || capture.incomplete,
    interruptionReason: complete ? undefined : record.error,
  };
  clearArchiveLease(record);
  store.put(record);
  await completion;
  return archiveInfo(record);
}

export async function interruptPowerCaptureArchive(
  archiveId: string,
  capture: PowerCapture,
  error: string,
  failed = false,
): Promise<PowerCaptureArchiveInfo> {
  const database = await openDatabase();
  const transaction = database.transaction(CAPTURE_STORE, "readwrite");
  const completion = transactionComplete(transaction);
  const store = transaction.objectStore(CAPTURE_STORE);
  const rawRecord = await requestResult(
    store.get(archiveId) as IDBRequest<CaptureArchiveRecord | undefined>,
  );
  if (!rawRecord) {
    transaction.abort();
    await completion.catch(() => undefined);
    throw new Error(`Power capture archive ${archiveId} does not exist`);
  }
  const record = normalizeArchiveRecord(rawRecord);
  const now = Date.now();
  record.status = failed ? "failed" : "interrupted";
  record.updatedAt = now;
  record.completedAt = now;
  record.error = error;
  record.capture = {
    ...capture,
    sampleCount: record.persistedSamples,
    incomplete: true,
    interruptionReason: error,
  };
  clearArchiveLease(record);
  store.put(record);
  await completion;
  return archiveInfo(record);
}

async function readChunkBatch(
  archiveId: string,
  firstIndex: number,
  limit = EXPORT_CURSOR_BATCH_CHUNKS,
): Promise<CaptureChunkRecord[]> {
  const database = await openDatabase();
  const transaction = database.transaction(CHUNK_STORE, "readonly");
  return new Promise((resolve, reject) => {
    const records: CaptureChunkRecord[] = [];
    const request = transaction.objectStore(CHUNK_STORE).openCursor(
      IDBKeyRange.bound(
        [archiveId, firstIndex],
        [archiveId, Number.MAX_SAFE_INTEGER],
      ),
    );
    request.onsuccess = () => {
      const cursor = request.result;
      if (!cursor || records.length >= limit) return;
      records.push(cursor.value as CaptureChunkRecord);
      if (records.length < limit) cursor.continue();
    };
    request.onerror = () => reject(request.error ?? new Error("Failed to read power capture chunk"));
    transaction.onerror = () => reject(
      transaction.error ?? new Error("Power capture archive transaction failed"),
    );
    transaction.onabort = () => reject(
      transaction.error ?? new Error("Power capture archive transaction was aborted"),
    );
    transaction.oncomplete = () => resolve(records);
  });
}

export async function iteratePowerCaptureChunks(
  archiveId: string,
  onChunk: (samples: CaptureSample[], index: number) => void | Promise<void>,
): Promise<void> {
  let firstIndex = 0;
  while (true) {
    const chunks = await readChunkBatch(archiveId, firstIndex);
    if (chunks.length === 0) return;
    for (const chunk of chunks) {
      await onChunk(expandChunk(chunk), chunk.index);
    }
    firstIndex = chunks[chunks.length - 1].index + 1;
    if (chunks.length < EXPORT_CURSOR_BATCH_CHUNKS) return;
  }
}

export async function readPowerCaptureSamples(archiveId: string): Promise<CaptureSample[]> {
  const samples: CaptureSample[] = [];
  await iteratePowerCaptureChunks(archiveId, (chunk) => {
    samples.push(...chunk);
  });
  return samples;
}

export async function listRecentPowerCaptures(limit = 4): Promise<PowerCapture[]> {
  const database = await openDatabase();
  const transaction = database.transaction(CAPTURE_STORE, "readonly");
  const request = transaction.objectStore(CAPTURE_STORE).getAll() as IDBRequest<
    CaptureArchiveRecord[]
  >;
  const [records] = await Promise.all([requestResult(request), transactionComplete(transaction)]);
  return records
    .map(normalizeArchiveRecord)
    .filter((record) => record.status !== "recording")
    .sort((left, right) => right.capture.capturedAt - left.capture.capturedAt)
    .slice(0, Math.max(0, limit))
    .reverse()
    .map((record) => record.capture);
}

interface RecoveredCaptureData {
  sampleCount: number;
  triggerOffset: number;
  samples: CaptureSample[];
  summaries: PowerCapture["summaries"];
}

async function rebuildPowerCaptureData(archiveId: string): Promise<RecoveredCaptureData> {
  const accumulator = createPowerCaptureAccumulator();
  const preview: CaptureSample[] = [];
  let previewStride = 1;
  let sampleCount = 0;
  let triggerOffset = -1;

  await iteratePowerCaptureChunks(archiveId, (samples) => {
    if (triggerOffset < 0) {
      const relativeTrigger = samples.findIndex((sample) => sample.triggered);
      if (relativeTrigger >= 0) triggerOffset = sampleCount + relativeTrigger;
    }
    appendPowerCaptureSummary(accumulator, samples);
    previewStride = appendPowerCapturePreview(
      preview,
      samples,
      previewStride,
      RECOVERY_PREVIEW_MAX_SAMPLES,
    );
    sampleCount += samples.length;
  });

  return {
    sampleCount,
    triggerOffset,
    samples: preview,
    summaries: finalizePowerCaptureSummaries(accumulator),
  };
}

function archiveLeaseIsActive(record: CaptureArchiveRecord, now: number): boolean {
  return record.leaseExpiresAt != null && record.leaseExpiresAt > now;
}

export async function recoverStalePowerCaptureArchives(
  staleAfterMs = 15_000,
  now = Date.now(),
): Promise<PowerCaptureArchiveInfo[]> {
  const database = await openDatabase();
  const listTransaction = database.transaction(CAPTURE_STORE, "readonly");
  const listCompletion = transactionComplete(listTransaction);
  const records = await requestResult(
    listTransaction.objectStore(CAPTURE_STORE).getAll() as IDBRequest<CaptureArchiveRecord[]>,
  );
  await listCompletion;
  const staleBefore = now - Math.max(0, staleAfterMs);
  const recovered: PowerCaptureArchiveInfo[] = [];
  for (const rawRecord of records) {
    const candidate = normalizeArchiveRecord(rawRecord);
    if (
      candidate.status !== "recording" ||
      archiveLeaseIsActive(candidate, now) ||
      (candidate.leaseExpiresAt == null && candidate.updatedAt > staleBefore)
    ) continue;

    const rebuilt = await rebuildPowerCaptureData(candidate.archiveId);
    const transaction = database.transaction(CAPTURE_STORE, "readwrite");
    const completion = transactionComplete(transaction);
    const store = transaction.objectStore(CAPTURE_STORE);
    const latestRaw = await requestResult(
      store.get(candidate.archiveId) as IDBRequest<CaptureArchiveRecord | undefined>,
    );
    if (!latestRaw) {
      await completion;
      continue;
    }
    const record = normalizeArchiveRecord(latestRaw);
    if (
      record.status !== "recording" ||
      record.updatedAt !== candidate.updatedAt ||
      archiveLeaseIsActive(record, now)
    ) {
      await completion;
      continue;
    }
    record.status = "interrupted";
    record.error = "The browser session ended before the recording was finalized";
    record.completedAt = Date.now();
    record.updatedAt = record.completedAt;
    const triggerOffset = rebuilt.triggerOffset >= 0
      ? rebuilt.triggerOffset
      : Math.max(-1, Number(record.capture.triggerOffset ?? -1));
    record.capture = {
      ...record.capture,
      samples: rebuilt.samples,
      sampleCount: rebuilt.sampleCount,
      triggerOffset,
      preSamples: Math.max(0, triggerOffset),
      postSamples: Math.max(0, rebuilt.sampleCount - Math.max(0, triggerOffset) - 1),
      incomplete: true,
      interruptionReason: record.error,
      summaries: rebuilt.summaries,
    };
    record.persistedSamples = rebuilt.sampleCount;
    clearArchiveLease(record);
    store.put(record);
    await completion;
    recovered.push(archiveInfo(record));
  }
  return recovered;
}

export async function deletePowerCaptureArchive(archiveId: string): Promise<void> {
  const database = await openDatabase();
  const transaction = database.transaction([CAPTURE_STORE, CHUNK_STORE], "readwrite");
  transaction.objectStore(CAPTURE_STORE).delete(archiveId);
  const cursorRequest = transaction.objectStore(CHUNK_STORE).index("archiveId")
    .openCursor(IDBKeyRange.only(archiveId));
  cursorRequest.onsuccess = () => {
    const cursor = cursorRequest.result;
    if (!cursor) return;
    cursor.delete();
    cursor.continue();
  };
  await transactionComplete(transaction);
}

export async function clearPowerCaptureArchives(options: {
  activeArchiveId?: string;
  includeRecording?: boolean;
} = {}): Promise<number> {
  const database = await openDatabase();
  const transaction = database.transaction([CAPTURE_STORE, CHUNK_STORE], "readwrite");
  const completion = transactionComplete(transaction);
  const captureStore = transaction.objectStore(CAPTURE_STORE);
  const chunkStore = transaction.objectStore(CHUNK_STORE);
  const records = await requestResult(
    captureStore.getAll() as IDBRequest<CaptureArchiveRecord[]>,
  );
  let removed = 0;
  for (const rawRecord of records) {
    const record = normalizeArchiveRecord(rawRecord);
    if (record.archiveId === options.activeArchiveId) continue;
    if (!options.includeRecording && record.status === "recording") continue;
    if (record.pinned) continue;
    captureStore.delete(record.archiveId);
    const cursorRequest = chunkStore.index("archiveId").openCursor(
      IDBKeyRange.only(record.archiveId),
    );
    cursorRequest.onsuccess = () => {
      const cursor = cursorRequest.result;
      if (!cursor) return;
      cursor.delete();
      cursor.continue();
    };
    removed += 1;
  }
  await completion;
  return removed;
}

export function estimatePowerCaptureBytes(sampleCount: number): number {
  return Math.ceil(
    Math.max(0, sampleCount) * ESTIMATED_SAMPLE_BYTES * STORAGE_SAFETY_FACTOR,
  );
}

export async function getPowerCaptureStoragePlan(config: {
  rateHz: number;
  preSamples: number;
  stopAfterMs?: number;
}): Promise<PowerCaptureStoragePlan> {
  const sampleCount = config.stopAfterMs == null
    ? null
    : Math.max(0, config.preSamples) + 1 + Math.max(
      1,
      Math.round(Math.max(0, config.stopAfterMs) * Math.max(1, config.rateHz) / 1000),
    );
  const projectedBytes = sampleCount == null ? null : estimatePowerCaptureBytes(sampleCount);
  if (typeof navigator === "undefined" || !navigator.storage?.estimate) {
    return {
      sampleCount,
      projectedBytes,
      usageBytes: null,
      quotaBytes: null,
      availableBytes: null,
      reserveBytes: null,
      sufficient: null,
      persisted: null,
    };
  }
  const estimate = await navigator.storage.estimate();
  const usageBytes = estimate.usage ?? null;
  const quotaBytes = estimate.quota ?? null;
  const availableBytes = quotaBytes == null
    ? null
    : Math.max(0, quotaBytes - (usageBytes ?? 0));
  const reserveBytes = quotaBytes == null
    ? null
    : Math.max(MINIMUM_STORAGE_RESERVE_BYTES, quotaBytes * STORAGE_RESERVE_RATIO);
  const sufficient = projectedBytes == null || availableBytes == null || reserveBytes == null
    ? null
    : projectedBytes <= Math.max(0, availableBytes - reserveBytes);
  let persisted: boolean | null = null;
  if (navigator.storage.persisted) {
    persisted = await navigator.storage.persisted().catch(() => false);
  }
  return {
    sampleCount,
    projectedBytes,
    usageBytes,
    quotaBytes,
    availableBytes,
    reserveBytes,
    sufficient,
    persisted,
  };
}

export async function ensurePowerCaptureStorageCapacity(config: {
  rateHz: number;
  preSamples: number;
  stopAfterMs?: number;
}): Promise<PowerCaptureStoragePlan> {
  let plan = await getPowerCaptureStoragePlan(config);
  if (
    plan.quotaBytes != null &&
    plan.usageBytes != null &&
    plan.availableBytes != null
  ) {
    const highWaterBytes = plan.quotaBytes * 0.7;
    const targetUsageBytes = plan.quotaBytes * 0.55;
    const capacityShortfall = plan.projectedBytes == null || plan.reserveBytes == null
      ? 0
      : Math.max(0, plan.projectedBytes + plan.reserveBytes - plan.availableBytes);
    const pressureBytes = plan.usageBytes > highWaterBytes
      ? plan.usageBytes - targetUsageBytes
      : 0;
    const bytesToFree = Math.max(capacityShortfall, pressureBytes);
    if (bytesToFree > 0) {
      const archives = (await listPowerCaptureArchives()).filter(
        (record) => record.status !== "recording" && !record.pinned,
      );
      const retainedBytes = archives.reduce(
        (total, record) => total + record.estimatedBytes,
        0,
      );
      await prunePowerCaptureArchives(Math.max(0, retainedBytes - bytesToFree));
      plan = await getPowerCaptureStoragePlan(config);
    }
  }
  if (plan.sufficient === false) {
    const requiredMiB = Math.ceil((plan.projectedBytes ?? 0) / 1024 / 1024);
    const availableMiB = Math.floor(Math.max(
      0,
      (plan.availableBytes ?? 0) - (plan.reserveBytes ?? 0),
    ) / 1024 / 1024);
    throw new Error(
      `Power capture needs about ${requiredMiB} MiB, but only ${availableMiB} MiB is safely available`,
    );
  }
  if (typeof navigator !== "undefined" && navigator.storage?.persist && plan.persisted === false) {
    await navigator.storage.persist().catch(() => false);
  }
  return plan;
}

export async function listPowerCaptureArchives(): Promise<PowerCaptureArchiveInfo[]> {
  const database = await openDatabase();
  const transaction = database.transaction(CAPTURE_STORE, "readonly");
  const request = transaction.objectStore(CAPTURE_STORE).getAll() as IDBRequest<
    CaptureArchiveRecord[]
  >;
  const [records] = await Promise.all([requestResult(request), transactionComplete(transaction)]);
  return records.map(normalizeArchiveRecord).map(archiveInfo);
}

export async function prunePowerCaptureArchives(
  maximumBytes: number,
  activeArchiveId?: string,
): Promise<number> {
  const records = (await listPowerCaptureArchives())
    .filter((record) => record.archiveId !== activeArchiveId)
    .filter((record) => record.status !== "recording")
    .filter((record) => !record.pinned)
    .sort((left, right) => left.updatedAt - right.updatedAt);
  let totalBytes = records.reduce((total, record) => total + record.estimatedBytes, 0);
  let removed = 0;
  for (const record of records) {
    if (totalBytes <= maximumBytes) break;
    await deletePowerCaptureArchive(record.archiveId);
    totalBytes -= record.estimatedBytes;
    removed += 1;
  }
  return removed;
}
