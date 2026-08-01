import type {
  AdcReading,
  CaptureCurrentReading,
  CurrentAdcReading,
  VoltageAdcReading,
} from "./types.ts";

type RecordValue = Record<string, unknown>;
type SensorValue = {
  readonly val1: number;
  readonly val2: number;
};

export const MEASUREMENT_HISTORY_LIMIT = 90;

function isRecord(value: unknown): value is RecordValue {
  return typeof value === "object" && value !== null;
}

function isSafeInteger(value: unknown): value is number {
  return typeof value === "number" && Number.isSafeInteger(value);
}

function parseSensorValue(value: unknown): SensorValue | undefined {
  if (!isRecord(value) || !isSafeInteger(value.val1) || !isSafeInteger(value.val2)) {
    return undefined;
  }
  return { val1: value.val1, val2: value.val2 };
}

function httpDiagnostics(record: RecordValue, sensorChannel: "current" | "voltage") {
  const sensorValue = parseSensorValue(record.sensor_value);
  return {
    sensor_channel: sensorChannel,
    ...(record.raw === null || isSafeInteger(record.raw) ? { raw: record.raw } : {}),
    ...(isSafeInteger(record.mv) ? { mv: record.mv } : {}),
    ...(sensorValue ? { sensor_value: sensorValue } : {}),
  };
}

function parseHttpReading(value: unknown): AdcReading | null {
  if (!isRecord(value) || typeof value.name !== "string" || typeof value.signal !== "string") {
    return null;
  }

  if (
    value.sensor_channel === "current" &&
    value.unit === "A" &&
    typeof value.power_enabled === "boolean" &&
    isSafeInteger(value.current_ua)
  ) {
    return {
      kind: "current",
      unit: "uA",
      name: value.name,
      signal: value.signal,
      value: value.current_ua,
      power_enabled: value.power_enabled,
      ...httpDiagnostics(value, "current"),
    };
  }

  const sensorValue = parseSensorValue(value.sensor_value);
  if (value.sensor_channel === "voltage" && value.unit === "V" && sensorValue) {
    const voltageUv = (sensorValue.val1 * 1_000_000) + sensorValue.val2;
    if (!Number.isSafeInteger(voltageUv)) return null;
    return {
      kind: "voltage",
      unit: "uV",
      name: value.name,
      signal: value.signal,
      value: voltageUv,
      ...httpDiagnostics(value, "voltage"),
    };
  }

  return null;
}

function parseCompactReading(value: unknown): AdcReading | null {
  if (
    !isRecord(value) ||
    typeof value.name !== "string" ||
    typeof value.signal !== "string" ||
    !isSafeInteger(value.value)
  ) {
    return null;
  }

  if (
    value.kind === "current" &&
    value.unit === "uA" &&
    typeof value.power_enabled === "boolean"
  ) {
    return {
      kind: "current",
      unit: "uA",
      name: value.name,
      signal: value.signal,
      value: value.value,
      power_enabled: value.power_enabled,
    };
  }

  if (value.kind === "voltage" && value.unit === "uV") {
    return {
      kind: "voltage",
      unit: "uV",
      name: value.name,
      signal: value.signal,
      value: value.value,
    };
  }

  return null;
}

function parseCaptureCurrentReading(value: unknown): CaptureCurrentReading | null {
  if (
    !isRecord(value) ||
    typeof value.name !== "string" ||
    typeof value.power_enabled !== "boolean" ||
    !isSafeInteger(value.current_ua)
  ) {
    return null;
  }

  return {
    kind: "current",
    unit: "uA",
    name: value.name,
    signal: typeof value.signal === "string" ? value.signal : "",
    value: value.current_ua,
    power_enabled: value.power_enabled,
    current_ua: value.current_ua,
    raw: null,
    mv: 0,
    sensor_channel: "current",
  };
}

function parseArray<T>(
  value: unknown,
  parseItem: (item: unknown) => T | null,
): readonly T[] {
  if (!Array.isArray(value)) return [];
  return value.flatMap((item) => {
    const parsed = parseItem(item);
    return parsed === null ? [] : [parsed];
  });
}

export function parseHttpAdcReadings(value: unknown): readonly AdcReading[] {
  if (!isRecord(value)) return [];
  return parseArray(value.readings, parseHttpReading);
}

export function parseCompactAdcReadings(value: unknown): readonly AdcReading[] {
  return parseArray(value, parseCompactReading);
}

export function parseCaptureCurrentReadings(value: unknown): readonly CaptureCurrentReading[] {
  return parseArray(value, parseCaptureCurrentReading);
}

export function appendMeasurementHistory<T>(
  previous: readonly T[],
  sample: T,
): readonly T[] {
  return [...previous, sample].slice(-MEASUREMENT_HISTORY_LIMIT);
}

export function isCurrentAdcReading(reading: AdcReading): reading is CurrentAdcReading {
  return reading.kind === "current";
}

export function isVoltageAdcReading(reading: AdcReading): reading is VoltageAdcReading {
  return reading.kind === "voltage";
}
