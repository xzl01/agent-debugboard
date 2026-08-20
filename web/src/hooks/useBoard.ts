import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import * as api from "@/lib/api";
import {
  isCurrentAdcReading,
  parseCaptureCurrentReadings,
  parseCompactAdcReadings,
  parseHttpAdcReadings,
} from "@/lib/adc";
import { parseSwitches } from "@/lib/switches";
import { mergePersistentConfigSummary } from "@/lib/persistentConfig";
import { persistentConfigCurrentStateKey } from "@/lib/persistentConfigCurrentStateKey";
import type {
  AdcReading,
  Availability,
  BoardSnapshot,
  BoardMonitoring,
  CaptureConfig,
  CaptureSample,
  PowerCapture,
  MemoryPressureSnapshot,
  PowerOutput,
  SafeGpio,
  WatchdogStatus,
} from "@/lib/types";
import {
  appendPowerCapturePreview,
  appendPowerCaptureSummary,
  createPowerCaptureAccumulator,
  finalizePowerCaptureSummaries,
  MAX_POWER_CAPTURE_PRE_TRIGGER_SAMPLES,
  POWER_CAPTURE_PROTOCOL,
  type PowerCaptureAccumulator,
} from "@/lib/powerCapture";
import {
  appendPowerCaptureChunk,
  beginPowerCaptureArchive,
  clearPowerCaptureArchives,
  deletePowerCaptureArchive,
  ensurePowerCaptureStorageCapacity,
  finishPowerCaptureArchive,
  interruptPowerCaptureArchive,
  listRecentPowerCaptures,
  POWER_CAPTURE_LEASE_DURATION_MS,
  recoverStalePowerCaptureArchives,
  renewPowerCaptureArchiveLease,
} from "@/lib/powerCaptureStore";
import {
  createPowerCaptureStopHandshake,
  type PowerCaptureStopHandshake,
} from "@/lib/powerCaptureStop";

const EMPTY: BoardSnapshot = {
  powerOutputs: [],
  switches: {},
  gpios: [],
  watchdog: {
    supported: false,
    automatic: false,
    healthy: false,
    armed: false,
    timeout_ms: 0,
    bootloader_on_timeout: false,
    failing_service: "",
  },
  monitoring: {
    temperature: { available: false },
    heap: { available: false },
    runtime: { available: false },
    cpu: { available: false },
  },
  adc: [],
};

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

function mergeAvailability<T extends Availability>(
  prev: T,
  raw: unknown,
  defaults?: Partial<T>
): T {
  if (!isRecord(raw)) {
    return prev;
  }

  return {
    ...defaults,
    ...prev,
    ...raw,
  } as T;
}

function parseMemoryPressure(
  raw: unknown,
  prev?: MemoryPressureSnapshot
): MemoryPressureSnapshot | undefined {
  if (!isRecord(raw)) {
    return prev;
  }

  return mergeAvailability(prev ?? { available: false }, raw);
}

function parseMonitoringMemory(
  raw: unknown,
  prev: BoardMonitoring["memory"]
): BoardMonitoring["memory"] {
  if (!isRecord(raw)) {
    return prev;
  }

  const next = mergeAvailability(prev ?? { available: false }, raw);
  const physical = isRecord(raw.physical)
    ? {
        ...prev?.physical,
        ...raw.physical,
      }
    : prev?.physical;
  const stacks = isRecord(raw.stacks)
    ? {
        ...prev?.stacks,
        ...raw.stacks,
      }
    : prev?.stacks;
  const current_pressure = parseMemoryPressure(raw.current_pressure, prev?.current_pressure);
  const peak_pressure = parseMemoryPressure(raw.peak_pressure, prev?.peak_pressure);

  return {
    ...next,
    physical,
    stacks,
    current_pressure,
    peak_pressure,
  };
}

function parseMonitoring(raw: unknown, prev: BoardMonitoring = EMPTY.monitoring): BoardMonitoring {
  if (!isRecord(raw)) return prev;
  return {
    temperature: mergeAvailability(prev.temperature, raw.temperature, { available: false }),
    heap: mergeAvailability(prev.heap, raw.heap, { available: false }),
    memory: parseMonitoringMemory(raw.memory, prev.memory),
    runtime: mergeAvailability(prev.runtime, raw.runtime, { available: false }),
    cpu: mergeAvailability(prev.cpu, raw.cpu, { available: false }),
  };
}

function parseWatchdog(raw: unknown, prev: WatchdogStatus = EMPTY.watchdog): WatchdogStatus {
  if (!isRecord(raw)) return prev;

  return {
    supported: typeof raw.supported === "boolean" ? raw.supported : prev.supported,
    automatic: typeof raw.automatic === "boolean" ? raw.automatic : prev.automatic,
    healthy: typeof raw.healthy === "boolean" ? raw.healthy : prev.healthy,
    armed: typeof raw.armed === "boolean" ? raw.armed : prev.armed,
    timeout_ms: typeof raw.timeout_ms === "number" ? raw.timeout_ms : prev.timeout_ms,
    bootloader_on_timeout:
      typeof raw.bootloader_on_timeout === "boolean"
        ? raw.bootloader_on_timeout
        : prev.bootloader_on_timeout,
    failing_service:
      typeof raw.failing_service === "string" ? raw.failing_service : prev.failing_service,
  };
}

function mapStatus(status: unknown, adc: readonly AdcReading[]): BoardSnapshot {
  const record = isRecord(status) ? status : {};
  const rawOutputs = Array.isArray(record.power_outputs)
    ? record.power_outputs.filter(isRecord)
    : [];
  const powerOutputs: PowerOutput[] = rawOutputs.map((o) => ({
    name: typeof o.name === "string" ? o.name : "",
    signal: typeof o.signal === "string" ? o.signal : undefined,
    gp: typeof o.gp === "number" ? o.gp : undefined,
    controllable: typeof o.controllable === "boolean" ? o.controllable : false,
    state: typeof o.state === "string" ? o.state : "",
    value: typeof o.value === "number" ? o.value : null,
  }));

  const rawGpios = Array.isArray(record.gpios) ? record.gpios.filter(isRecord) : [];
  const gpios: SafeGpio[] = rawGpios.map((g) => ({
    name: typeof g.name === "string" ? g.name : "",
    pin: typeof g.pin === "number" ? g.pin : 0,
    note: typeof g.note === "string" ? g.note : "",
    value: typeof g.value === "number" ? g.value : 0,
    direction: typeof g.direction === "string" ? g.direction : "input",
    layoutGroup: typeof g.layoutGroup === "string" ? g.layoutGroup : undefined,
    layoutLabel: typeof g.layoutLabel === "string" ? g.layoutLabel : undefined,
    layoutRow: typeof g.layoutRow === "number" ? g.layoutRow : undefined,
    layoutColumn: typeof g.layoutColumn === "number" ? g.layoutColumn : undefined,
  }));

  const switches = parseSwitches(record.switches);

  const config = mergePersistentConfigSummary(undefined, record.config);
  return {
    mcu: typeof record.mcu === "string" ? record.mcu : undefined,
    usb: typeof record.usb === "string" ? record.usb : undefined,
    powerCaptureProtocol:
      typeof record.power_capture_protocol === "string"
        ? record.power_capture_protocol
        : undefined,
    powerOutputs,
    switches,
    gpios,
    watchdog: parseWatchdog(record.watchdog, EMPTY.watchdog),
    monitoring: parseMonitoring(record.board_monitoring, EMPTY.monitoring),
    adc,
    ...(config ? { config } : {}),
  };
}

// Merge a WebSocket "snapshot" message into the previous snapshot, preserving
// metadata (signal/gp/controllable) we only learned from the HTTP status poll.
function mergeWsSnapshot(prev: BoardSnapshot, msg: unknown): BoardSnapshot {
  const meta = new Map(prev.powerOutputs.map((o) => [o.name, o]));
  const record = isRecord(msg) ? msg : {};
  const powerOutputs: PowerOutput[] = Array.isArray(record.power_outputs)
    ? record.power_outputs.filter(isRecord).map((o) => ({
        name: typeof o.name === "string" ? o.name : "",
        signal: meta.get(typeof o.name === "string" ? o.name : "")?.signal,
        gp: meta.get(typeof o.name === "string" ? o.name : "")?.gp,
        controllable:
          meta.get(typeof o.name === "string" ? o.name : "")?.controllable ?? true,
        state: typeof o.state === "string" ? o.state : "",
        value: typeof o.value === "number" ? o.value : null,
      }))
    : prev.powerOutputs;

  const gpios: SafeGpio[] = Array.isArray(record.gpios)
    ? record.gpios.filter(isRecord).map((g) => ({
        name: typeof g.name === "string" ? g.name : "",
        pin: typeof g.pin === "number" ? g.pin : 0,
        note: typeof g.note === "string" ? g.note : "",
        value: typeof g.value === "number" ? g.value : 0,
        direction: typeof g.direction === "string" ? g.direction : "input",
        layoutGroup: typeof g.layoutGroup === "string" ? g.layoutGroup : undefined,
        layoutLabel: typeof g.layoutLabel === "string" ? g.layoutLabel : undefined,
        layoutRow: typeof g.layoutRow === "number" ? g.layoutRow : undefined,
        layoutColumn: typeof g.layoutColumn === "number" ? g.layoutColumn : undefined,
      }))
    : prev.gpios;

  const config = mergePersistentConfigSummary(prev.config, record.config);
  return {
    ...prev,
    powerCaptureProtocol:
      typeof record.power_capture_protocol === "string"
        ? record.power_capture_protocol
        : prev.powerCaptureProtocol,
    powerOutputs,
    switches: parseSwitches(record.switches, prev.switches),
    gpios,
    watchdog: parseWatchdog(record.watchdog, prev.watchdog),
    monitoring: parseMonitoring(record.board_monitoring, prev.monitoring),
    ...(config ? { config } : {}),
  };
}

export interface UseBoard {
  snapshot: BoardSnapshot;
  persistentConfigCurrentStateKey: string;
  hasData: boolean;
  connected: boolean;
  lastVerifiedAt: number | null;
  error: string | null;
  loading: boolean;
  auto: boolean;
  setAuto: (v: boolean) => void;
  live: boolean;
  setLive: (v: boolean) => void;
  refresh: () => Promise<void>;
  setPower: (name: string, on: boolean) => Promise<void>;
  readPower: (name: string) => Promise<{ state: string; currentUa: number }>;
  setSwitch: (name: string, route: string) => Promise<void>;
  setGpio: (identifier: string, direction: "input" | "output", value?: number) => Promise<void>;
  enterBootloader: () => Promise<void>;
  enterTargetRecovery: (mode: api.TargetRecoveryMode, rail: string) => Promise<void>;
  captureState: "idle" | "connecting" | "armed" | "recording" | "receiving";
  captureProgress: {
    received: number;
    total: number;
    persisted?: number;
    queuedChunks?: number;
    dropped?: number;
  } | null;
  captures: PowerCapture[];
  armCapture: (config: CaptureConfig) => Promise<void>;
  triggerCapture: () => void;
  stopCapture: () => void;
  cancelCapture: () => void;
  clearCaptures: () => void;
}

type CaptureBuilder = Omit<PowerCapture, "samples" | "capturedAt"> & {
  samples: CaptureSample[];
  expected: number;
};

const TELEMETRY_STREAM_BATCH_SIZE = 20;
const LIVE_TELEMETRY_RATE_HZ = 10;
const LIVE_TELEMETRY_BATCH_SIZE = 2;
const MAX_WEB_STREAMING_RATE_HZ = 500;
const POWER_ARCHIVE_CHUNK_SAMPLES = 200;
const POWER_PREVIEW_MAX_SAMPLES = 3000;
const POWER_ARCHIVE_MAX_QUEUED_CHUNKS = 8;
const POWER_CAPTURE_LEASE_RENEW_INTERVAL_MS = 10_000;

interface StreamingCaptureBuilder {
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

function createArchiveId(): string {
  if (typeof crypto !== "undefined" && typeof crypto.randomUUID === "function") {
    return crypto.randomUUID();
  }
  return `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
}

function compactBatchReadings(
  channels: readonly Record<string, unknown>[],
  sample: Record<string, unknown>,
): readonly AdcReading[] {
  const values = Array.isArray(sample.values) ? sample.values : [];
  const powerEnabledMask = Number(sample.power_enabled_mask ?? 0);
  return parseCompactAdcReadings(channels.map((channel, index) => ({
    ...channel,
    value: values[index],
    ...(channel.kind === "current"
      ? { power_enabled: (powerEnabledMask & (1 << index)) !== 0 }
      : {}),
  })));
}

function telemetryReadings(message: Record<string, unknown>): readonly AdcReading[] {
  if (message.type === "telemetry") {
    return parseCompactAdcReadings(message.readings);
  }
  if (message.type !== "telemetry-batch" || !Array.isArray(message.samples)) return [];

  const channels = Array.isArray(message.channels) ? message.channels.filter(isRecord) : [];
  const latest = message.samples.filter(isRecord).at(-1);
  return latest ? compactBatchReadings(channels, latest) : [];
}

function telemetrySamples(message: Record<string, unknown>): CaptureSample[] {
  if (message.type === "telemetry" && Array.isArray(message.readings)) {
    const readings = parseCompactAdcReadings(message.readings);
    return [{
      offset: 0,
      triggered: false,
      sampleSequence: Number(message.sample_sequence ?? message.sequence ?? 0),
      deviceTimeUs: Number(message.device_t_mono_us ?? message.uptime_us ?? 0),
      readings: parseCaptureCurrentReadings(readings.filter(isCurrentAdcReading).map((reading) => ({
        ...reading,
        current_ua: reading.value,
      }))),
    }];
  }
  if (message.type !== "telemetry-batch" || !Array.isArray(message.samples)) return [];

  const channels = Array.isArray(message.channels) ? message.channels.filter(isRecord) : [];
  return message.samples.filter(isRecord).map((sample) => {
    const readings = compactBatchReadings(channels, sample);
    return {
      offset: 0,
      triggered: false,
      sampleSequence: Number(sample.sample_sequence ?? sample.sequence ?? 0),
      deviceTimeUs: Number(sample.device_t_mono_us ?? sample.uptime_us ?? 0),
      readings: parseCaptureCurrentReadings(readings.filter(isCurrentAdcReading).map((reading) => ({
        ...reading,
        current_ua: reading.value,
      }))),
    };
  });
}

function subscribeMessage(rateHz: number, batchSize: number) {
  return {
    type: "subscribe",
    topic: "live",
    rate_hz: Math.max(1, Math.min(1000, Math.round(rateHz))),
    batch_size: Math.max(1, Math.min(TELEMETRY_STREAM_BATCH_SIZE, Math.round(batchSize))),
    id: "web",
  };
}

function appendCaptureSamples(builder: CaptureBuilder, frames: unknown[]) {
  for (const frame of frames) {
    if (!isRecord(frame)) continue;
    builder.samples.push({
      offset: Number(frame.offset ?? builder.samples.length),
      triggered: frame.triggered === true,
      sampleSequence: Number(frame.sample_sequence ?? 0),
      deviceTimeUs: Number(frame.device_t_mono_us ?? 0),
      readings: parseCaptureCurrentReadings(frame.readings),
    });
  }
}

function captureArmMessage(config: CaptureConfig) {
  return {
    type: "command",
    command: "capture_arm",
    id: "web-capture",
    mode: POWER_CAPTURE_PROTOCOL,
    trigger: config.trigger,
    output: config.trigger === "gpio" ? "" : config.source,
    gpio: config.trigger === "gpio" ? config.source : "",
    edge: config.edge,
    threshold_ua: config.thresholdUa,
    rate_hz: config.rateHz,
    // Streaming mode uses the capture engine only as a precise trigger
    // detector. The complete record flows through telemetry and is persisted
    // by the host, so it is not bounded by firmware capture RAM.
    // These fields are retained only so the Web UI can still arm older
    // firmware. Trigger-only firmware ignores them; the host owns buffering.
    pre_samples: 0,
    post_samples: 1,
  };
}

function streamingCaptureRecord(
  builder: StreamingCaptureBuilder,
  incomplete = false,
  interruptionReason?: string,
): PowerCapture {
  const hasDroppedSamples = builder.droppedSamples > 0;
  const finalIncomplete = incomplete || hasDroppedSamples;
  const finalInterruptionReason = interruptionReason ?? (hasDroppedSamples
    ? `The debugger reported ${builder.droppedSamples} dropped samples`
    : undefined);
  return {
    id: builder.captureId,
    trigger: builder.config.trigger,
    source: builder.config.source,
    edge: builder.config.edge,
    thresholdUa: builder.config.thresholdUa,
    rateHz: builder.config.rateHz,
    preSamples: builder.triggerOffset,
    postSamples: Math.max(0, builder.totalSamples - builder.triggerOffset - 1),
    triggerOffset: builder.preview.findIndex((sample) => sample.triggered),
    samples: builder.preview,
    capturedAt: builder.capturedAt,
    archiveId: builder.archiveId,
    sampleCount: builder.totalSamples,
    droppedSamples: builder.droppedSamples,
    triggerDeviceTimeUs: builder.triggerDeviceTimeUs,
    incomplete: finalIncomplete,
    interruptionReason: finalInterruptionReason,
    summaries: finalizePowerCaptureSummaries(builder.accumulator),
  };
}

function archiveLease(builder: StreamingCaptureBuilder) {
  return {
    ownerId: builder.ownerId,
    leaseDurationMs: POWER_CAPTURE_LEASE_DURATION_MS,
    droppedSamples: builder.droppedSamples,
  };
}

function queueStreamingLeaseRenewal(
  builder: StreamingCaptureBuilder,
  onWriteError: (error: Error) => void,
): void {
  const now = Date.now();
  if (
    !builder.archiveStarted ||
    builder.finishing ||
    builder.writeError ||
    now - builder.lastLeaseRenewedAt < POWER_CAPTURE_LEASE_RENEW_INTERVAL_MS
  ) return;

  builder.lastLeaseRenewedAt = now;
  builder.writeChain = builder.writeChain
    .then(() => renewPowerCaptureArchiveLease(builder.archiveId, archiveLease(builder)))
    .catch((reason: unknown) => {
      const error = reason instanceof Error ? reason : new Error(String(reason));
      if (!builder.writeError) {
        builder.writeError = error;
        onWriteError(error);
      }
      throw error;
    });
}

function queueStreamingSamples(
  builder: StreamingCaptureBuilder,
  incoming: CaptureSample[],
  onWriteError: (error: Error) => void,
): void {
  if (builder.finishing || builder.writeError) return;
  const normalized: CaptureSample[] = [];
  for (const sample of incoming) {
    if (sample.sampleSequence <= builder.lastStoredSequence) continue;
    const offset = builder.totalSamples;
    const triggered = builder.triggerOffset < 0 && (builder.triggerSampleSequence > 0
      ? sample.sampleSequence >= builder.triggerSampleSequence
      : sample.deviceTimeUs >= builder.triggerDeviceTimeUs);
    if (triggered) builder.triggerOffset = offset;
    const next = { ...sample, offset, triggered };
    builder.lastStoredSequence = sample.sampleSequence;
    builder.totalSamples += 1;
    normalized.push(next);
  }
  if (normalized.length === 0) return;

  appendPowerCaptureSummary(builder.accumulator, normalized);
  builder.previewStride = appendPowerCapturePreview(
    builder.preview,
    normalized,
    builder.previewStride,
    POWER_PREVIEW_MAX_SAMPLES,
  );
  builder.pendingChunk.push(...normalized);
  while (builder.pendingChunk.length >= POWER_ARCHIVE_CHUNK_SAMPLES) {
    const chunk = builder.pendingChunk.splice(0, POWER_ARCHIVE_CHUNK_SAMPLES);
    const index = builder.chunkIndex++;
    if (builder.queuedChunks >= POWER_ARCHIVE_MAX_QUEUED_CHUNKS) {
      const error = new Error(
        "Host storage is not keeping up with the capture stream; recording was stopped before browser memory could grow without limit",
      );
      builder.writeError = error;
      onWriteError(error);
      return;
    }
    builder.queuedChunks += 1;
    builder.writeChain = builder.writeChain
      .then(() => appendPowerCaptureChunk(
        builder.archiveId,
        index,
        chunk,
        archiveLease(builder),
      ))
      .then((result) => {
        builder.queuedChunks = Math.max(0, builder.queuedChunks - 1);
        builder.persistedSamples = result.persistedSamples;
        builder.persistedBytes = result.estimatedBytes;
        builder.lastPersistedSequence = result.lastSequence;
      })
      .catch((reason: unknown) => {
        builder.queuedChunks = Math.max(0, builder.queuedChunks - 1);
        const error = reason instanceof Error ? reason : new Error(String(reason));
        if (!builder.writeError) {
          builder.writeError = error;
          onWriteError(error);
        }
        throw error;
      });
  }
}

export function useBoard(): UseBoard {
  const [snapshot, setSnapshot] = useState<BoardSnapshot>(EMPTY);
  const [hasData, setHasData] = useState(false);
  const [connected, setConnected] = useState(true);
  const [lastVerifiedAt, setLastVerifiedAt] = useState<number | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const [auto, setAuto] = useState(true);
  const [live, setLive] = useState(false);
  const [pageVisible, setPageVisible] = useState(
    () => typeof document === "undefined" || !document.hidden
  );
  const [captureState, setCaptureState] = useState<UseBoard["captureState"]>("idle");
  const [captureProgress, setCaptureProgress] = useState<UseBoard["captureProgress"]>(null);
  const [captures, setCaptures] = useState<PowerCapture[]>([]);
  const wsRef = useRef<WebSocket | null>(null);
  const pendingCaptureRef = useRef<CaptureConfig | null>(null);
  const captureBuilderRef = useRef<CaptureBuilder | null>(null);
  const streamingCaptureRef = useRef<StreamingCaptureBuilder | null>(null);
  const captureOwnerRef = useRef(createArchiveId());
  const lastTelemetryPreviewAtRef = useRef(0);
  const lastVerifiedAtRef = useRef<number | null>(null);
  const captureArmPromiseRef = useRef<{
    resolve: () => void;
    reject: (reason: Error) => void;
  } | null>(null);
  const currentStateKey = useMemo(
    () => persistentConfigCurrentStateKey({
      powerOutputs: snapshot.powerOutputs,
      switches: snapshot.switches,
      gpios: snapshot.gpios,
    }),
    [snapshot.gpios, snapshot.powerOutputs, snapshot.switches]
  );

  useEffect(() => {
    let cancelled = false;
    void recoverStalePowerCaptureArchives().then(() => listRecentPowerCaptures()).then((archived) => {
      if (!cancelled && archived.length > 0) setCaptures(archived);
    }).catch(() => {
      // IndexedDB is an optional browser capability. A recording attempt will
      // surface an actionable error if persistent storage is unavailable.
    });
    return () => {
      cancelled = true;
    };
  }, []);

  useEffect(() => {
    if (typeof document === "undefined") return;
    const updateVisibility = () => {
      // Do not interrupt an armed or active capture just because its tab was
      // backgrounded. Once the capture is idle, the hidden tab can release its
      // polling/WebSocket load and reconnect when it becomes visible again.
      setPageVisible(!document.hidden || captureState !== "idle");
    };
    updateVisibility();
    document.addEventListener("visibilitychange", updateVisibility);
    return () => document.removeEventListener("visibilitychange", updateVisibility);
  }, [captureState]);

  const finalizeStreamingCapture = useCallback((
    incomplete = false,
    interruptionReason?: string,
  ): Promise<void> => {
    const builder = streamingCaptureRef.current;
    if (!builder || !builder.triggered) return Promise.resolve();
    if (incomplete) {
      builder.requestedIncomplete = true;
      builder.requestedInterruptionReason ??= interruptionReason;
      builder.stopHandshake?.fail(new Error(
        interruptionReason ?? "Power capture was interrupted while stopping",
      ));
    }
    if (builder.finalizePromise) return builder.finalizePromise;

    builder.finalizePromise = (async () => {
      if (builder.stopTimer) clearTimeout(builder.stopTimer);
      builder.stopTimer = null;
      setCaptureState("receiving");

      const socket = wsRef.current;
      if (socket?.readyState === WebSocket.OPEN) {
        const stopHandshake = createPowerCaptureStopHandshake(`web-stop-${builder.captureId}`);
        builder.stopHandshake = stopHandshake;
        try {
          socket.send(JSON.stringify({
            type: "command",
            command: "capture_stop",
            id: stopHandshake.requestId,
          }));
        } catch (reason) {
          stopHandshake.fail(reason instanceof Error ? reason : new Error(String(reason)));
        }
        try {
          await stopHandshake.promise;
        } catch (reason) {
          const message = reason instanceof Error ? reason.message : String(reason);
          builder.requestedIncomplete = true;
          builder.requestedInterruptionReason ??= message;
        }
      } else {
        builder.requestedIncomplete = true;
        builder.requestedInterruptionReason ??=
          "Live WebSocket disconnected before capture_stop was acknowledged";
      }

      // The ACK closes the capture boundary. Telemetry stays enabled while
      // capture_stop is in flight, then the final tail is flushed before the
      // archive metadata is sealed.
      builder.finishing = true;
      if (!builder.writeError && builder.pendingChunk.length > 0) {
        const chunk = builder.pendingChunk.splice(0);
        const index = builder.chunkIndex++;
        builder.queuedChunks += 1;
        builder.writeChain = builder.writeChain
          .then(() => appendPowerCaptureChunk(
            builder.archiveId,
            index,
            chunk,
            archiveLease(builder),
          ))
          .then((result) => {
            builder.queuedChunks = Math.max(0, builder.queuedChunks - 1);
            builder.persistedSamples = result.persistedSamples;
            builder.persistedBytes = result.estimatedBytes;
            builder.lastPersistedSequence = result.lastSequence;
          })
          .catch((reason: unknown) => {
            builder.queuedChunks = Math.max(0, builder.queuedChunks - 1);
            const error = reason instanceof Error ? reason : new Error(String(reason));
            builder.writeError ??= error;
            throw error;
          });
      }

      try {
        await builder.writeChain;
        if (builder.writeError) throw builder.writeError;
        const completed = streamingCaptureRecord(
          builder,
          builder.requestedIncomplete,
          builder.requestedInterruptionReason,
        );
        const archived = await finishPowerCaptureArchive(completed, builder.chunkIndex);
        setCaptures((previous) => [...previous, archived.capture].slice(-4));
        if (builder.requestedInterruptionReason) {
          setError(builder.requestedInterruptionReason);
        }
      } catch (reason) {
        const message = reason instanceof Error ? reason.message : String(reason);
        const partial = streamingCaptureRecord(
          builder,
          true,
          builder.requestedInterruptionReason ?? message,
        );
        partial.sampleCount = builder.persistedSamples;
        partial.samples = partial.samples.filter(
          (sample) => sample.sampleSequence <= builder.lastPersistedSequence,
        );
        try {
          const archived = await interruptPowerCaptureArchive(
            builder.archiveId,
            partial,
            builder.requestedInterruptionReason ?? message,
            builder.writeError != null,
          );
          setCaptures((previous) => [...previous, archived.capture].slice(-4));
        } catch {
          // Opening the archive itself may have failed. The original storage
          // error is more useful than a secondary metadata update failure.
        }
        setError(message);
      } finally {
        if (streamingCaptureRef.current === builder) streamingCaptureRef.current = null;
        setCaptureProgress(null);
        setCaptureState("idle");
        if (wsRef.current?.readyState === WebSocket.OPEN) {
          wsRef.current.send(JSON.stringify(subscribeMessage(
            LIVE_TELEMETRY_RATE_HZ,
            LIVE_TELEMETRY_BATCH_SIZE,
          )));
        }
      }
    })();
    return builder.finalizePromise;
  }, []);

  const discardStreamingCapture = useCallback(() => {
    const builder = streamingCaptureRef.current;
    if (!builder) return;
    builder.finishing = true;
    builder.stopHandshake?.fail(new Error("Power capture was discarded"));
    if (builder.stopTimer) clearTimeout(builder.stopTimer);
    builder.stopTimer = null;
    streamingCaptureRef.current = null;
    if (builder.archiveStarted) {
      void builder.writeChain
        .then(() => deletePowerCaptureArchive(builder.archiveId))
        .catch(() => undefined);
    }
    if (wsRef.current?.readyState === WebSocket.OPEN) {
      wsRef.current.send(JSON.stringify(subscribeMessage(
        LIVE_TELEMETRY_RATE_HZ,
        LIVE_TELEMETRY_BATCH_SIZE,
      )));
    }
  }, []);

  const resetCapture = useCallback((reason?: Error) => {
    if (captureArmPromiseRef.current) {
      captureArmPromiseRef.current.reject(reason ?? new Error("Power capture arming was cancelled"));
      captureArmPromiseRef.current = null;
    }
    pendingCaptureRef.current = null;
    captureBuilderRef.current = null;
    discardStreamingCapture();
    setCaptureProgress(null);
    setCaptureState("idle");
  }, [discardStreamingCapture]);

  const refresh = useCallback(async () => {
    try {
      const status = await api.getStatus();
      const adcResponse: unknown = await api.getAdc();
      const readings = parseHttpAdcReadings(adcResponse);
      setSnapshot(mapStatus(status, readings));
      setHasData(true);
      setConnected(true);
      const verifiedAt = Date.now();
      lastVerifiedAtRef.current = verifiedAt;
      setLastVerifiedAt(verifiedAt);
      setError(null);
    } catch (e) {
      setConnected(false);
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setLoading(false);
    }
  }, []);

  // Polling mode (default) or WebSocket live mode.
  useEffect(() => {
    if (live && pageVisible) {
      let ws: WebSocket | null = null;
      let cancelled = false;
      let sessionId: number | null = null;
      let sessionReleased = false;

      const releaseSession = async () => {
        if (sessionId == null || sessionReleased) return;
        sessionReleased = true;
        try {
          await api.deleteLiveSession(sessionId);
        } catch {
          // The firmware also releases the session when the socket closes, so
          // a follow-up DELETE can legitimately return unknown_session_id.
        }
      };

      (async () => {
        // Seed metadata from an HTTP poll before switching to live stream.
        try {
          const status = await api.getStatus();
          const adcResponse: unknown = await api.getAdc();
          if (cancelled) return;
          setSnapshot(mapStatus(status, parseHttpAdcReadings(adcResponse)));
          setHasData(true);
          setConnected(true);
          const verifiedAt = Date.now();
          lastVerifiedAtRef.current = verifiedAt;
          setLastVerifiedAt(verifiedAt);
          setError(null);
          setLoading(false);
        } catch (e) {
          setLive(false);
          setConnected(false);
          setError(e instanceof Error ? e.message : String(e));
          return;
        }

        let session: api.LiveSession;
        try {
          session = await api.createLiveSession();
          sessionId = session.session_id;
          if (cancelled) {
            await releaseSession();
            return;
          }
        } catch (e) {
          if (!cancelled) {
            setLive(false);
            setError(e instanceof Error ? e.message : String(e));
          }
          return;
        }

        // Use the session path returned by firmware, but keep the browser
        // connection same-origin so Vite's WebSocket proxy and deployed pages
        // both work without hard-coding the board address.
        ws = new WebSocket(api.liveWebSocketUrl(session.ws_url));
        wsRef.current = ws;
        ws.onopen = () => {
          const pending = pendingCaptureRef.current;
          ws?.send(JSON.stringify(subscribeMessage(
            pending?.streaming ? pending.rateHz : LIVE_TELEMETRY_RATE_HZ,
            pending?.streaming ? TELEMETRY_STREAM_BATCH_SIZE : LIVE_TELEMETRY_BATCH_SIZE,
          )));
          if (pendingCaptureRef.current) {
            ws?.send(JSON.stringify(captureArmMessage(pendingCaptureRef.current)));
          }
        };
        ws.onmessage = (ev) => {
          try {
            const parsed: unknown = typeof ev.data === "string" ? JSON.parse(ev.data) : null;
            if (!isRecord(parsed)) return;
            const msg = parsed;
            if (msg.type === "snapshot") {
              const verifiedAt = Date.now();
              lastVerifiedAtRef.current = verifiedAt;
              setLastVerifiedAt(verifiedAt);
              setSnapshot((prev) => mergeWsSnapshot(prev, msg));
            } else if (msg.type === "telemetry" || msg.type === "telemetry-batch") {
              lastVerifiedAtRef.current = Date.now();
              const samples = telemetrySamples(msg);
              const latestReadings = telemetryReadings(msg);
              const now = performance.now();
              if (latestReadings.length > 0 && now - lastTelemetryPreviewAtRef.current >= 100) {
                lastTelemetryPreviewAtRef.current = now;
                setSnapshot((prev) => ({ ...prev, adc: latestReadings }));
              }

              const streaming = streamingCaptureRef.current;
              if (
                streaming &&
                !streaming.finishing &&
                (streaming.stopHandshake?.acceptsTelemetry ?? true)
              ) {
                streaming.droppedSamples += Math.max(0, Number(msg.dropped_samples ?? 0));
                const stopForStorageError = (storageError: Error) => {
                  setError(storageError.message);
                  void finalizeStreamingCapture(true, storageError.message);
                };
                queueStreamingLeaseRenewal(streaming, stopForStorageError);
                if (!streaming.triggered) {
                  streaming.preBuffer.push(...samples);
                  const keep = Math.max(
                    streaming.config.preSamples + TELEMETRY_STREAM_BATCH_SIZE * 2,
                    TELEMETRY_STREAM_BATCH_SIZE * 2,
                  );
                  if (streaming.preBuffer.length > keep) {
                    streaming.preBuffer.splice(0, streaming.preBuffer.length - keep);
                  }
                } else {
                  queueStreamingSamples(streaming, samples, stopForStorageError);
                  if (now - streaming.lastProgressAt >= 250) {
                    streaming.lastProgressAt = now;
                    const expected = streaming.config.stopAfterMs
                      ? streaming.triggerOffset + 1 + Math.round(
                        streaming.config.stopAfterMs * streaming.config.rateHz / 1000,
                      )
                      : 0;
                    setCaptureProgress({
                      received: streaming.totalSamples,
                      total: expected,
                      persisted: streaming.persistedSamples,
                      queuedChunks: streaming.queuedChunks,
                      dropped: streaming.droppedSamples,
                    });
                  }
                }
              }
            } else if (msg.type === "result" && msg.command === "capture_arm") {
              pendingCaptureRef.current = null;
              setCaptureState("armed");
              captureArmPromiseRef.current?.resolve();
              captureArmPromiseRef.current = null;
            } else if (msg.type === "capture_triggered") {
              const streaming = streamingCaptureRef.current;
              if (streaming) {
                streaming.captureId = Number(msg.capture_id ?? Date.now());
                streaming.capturedAt = Date.now();
                streaming.triggerDeviceTimeUs = Number(msg.device_t_mono_us ?? 0);
                streaming.triggerSampleSequence = Number(msg.sample_sequence ?? 0);
                streaming.droppedSamples = Math.max(
                  streaming.droppedSamples,
                  Math.max(0, Number(msg.dropped_samples ?? 0)),
                );
                streaming.triggered = true;
                streaming.archiveStarted = true;
                const before = streaming.preBuffer
                  .filter((sample) => streaming.triggerSampleSequence > 0
                    ? sample.sampleSequence < streaming.triggerSampleSequence
                    : sample.deviceTimeUs < streaming.triggerDeviceTimeUs)
                  .slice(-streaming.config.preSamples);
                const atOrAfter = streaming.preBuffer.filter(
                  (sample) => streaming.triggerSampleSequence > 0
                    ? sample.sampleSequence >= streaming.triggerSampleSequence
                    : sample.deviceTimeUs >= streaming.triggerDeviceTimeUs,
                );
                streaming.preBuffer = [];
                const initial = streamingCaptureRecord(streaming);
                streaming.lastLeaseRenewedAt = Date.now();
                streaming.writeChain = beginPowerCaptureArchive(initial, archiveLease(streaming));
                queueStreamingSamples(streaming, [...before, ...atOrAfter], (storageError) => {
                  setError(storageError.message);
                  void finalizeStreamingCapture(true, storageError.message);
                });
                setCaptureState("recording");
                setCaptureProgress({
                  received: streaming.totalSamples,
                  total: streaming.config.stopAfterMs
                    ? streaming.triggerOffset + 1 + Math.round(
                      streaming.config.stopAfterMs * streaming.config.rateHz / 1000,
                    )
                    : 0,
                  persisted: streaming.persistedSamples,
                  queuedChunks: streaming.queuedChunks,
                  dropped: streaming.droppedSamples,
                });
                if (streaming.config.stopAfterMs && streaming.config.stopAfterMs > 0) {
                  streaming.stopTimer = setTimeout(
                    () => void finalizeStreamingCapture(),
                    streaming.config.stopAfterMs,
                  );
                }
              } else {
                setCaptureState("recording");
              }
            } else if (msg.type === "result" && msg.command === "capture_stop") {
              const streaming = streamingCaptureRef.current;
              if (streaming?.stopHandshake?.acknowledge(String(msg.id ?? ""))) {
                setCaptureState("receiving");
              }
            } else if (msg.type === "capture_begin" && !streamingCaptureRef.current) {
              captureBuilderRef.current = {
                id: Number(msg.capture_id ?? 0),
                trigger: String(msg.trigger ?? ""),
                source: String(msg.source ?? ""),
                edge: String(msg.edge ?? ""),
                thresholdUa: Number(msg.threshold_ua ?? 0),
                rateHz: Number(msg.rate_hz ?? 0),
                preSamples: Number(msg.pre_samples ?? 0),
                postSamples: Number(msg.post_samples ?? 0),
                triggerOffset: Number(msg.trigger_offset ?? 0),
                expected: Number(msg.sample_count ?? 0),
                samples: [],
              };
              setCaptureState("receiving");
              setCaptureProgress({ received: 0, total: Number(msg.sample_count ?? 0) });
            } else if (msg.type === "capture_sample" && captureBuilderRef.current && !streamingCaptureRef.current) {
              const builder = captureBuilderRef.current;
              appendCaptureSamples(builder, [msg]);
              if (builder.samples.length % 20 === 0 || builder.samples.length === builder.expected) {
                setCaptureProgress({ received: builder.samples.length, total: builder.expected });
              }
            } else if (msg.type === "capture_samples" && captureBuilderRef.current && !streamingCaptureRef.current && Array.isArray(msg.samples)) {
              const builder = captureBuilderRef.current;
              appendCaptureSamples(builder, msg.samples);
              setCaptureProgress({ received: builder.samples.length, total: builder.expected });
            } else if (msg.type === "capture_complete" && captureBuilderRef.current && !streamingCaptureRef.current) {
              const builder = captureBuilderRef.current;
              const completed: PowerCapture = {
                id: builder.id,
                trigger: builder.trigger,
                source: builder.source,
                edge: builder.edge,
                thresholdUa: builder.thresholdUa,
                rateHz: builder.rateHz,
                preSamples: builder.preSamples,
                postSamples: builder.postSamples,
                triggerOffset: builder.triggerOffset,
                samples: builder.samples,
                capturedAt: Date.now(),
              };
              setCaptures((previous) => [...previous, completed].slice(-4));
              captureBuilderRef.current = null;
              setCaptureProgress(null);
              setCaptureState("idle");
            } else if (msg.type === "error" && msg.command === "capture_stop") {
              if (streamingCaptureRef.current) {
                const detail = isRecord(msg.error) ? msg.error : {};
                const stopError = new Error(String(
                  detail.message ?? "Power capture could not be stopped",
                ));
                streamingCaptureRef.current.stopHandshake?.fail(stopError);
                setError(stopError.message);
              }
            } else if (msg.type === "error" && msg.command === "capture") {
              const error = isRecord(msg.error) ? msg.error : null;
              const message = typeof error?.message === "string"
                ? error.message
                : "Power capture failed";
              resetCapture(new Error(message));
              setError(message);
            }
          } catch {
            /* ignore malformed frames */
          }
        };
        ws.onerror = () => {
          if (!cancelled) {
            if (streamingCaptureRef.current?.triggered) {
              void finalizeStreamingCapture(true, "Live WebSocket error");
            } else {
              resetCapture(new Error("Live WebSocket error"));
            }
            setConnected(false);
            setLastVerifiedAt(lastVerifiedAtRef.current);
            setError("Live WebSocket error");
          }
        };
        ws.onclose = () => {
          if (wsRef.current === ws) wsRef.current = null;
          void releaseSession();
          if (!cancelled) {
            if (streamingCaptureRef.current?.triggered) {
              void finalizeStreamingCapture(true, "Live WebSocket disconnected");
            } else {
              resetCapture(new Error("Live WebSocket disconnected"));
            }
            setConnected(false);
            setLastVerifiedAt(lastVerifiedAtRef.current);
            setLive(false);
            setError("Live WebSocket disconnected");
          }
        };
      })();

      return () => {
        cancelled = true;
        if (streamingCaptureRef.current?.triggered) {
          void finalizeStreamingCapture(true, "Live session ended before the recording stopped");
        } else if (!streamingCaptureRef.current?.finishing) {
          resetCapture();
        }
        if (wsRef.current === ws) wsRef.current = null;
        if (ws && ws.readyState < WebSocket.CLOSING) {
          ws.close();
        } else {
          void releaseSession();
        }
      };
    }

    if (!pageVisible) {
      setLoading(false);
      return;
    }
    setLoading(true);
    refresh();
    if (!auto) return;
    const id = setInterval(refresh, 2000);
    return () => clearInterval(id);
  }, [auto, finalizeStreamingCapture, live, pageVisible, refresh, resetCapture]);

  const setPower = useCallback(
    async (name: string, on: boolean) => {
      const response = await api.setPower(name, on);
      const expectedState = on ? "on" : "off";
      if (response?.power_output?.name !== name || response?.power_output?.state !== expectedState) {
        throw new Error(`Power output ${name} did not confirm state ${expectedState}`);
      }
      if (!live) await refresh();
    },
    [live, refresh]
  );

  const readPower = useCallback(async (name: string) => {
    // Keep these reads sequential. The firmware HTTP server has a deliberately
    // small client pool and the live WebSocket already owns one slot; opening
    // two more requests at once can cause a transient connection refusal
    // exactly while a power-cycle task is verifying the shutdown edge.
    const statusResponse: unknown = await api.getStatus();
    const adcResponse: unknown = await api.getAdc();
    const status = isRecord(statusResponse) ? statusResponse : {};
    const output = Array.isArray(status.power_outputs)
      ? status.power_outputs.filter(isRecord).find((item) => item.name === name)
      : undefined;
    const reading = parseHttpAdcReadings(adcResponse)
      .filter(isCurrentAdcReading)
      .find((item) => item.name === name);
    if (!output) throw new Error(`Power output ${name} was not reported by the device`);
    return {
      state: typeof output.state === "string" ? output.state : "",
      currentUa: Math.max(0, reading?.value ?? 0),
    };
  }, []);

  const setSwitch = useCallback(
    async (name: string, route: string) => {
      await api.setSwitch(name, route);
      if (!live) await refresh();
    },
    [live, refresh]
  );

  const setGpio = useCallback(
    async (identifier: string, direction: "input" | "output", value?: number) => {
      await api.setGpio(identifier, direction, value);
      if (!live) await refresh();
    },
    [live, refresh]
  );

  const enterBootloader = useCallback(async () => {
    await api.enterBootloader();
  }, []);

  const enterTargetRecovery = useCallback(
    async (mode: api.TargetRecoveryMode, rail: string) => {
      const response = await api.enterTargetRecovery(mode, rail);
      if (
        response.action !== "enter" ||
        response.mode !== mode ||
        response.rail !== rail ||
        response.release_direction !== "input"
      ) {
        throw new Error("Target recovery response did not confirm the requested safe sequence");
      }
      if (!live) await refresh();
    },
    [live, refresh]
  );

  const armCapture = useCallback(async (config: CaptureConfig) => {
    if (config.streaming) {
      if (snapshot.powerCaptureProtocol !== POWER_CAPTURE_PROTOCOL) {
        const reported = snapshot.powerCaptureProtocol ?? "not reported";
        const message = `Power capture requires firmware protocol ${POWER_CAPTURE_PROTOCOL}; the debugger reported ${reported}`;
        setError(message);
        throw new Error(message);
      }
      if (config.rateHz > MAX_WEB_STREAMING_RATE_HZ) {
        const message = `Continuous Web recording is limited to ${MAX_WEB_STREAMING_RATE_HZ} Hz to keep USB telemetry and the control API responsive`;
        setError(message);
        throw new Error(message);
      }
      if (config.preSamples > MAX_POWER_CAPTURE_PRE_TRIGGER_SAMPLES) {
        const message = `Pre-trigger history is limited to ${MAX_POWER_CAPTURE_PRE_TRIGGER_SAMPLES} samples to protect browser memory`;
        setError(message);
        throw new Error(message);
      }
      try {
        await ensurePowerCaptureStorageCapacity({
          rateHz: config.rateHz,
          preSamples: config.preSamples,
          stopAfterMs: config.stopAfterMs,
        });
      } catch (reason) {
        const message = reason instanceof Error ? reason.message : String(reason);
        setError(message);
        throw reason;
      }
    }
    return new Promise<void>((resolve, reject) => {
    captureArmPromiseRef.current?.reject(new Error("Power capture arming was superseded"));
    captureArmPromiseRef.current = { resolve, reject };
    pendingCaptureRef.current = config;
    captureBuilderRef.current = null;
    streamingCaptureRef.current = config.streaming ? {
      config,
      archiveId: createArchiveId(),
      ownerId: captureOwnerRef.current,
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
    } : null;
    setCaptureProgress(null);
    setCaptureState("connecting");
    setError(null);
    if (wsRef.current?.readyState === WebSocket.OPEN) {
      if (config.streaming) {
        wsRef.current.send(JSON.stringify(subscribeMessage(
          config.rateHz,
          TELEMETRY_STREAM_BATCH_SIZE,
        )));
      }
      wsRef.current.send(JSON.stringify(captureArmMessage(config)));
    } else {
      setLive(true);
    }
    });
  }, [snapshot.powerCaptureProtocol]);

  const triggerCapture = useCallback(() => {
    wsRef.current?.send(JSON.stringify({
      type: "command", command: "capture_trigger", id: "web-trigger",
    }));
  }, []);

  const stopCapture = useCallback(() => {
    if (streamingCaptureRef.current?.triggered) {
      void finalizeStreamingCapture();
      return;
    }
    wsRef.current?.send(JSON.stringify({
      type: "command", command: "capture_stop", id: "web-stop",
    }));
  }, [finalizeStreamingCapture]);

  const cancelCapture = useCallback(() => {
    if (streamingCaptureRef.current) {
      wsRef.current?.send(JSON.stringify({
        type: "command", command: "capture_cancel", id: "web-cancel",
      }));
    }
    resetCapture(new Error("Power capture was cancelled"));
  }, [resetCapture]);

  const clearCaptures = useCallback(() => {
    const activeArchiveId = streamingCaptureRef.current?.archiveId;
    void clearPowerCaptureArchives({ activeArchiveId }).then(() => listRecentPowerCaptures()).then(
      setCaptures,
    ).catch((reason) => {
      setError(reason instanceof Error ? reason.message : String(reason));
    });
  }, []);

  return {
    snapshot,
    persistentConfigCurrentStateKey: currentStateKey,
    hasData,
    connected,
    lastVerifiedAt,
    error,
    loading,
    auto,
    setAuto,
    live,
    setLive,
    refresh,
    setPower,
    readPower,
    setSwitch,
    enterBootloader,
    enterTargetRecovery,
    setGpio,
    captureState,
    captureProgress,
    captures,
    armCapture,
    triggerCapture,
    stopCapture,
    cancelCapture,
    clearCaptures,
  };
}
