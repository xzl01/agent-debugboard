import {
  isCurrentAdcReading,
  parseCaptureCurrentReadings,
  parseCompactAdcReadings,
} from "./adc.ts";
import type {
  AdcReading,
  CaptureSample,
} from "./types.ts";

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
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

export function decodeTelemetryReadings(
  message: Record<string, unknown>
): readonly AdcReading[] {
  if (message.type === "telemetry") {
    return parseCompactAdcReadings(message.readings);
  }
  if (message.type !== "telemetry-batch" || !Array.isArray(message.samples)) return [];

  const channels = Array.isArray(message.channels) ? message.channels.filter(isRecord) : [];
  const latest = message.samples.filter(isRecord).at(-1);
  return latest ? compactBatchReadings(channels, latest) : [];
}

export function decodeTelemetrySamples(
  message: Record<string, unknown>
): CaptureSample[] {
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
