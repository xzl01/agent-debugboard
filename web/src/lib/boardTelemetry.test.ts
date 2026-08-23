import assert from "node:assert/strict";
import test from "node:test";
import {
  decodeTelemetryReadings,
  decodeTelemetrySamples,
} from "./boardTelemetry.ts";

const CURRENT_CHANNEL = { name: "5v_out", signal: "S_C_5V", kind: "current", unit: "uA" };
const CURRENT_CHANNEL_2 = { name: "12v_out", signal: "S_C_12V", kind: "current", unit: "uA" };
const VOLTAGE_CHANNEL = { name: "adc3", signal: "S_ADC3", kind: "voltage", unit: "uV" };

function compactCurrent(value: number, powerEnabled: boolean) {
  return { ...CURRENT_CHANNEL, value, power_enabled: powerEnabled };
}

function compactVoltage(value: number) {
  return { ...VOLTAGE_CHANNEL, value };
}

test("decodeTelemetryReadings previews a single telemetry frame", () => {
  // Given a single compact telemetry frame with current and voltage readings
  // When preview readings are decoded
  const readings = decodeTelemetryReadings({
    type: "telemetry",
    readings: [compactCurrent(100, true), compactVoltage(3300000)],
  });

  // Then both channels are parsed in order
  assert.deepEqual(readings, [
    { kind: "current", unit: "uA", name: "5v_out", signal: "S_C_5V", value: 100, power_enabled: true },
    { kind: "voltage", unit: "uV", name: "adc3", signal: "S_ADC3", value: 3300000 },
  ]);
});

test("decodeTelemetryReadings previews only the latest batch sample", () => {
  // Given a batch whose mask powers only the second channel
  // When preview readings are decoded
  const readings = decodeTelemetryReadings({
    type: "telemetry-batch",
    channels: [CURRENT_CHANNEL, CURRENT_CHANNEL_2, VOLTAGE_CHANNEL],
    samples: [
      { sample_sequence: 1, values: [10, 20, 30], power_enabled_mask: 0b111 },
      { sample_sequence: 2, values: [40, 50, 60], power_enabled_mask: 0b010 },
    ],
  });

  // Then only the latest sample is decoded and the mask gates current channels
  assert.deepEqual(readings, [
    { kind: "current", unit: "uA", name: "5v_out", signal: "S_C_5V", value: 40, power_enabled: false },
    { kind: "current", unit: "uA", name: "12v_out", signal: "S_C_12V", value: 50, power_enabled: true },
    { kind: "voltage", unit: "uV", name: "adc3", signal: "S_ADC3", value: 60 },
  ]);
});

test("decodeTelemetryReadings returns empty readings for unsupported frames", () => {
  // Given malformed or unknown messages
  // When preview readings are decoded
  // Then every one decodes to an empty list
  assert.deepEqual(decodeTelemetryReadings({ type: "snapshot" }), []);
  assert.deepEqual(decodeTelemetryReadings({ type: "telemetry-batch" }), []);
  assert.deepEqual(decodeTelemetryReadings({ type: "telemetry-batch", samples: "nope" }), []);
  assert.deepEqual(decodeTelemetryReadings({ type: "telemetry-batch", samples: [] }), []);
});

test("decodeTelemetrySamples decodes a single frame with legacy field fallbacks", () => {
  // Given a single frame using only the legacy sequence/uptime field names
  // When capture samples are decoded
  const samples = decodeTelemetrySamples({
    type: "telemetry",
    readings: [compactCurrent(42, true), compactVoltage(3300000)],
    sequence: 9,
    uptime_us: 12345,
  });

  // Then one sample results, voltage is filtered out, and fallbacks apply
  assert.equal(samples.length, 1);
  const sample = samples[0];
  assert.equal(sample?.offset, 0);
  assert.equal(sample?.triggered, false);
  assert.equal(sample?.sampleSequence, 9);
  assert.equal(sample?.deviceTimeUs, 12345);
  assert.deepEqual(sample?.readings, [{
    kind: "current",
    unit: "uA",
    name: "5v_out",
    signal: "S_C_5V",
    value: 42,
    power_enabled: true,
    current_ua: 42,
    raw: null,
    mv: 0,
    sensor_channel: "current",
  }]);
});

test("decodeTelemetrySamples decodes every batch sample in order", () => {
  // Given a batch with three samples and a mask that disables the current rail
  // When capture samples are decoded
  const samples = decodeTelemetrySamples({
    type: "telemetry-batch",
    channels: [CURRENT_CHANNEL, VOLTAGE_CHANNEL],
    samples: [
      { sample_sequence: 7, device_t_mono_us: 7000, values: [70, 700], power_enabled_mask: 0b01 },
      { sequence: 8, uptime_us: 8000, values: [80, 800], power_enabled_mask: 0b00 },
      { sample_sequence: 9, device_t_mono_us: 9000, values: [90, 900], power_enabled_mask: 0b01 },
    ],
  });

  // Then every sample is decoded in order with per-sample fallbacks and mask
  assert.equal(samples.length, 3);
  assert.deepEqual(
    samples.map((sample) => [sample.sampleSequence, sample.deviceTimeUs]),
    [[7, 7000], [8, 8000], [9, 9000]],
  );
  assert.equal(samples[0]?.readings[0]?.power_enabled, true);
  assert.equal(samples[1]?.readings[0]?.power_enabled, false);
  assert.equal(samples[1]?.readings.length, 1);
});

test("decodeTelemetrySamples returns no samples for unsupported frames", () => {
  // Given malformed or unknown messages
  // When capture samples are decoded
  // Then every one yields zero samples
  assert.deepEqual(decodeTelemetrySamples({ type: "snapshot" }), []);
  assert.deepEqual(decodeTelemetrySamples({ type: "telemetry" }), []);
  assert.deepEqual(decodeTelemetrySamples({ type: "telemetry-batch", samples: "nope" }), []);
  assert.deepEqual(decodeTelemetrySamples({ type: "telemetry-batch", samples: [null, 5] }), []);
});
