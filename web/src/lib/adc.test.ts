import assert from "node:assert/strict";
import test from "node:test";
import {
  appendMeasurementHistory,
  MEASUREMENT_HISTORY_LIMIT,
  MEASUREMENT_PREVIEW_SAMPLES,
  parseCaptureCurrentReadings,
  parseCompactAdcReadings,
  parseHttpAdcReadings,
} from "./adc.ts";

const currentNames = ["5v_out", "12v_out", "20v_out", "vdd_5v"];
const voltageChannels = [
  { name: "adc0", signal: "GPIO26_ADC0", value: 2_608_696, note: "PD input divider; ADC pad voltage" },
  { name: "adc1", signal: "GPIO27_ADC1", value: 1_100_000 },
  { name: "adc2", signal: "GPIO28_ADC2", value: 2_200_000 },
  { name: "adc3", signal: "GPIO29_ADC3", value: 1_650_000 },
];

test("normalizes all eight HTTP readings without scaling ADC-pad voltage", () => {
  const readings = parseHttpAdcReadings({
    readings: [
      ...currentNames.map((name) => ({
        name,
        signal: `INA226_${name}`,
        power_enabled: true,
        raw: null,
        mv: 5010,
        voltage_valid: true,
        voltage_uv: 5_010_000,
        sensor_channel: "current",
        unit: "A",
        sensor_value: { val1: 0, val2: 125_000 },
        current_ua: 125_000,
      })),
      ...voltageChannels.map(({ value, ...metadata }) => ({
        ...metadata,
        raw: 2048,
        mv: Math.trunc(value / 1000),
        sensor_channel: "voltage",
        unit: "V",
        sensor_value: { val1: Math.trunc(value / 1_000_000), val2: value % 1_000_000 },
      })),
    ],
  });

  assert.deepEqual(readings, [
    ...currentNames.map((name) => ({
      kind: "current",
      unit: "uA",
      name,
      signal: `INA226_${name}`,
      value: 125_000,
      power_enabled: true,
      voltage_valid: true,
      voltage_uv: 5_010_000,
      raw: null,
      mv: 5010,
      sensor_channel: "current",
      sensor_value: { val1: 0, val2: 125_000 },
    })),
    ...voltageChannels.map((channel) => ({
      kind: "voltage",
      unit: "uV",
      ...channel,
      raw: 2048,
      mv: Math.trunc(channel.value / 1000),
      sensor_channel: "voltage",
      sensor_value: { val1: Math.trunc(channel.value / 1_000_000), val2: channel.value % 1_000_000 },
    })),
  ]);
  assert.equal(readings.length, 8);
});

test("accepts all eight compact channels with matching kind and unit pairs", () => {
  const readings = parseCompactAdcReadings([
    ...currentNames.map((name) => ({
      name,
      signal: `INA226_${name}`,
      kind: "current",
      unit: "uA",
      power_enabled: false,
      value: 42_000,
    })),
    ...voltageChannels.map((channel) => ({
      ...channel,
      kind: "voltage",
      unit: "uV",
    })),
    {
      name: "invalid-current",
      signal: "INVALID",
      kind: "current",
      unit: "uV",
      power_enabled: true,
      value: 1,
    },
    {
      name: "invalid-voltage",
      signal: "INVALID",
      kind: "voltage",
      unit: "uA",
      value: 1,
    },
  ]);

  assert.deepEqual(readings, [
    ...currentNames.map((name) => ({
      kind: "current",
      unit: "uA",
      name,
      signal: `INA226_${name}`,
      value: 42_000,
      power_enabled: false,
    })),
    ...voltageChannels.map((channel) => ({
      kind: "voltage",
      unit: "uV",
      ...channel,
    })),
  ]);
});

test("does not turn missing or invalid voltage values into zero or invent unreported channels", () => {
  const channel = { name: "adc3", signal: "GPIO29_ADC3", kind: "voltage", unit: "uV" };
  assert.deepEqual(parseCompactAdcReadings([{ ...channel, value: 0 }]), [{ ...channel, value: 0 }]);
  for (const value of [null, undefined, NaN, Infinity, "1650000", 1.5]) {
    assert.deepEqual(parseCompactAdcReadings([{ ...channel, value }]), []);
  }
  assert.deepEqual(parseHttpAdcReadings({ readings: [{ ...channel, sensor_channel: "voltage", unit: "V", sensor_value: null }] }), []);
  assert.deepEqual(parseHttpAdcReadings({ readings: [] }), []);
});

test("keeps power-capture readings current-only", () => {
  const readings = parseCaptureCurrentReadings([
    ...currentNames.map((name) => ({ name, power_enabled: true, current_ua: 750_000 })),
    ...voltageChannels.map((channel) => ({ ...channel, kind: "voltage", unit: "uV" })),
  ]);

  assert.deepEqual(readings, currentNames.map((name) => ({
      kind: "current",
      unit: "uA",
      name,
      signal: "",
      value: 750_000,
      power_enabled: true,
      current_ua: 750_000,
      raw: null,
      mv: 0,
      sensor_channel: "current",
    })));
});

test("retains a bounded 600-sample browser history independently from the 90-sample preview", () => {
  const history = Array.from({ length: MEASUREMENT_HISTORY_LIMIT + 1 }, (_, index) => index)
    .reduce<readonly number[]>(
      (previous, sample) => appendMeasurementHistory(previous, sample),
      [],
    );

  assert.equal(MEASUREMENT_HISTORY_LIMIT, 600);
  assert.equal(MEASUREMENT_PREVIEW_SAMPLES, 90);
  assert.equal(history.length, MEASUREMENT_HISTORY_LIMIT);
  assert.equal(history[0], 1);
  assert.equal(history.at(-1), MEASUREMENT_HISTORY_LIMIT);
});
