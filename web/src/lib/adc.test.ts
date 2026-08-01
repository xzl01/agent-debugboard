import assert from "node:assert/strict";
import test from "node:test";
import {
  appendMeasurementHistory,
  MEASUREMENT_HISTORY_LIMIT,
  parseCaptureCurrentReadings,
  parseCompactAdcReadings,
  parseHttpAdcReadings,
} from "./adc.ts";

test("normalizes HTTP current and ADC3 voltage readings at the boundary", () => {
  const readings = parseHttpAdcReadings({
    readings: [
      {
        name: "5v_out",
        signal: "S_C_5V",
        power_enabled: true,
        raw: 101,
        mv: 250,
        sensor_channel: "current",
        unit: "A",
        sensor_value: { val1: 0, val2: 125_000 },
        current_ua: 125_000,
      },
      {
        name: "adc3",
        signal: "GPIO29_ADC3",
        raw: 2048,
        mv: 1_650,
        sensor_channel: "voltage",
        unit: "V",
        sensor_value: { val1: 1, val2: 650_000 },
      },
    ],
  });

  assert.deepEqual(readings, [
    {
      kind: "current",
      unit: "uA",
      name: "5v_out",
      signal: "S_C_5V",
      value: 125_000,
      power_enabled: true,
      raw: 101,
      mv: 250,
      sensor_channel: "current",
      sensor_value: { val1: 0, val2: 125_000 },
    },
    {
      kind: "voltage",
      unit: "uV",
      name: "adc3",
      signal: "GPIO29_ADC3",
      value: 1_650_000,
      raw: 2048,
      mv: 1_650,
      sensor_channel: "voltage",
      sensor_value: { val1: 1, val2: 650_000 },
    },
  ]);
});

test("accepts only matching compact single-reading kind and unit pairs", () => {
  const readings = parseCompactAdcReadings([
    {
      name: "12v_out",
      signal: "S_C_12V",
      kind: "current",
      unit: "uA",
      power_enabled: false,
      value: 42_000,
    },
    {
      name: "adc3",
      signal: "GPIO29_ADC3",
      kind: "voltage",
      unit: "uV",
      power_enabled: false,
      value: 2_345_678,
    },
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
    {
      kind: "current",
      unit: "uA",
      name: "12v_out",
      signal: "S_C_12V",
      value: 42_000,
      power_enabled: false,
    },
    {
      kind: "voltage",
      unit: "uV",
      name: "adc3",
      signal: "GPIO29_ADC3",
      value: 2_345_678,
    },
  ]);
});

test("keeps power-capture readings current-only", () => {
  const readings = parseCaptureCurrentReadings([
    { name: "20v_out", power_enabled: true, current_ua: 750_000 },
    { name: "adc3", kind: "voltage", unit: "uV", value: 1_800_000 },
  ]);

  assert.deepEqual(readings, [
    {
      kind: "current",
      unit: "uA",
      name: "20v_out",
      signal: "",
      value: 750_000,
      power_enabled: true,
      current_ua: 750_000,
      raw: null,
      mv: 0,
      sensor_channel: "current",
    },
  ]);
});

test("retains exactly the latest 90 measurement samples", () => {
  const history = Array.from({ length: MEASUREMENT_HISTORY_LIMIT + 1 }, (_, index) => index)
    .reduce<readonly number[]>(
      (previous, sample) => appendMeasurementHistory(previous, sample),
      [],
    );

  assert.equal(history.length, 90);
  assert.equal(history[0], 1);
  assert.equal(history.at(-1), 90);
});
