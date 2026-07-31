import { nominalVoltage, USER_POWER_RAILS } from "./power.ts";
import { estimatePowerCaptureBytes, iteratePowerCaptureChunks } from "./powerCaptureStore.ts";
import type { CaptureSample, PowerCapture } from "./types";

export type PowerCaptureExportFormat = "csv" | "ndjson";

export interface PowerCaptureExportProgress {
  writtenSamples: number;
  totalSamples: number;
  writtenBytes: number;
}

export interface PowerCaptureExportResult extends PowerCaptureExportProgress {
  fileName: string;
  usedNativeFileWriter: boolean;
}

type ExportScalar = string | number | boolean | null | undefined;

export interface PowerCaptureExportOptions {
  fileName?: string;
  extraColumns?: readonly string[];
  extraValues?: (sample: CaptureSample) => Record<string, ExportScalar>;
}

interface ExportWriter {
  write(data: string): Promise<void>;
  close(): Promise<void>;
  abort?(reason?: unknown): Promise<void>;
}

interface NativeFileWriter {
  write(data: string | Blob): Promise<void>;
  close(): Promise<void>;
  abort(reason?: unknown): Promise<void>;
}

interface NativeFileHandle {
  createWritable(): Promise<NativeFileWriter>;
}

type FilePickerWindow = Window & {
  showSaveFilePicker?: (options: {
    suggestedName: string;
    types: Array<{
      description: string;
      accept: Record<string, string[]>;
    }>;
  }) => Promise<NativeFileHandle>;
};

const FALLBACK_EXPORT_LIMIT_BYTES = 64 * 1024 * 1024;

function csvField(value: unknown): string {
  const text = String(value ?? "");
  return /[",\r\n]/.test(text) ? `"${text.replaceAll('"', '""')}"` : text;
}

function triggerTimeForCapture(capture: PowerCapture): number {
  return capture.triggerDeviceTimeUs ??
    capture.samples[capture.triggerOffset]?.deviceTimeUs ??
    capture.samples.find((sample) => sample.triggered)?.deviceTimeUs ??
    0;
}

function exportRow(
  capture: PowerCapture,
  sample: CaptureSample,
  triggerTime: number,
  options?: PowerCaptureExportOptions,
) {
  const extraValues = options?.extraValues?.(sample) ?? {};
  const extras = Object.fromEntries(
    (options?.extraColumns ?? []).map((column) => [column, extraValues[column]]),
  );
  return {
    capture_id: capture.id,
    trigger: capture.trigger,
    source: capture.source,
    edge: capture.edge,
    threshold_ua: capture.thresholdUa,
    rate_hz: capture.rateHz,
    pre_samples: capture.preSamples,
    post_samples: capture.postSamples,
    offset: sample.offset,
    triggered: sample.triggered,
    device_t_mono_us: sample.deviceTimeUs,
    relative_us: sample.deviceTimeUs - triggerTime,
    sample_sequence: sample.sampleSequence,
    ...extras,
    readings: sample.readings.map((reading) => {
      const voltage = nominalVoltage(reading.name) ?? 0;
      const currentUa = reading.power_enabled ? reading.current_ua : 0;
      return {
        name: reading.name,
        signal: reading.signal,
        sensor_channel: reading.sensor_channel,
        unit: reading.unit,
        power_enabled: reading.power_enabled,
        raw: reading.raw,
        mv: reading.mv,
        reported_current_ua: reading.current_ua,
        current_ua: currentUa,
        current_a: currentUa / 1_000_000,
        nominal_voltage_v: voltage,
        power_w: currentUa / 1_000_000 * voltage,
      };
    }),
  };
}

function csvHeader(options?: PowerCaptureExportOptions): string {
  return [
    "capture_id", "trigger", "source", "edge", "threshold_ua", "rate_hz",
    "pre_samples", "post_samples", "offset", "triggered", "device_t_mono_us", "relative_us",
    ...(options?.extraColumns ?? []),
    ...USER_POWER_RAILS.flatMap((railName) => [
      `${railName}_current_ua`,
      `${railName}_power_w`,
    ]),
    "sample_sequence",
    ...USER_POWER_RAILS.flatMap((railName) => [
      `${railName}_signal`,
      `${railName}_sensor_channel`,
      `${railName}_unit`,
      `${railName}_power_enabled`,
      `${railName}_adc_raw`,
      `${railName}_sense_mv`,
      `${railName}_reported_current_ua`,
      `${railName}_nominal_voltage_v`,
    ]),
  ].map(csvField).join(",") + "\n";
}

function serializeSamples(
  capture: PowerCapture,
  samples: CaptureSample[],
  format: PowerCaptureExportFormat,
  triggerTime: number,
  options?: PowerCaptureExportOptions,
): string {
  if (format === "ndjson") {
    return samples.map((sample) => JSON.stringify(exportRow(capture, sample, triggerTime, options))).join("\n") +
      (samples.length > 0 ? "\n" : "");
  }

  return samples.map((sample) => {
    const row = exportRow(capture, sample, triggerTime, options);
    const values = new Map(row.readings.map((reading) => [reading.name, reading]));
    return [
      row.capture_id,
      row.trigger,
      row.source,
      row.edge,
      row.threshold_ua,
      row.rate_hz,
      row.pre_samples,
      row.post_samples,
      row.offset,
      row.triggered,
      row.device_t_mono_us,
      row.relative_us,
      ...(options?.extraColumns ?? []).map((column) => row[column as keyof typeof row]),
      ...USER_POWER_RAILS.flatMap((railName) => [
        values.get(railName)?.current_ua ?? 0,
        values.get(railName)?.power_w ?? 0,
      ]),
      row.sample_sequence,
      ...USER_POWER_RAILS.flatMap((railName) => [
        values.get(railName)?.signal ?? "",
        values.get(railName)?.sensor_channel ?? "",
        values.get(railName)?.unit ?? "",
        values.get(railName)?.power_enabled ?? false,
        values.get(railName)?.raw ?? null,
        values.get(railName)?.mv ?? 0,
        values.get(railName)?.reported_current_ua ?? 0,
        values.get(railName)?.nominal_voltage_v ?? 0,
      ]),
    ].map(csvField).join(",");
  }).join("\n") + (samples.length > 0 ? "\n" : "");
}

export async function streamPowerCaptureExport(
  capture: PowerCapture,
  format: PowerCaptureExportFormat,
  writer: ExportWriter,
  onProgress?: (progress: PowerCaptureExportProgress) => void,
  options?: PowerCaptureExportOptions,
): Promise<PowerCaptureExportProgress> {
  const totalSamples = capture.sampleCount ?? capture.samples.length;
  const triggerTime = triggerTimeForCapture(capture);
  let writtenSamples = 0;
  let writtenBytes = 0;
  const write = async (text: string) => {
    if (!text) return;
    await writer.write(text);
    writtenBytes += new TextEncoder().encode(text).byteLength;
  };

  try {
    if (format === "csv") await write(csvHeader(options));
    const writeSamples = async (samples: CaptureSample[]) => {
      await write(serializeSamples(capture, samples, format, triggerTime, options));
      writtenSamples += samples.length;
      onProgress?.({ writtenSamples, totalSamples, writtenBytes });
    };

    if (capture.archiveId) {
      await iteratePowerCaptureChunks(capture.archiveId, writeSamples);
    } else {
      await writeSamples(capture.samples);
    }
    await writer.close();
    return { writtenSamples, totalSamples, writtenBytes };
  } catch (error) {
    await writer.abort?.(error).catch(() => undefined);
    throw error;
  }
}

function downloadBlob(fileName: string, parts: BlobPart[], type: string) {
  const url = URL.createObjectURL(new Blob(parts, { type }));
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = fileName;
  anchor.click();
  setTimeout(() => URL.revokeObjectURL(url), 0);
}

export async function exportPowerCaptureToFile(
  capture: PowerCapture,
  format: PowerCaptureExportFormat,
  onProgress?: (progress: PowerCaptureExportProgress) => void,
  options?: PowerCaptureExportOptions,
): Promise<PowerCaptureExportResult> {
  const fileName = options?.fileName ?? `linkr-power-capture-${capture.id}.${format}`;
  const mimeType = format === "csv" ? "text/csv" : "application/x-ndjson";
  const picker = typeof window === "undefined"
    ? undefined
    : (window as FilePickerWindow).showSaveFilePicker;

  if (picker) {
    const handle = await picker.call(window, {
      suggestedName: fileName,
      types: [{
        description: format === "csv" ? "CSV power samples" : "NDJSON power samples",
        accept: { [mimeType]: [`.${format}`] },
      }],
    });
    const nativeWriter = await handle.createWritable();
    const progress = await streamPowerCaptureExport(capture, format, nativeWriter, onProgress, options);
    return { ...progress, fileName, usedNativeFileWriter: true };
  }

  const sampleCount = capture.sampleCount ?? capture.samples.length;
  const conservativeTextBytes = Math.max(
    estimatePowerCaptureBytes(sampleCount),
    sampleCount * 220,
  );
  if (conservativeTextBytes > FALLBACK_EXPORT_LIMIT_BYTES) {
    throw new Error(
      "This browser cannot stream directly to a file. Use a Chromium-based browser for large exports.",
    );
  }

  const parts: string[] = [];
  let bufferedBytes = 0;
  const memoryWriter: ExportWriter = {
    async write(data) {
      bufferedBytes += new TextEncoder().encode(data).byteLength;
      if (bufferedBytes > FALLBACK_EXPORT_LIMIT_BYTES) {
        throw new Error(
          "The export exceeded the safe in-memory limit. Use a Chromium-based browser to stream it directly to a file.",
        );
      }
      parts.push(data);
    },
    async close() {
      downloadBlob(fileName, parts, mimeType);
    },
  };
  const progress = await streamPowerCaptureExport(capture, format, memoryWriter, onProgress, options);
  return { ...progress, fileName, usedNativeFileWriter: false };
}
