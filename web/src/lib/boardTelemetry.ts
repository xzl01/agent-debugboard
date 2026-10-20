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
  channels: readonly unknown[],
  sample: Record<string, unknown>,
): readonly AdcReading[] {
  const values = Array.isArray(sample.values) ? sample.values : [];
  const voltagesUv = Array.isArray(sample.voltages_uv) ? sample.voltages_uv : [];
  const hasVoltageData = Array.isArray(sample.voltages_uv) || typeof sample.voltage_valid_mask === "number";
  const powerEnabledMask = Number(sample.power_enabled_mask ?? 0);
  const voltageValidMask = Number(sample.voltage_valid_mask ?? 0);
  let currentIndex = 0;
  return parseCompactAdcReadings(channels.map((channel, index) => {
    // Preserve each descriptor's value slot even when another descriptor is malformed.
    if (!isRecord(channel)) return null;
    const current = channel.kind === "current" ? currentIndex++ : -1;
    return {
      ...channel,
      value: values[index],
      ...(current >= 0
        ? {
            power_enabled: (powerEnabledMask & (1 << current)) !== 0,
            ...(hasVoltageData
              ? {
                  voltage_valid: (voltageValidMask & (1 << current)) !== 0,
                  voltage_uv: voltagesUv[current],
                }
              : {}),
          }
        : {}),
    };
  }));
}

export function decodeTelemetryReadings(
  message: Record<string, unknown>
): readonly AdcReading[] {
  if (message.type === "telemetry") {
    return parseCompactAdcReadings(message.readings);
  }
  if (message.type !== "telemetry-batch" || !Array.isArray(message.samples)) return [];

  const channels = Array.isArray(message.channels) ? message.channels : [];
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

  const channels = Array.isArray(message.channels) ? message.channels : [];
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
