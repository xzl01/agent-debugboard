import assert from "node:assert/strict";
import test from "node:test";
import {
  decodeTelemetryReadings,
  decodeTelemetrySamples,
} from "./boardTelemetry.ts";

const CURRENT_CHANNELS = [
  { name: "5v_out", signal: "INA226_5V", kind: "current", unit: "uA" },
  { name: "12v_out", signal: "INA226_12V", kind: "current", unit: "uA" },
  { name: "20v_out", signal: "INA226_20V", kind: "current", unit: "uA" },
  { name: "vdd_5v", signal: "INA226_VDD_5V", kind: "current", unit: "uA" },
];
const VOLTAGE_CHANNELS = [
  { name: "adc0", signal: "GPIO26_ADC0", kind: "voltage", unit: "uV", role: "pd_input" },
  { name: "adc1", signal: "GPIO27_ADC1", kind: "voltage", unit: "uV", role: "native" },
  { name: "adc2", signal: "GPIO28_ADC2", kind: "voltage", unit: "uV", role: "native" },
  { name: "adc3", signal: "GPIO29_ADC3", kind: "voltage", unit: "uV", role: "native" },
];
const CHANNELS = [...CURRENT_CHANNELS, ...VOLTAGE_CHANNELS];
const VALUES = [100, -200, 300, 400, 20_000_000, 1_100_000, 2_200_000, 1_650_000];

function compactReadings() {
  return CHANNELS.map((channel, index) => ({
    ...channel, value: VALUES[index],
    ...(channel.kind === "current" ? { power_enabled: true, voltage_valid: true, voltage_uv: 5_000_000 } : {}),
  }));
}

test("decodeTelemetryReadings previews all eight single-frame descriptors", () => {
  const readings = decodeTelemetryReadings({
    type: "telemetry",
    readings: compactReadings(),
  });

  assert.deepEqual(readings, compactReadings());
  assert.equal(readings.length, 8);
  assert.equal(readings[4].value, 20_000_000);
});

test("decodeTelemetryReadings previews only the latest batch sample", () => {
  const readings = decodeTelemetryReadings({
    type: "telemetry-batch",
    channels: CHANNELS,
    samples: [
      { sample_sequence: 1, values: VALUES.map(() => 0), power_enabled_mask: 0b1111 },
      { sample_sequence: 2, values: VALUES, power_enabled_mask: 0b1010 },
    ],
  });

  assert.deepEqual(readings, CHANNELS.map((channel, index) => ({
    ...channel, value: VALUES[index],
    ...(channel.kind === "current" ? { power_enabled: index === 1 || index === 3 } : {}),
  })));
});

test("decodeTelemetryReadings keeps four rail-voltage slots separate from source/native ADC values", () => {
  const readings = decodeTelemetryReadings({
    type: "telemetry-batch",
    channels: CHANNELS,
    samples: [{
      sample_sequence: 3,
      values: VALUES,
      power_enabled_mask: 0b1111,
      voltage_valid_mask: 0b1001,
      voltages_uv: [5_010_000, 12_020_000, null, 5_000_000],
    }],
  });

  assert.equal(readings.length, 8);
  assert.deepEqual(readings.slice(0, 4).map((reading) => reading.kind === "current" && [
    reading.voltage_valid, reading.voltage_uv,
  ]), [[true, 5_010_000], [false, 12_020_000], [false, undefined], [true, 5_000_000]]);
  assert.deepEqual(readings.slice(4), VOLTAGE_CHANNELS.map((channel, index) => ({ ...channel, value: VALUES[index + 4] })));
  for (const reading of readings.slice(4)) {
    assert.equal("power_enabled" in reading, false);
    assert.equal("voltage_uv" in reading, false);
  }
});

test("missing and null sample values never shift another descriptor or become zero", () => {
  const readings = decodeTelemetryReadings({
    type: "telemetry-batch",
    channels: [...CURRENT_CHANNELS, null, ...VOLTAGE_CHANNELS.slice(1)],
    samples: [{ values: [...VALUES.slice(0, 5), null, 0], power_enabled_mask: 0b1111 }],
  });
  assert.deepEqual(readings.map(({ name, value }) => [name, value]), [
    ["5v_out", 100], ["12v_out", -200], ["20v_out", 300], ["vdd_5v", 400], ["adc2", 0],
  ]);
  assert.deepEqual(decodeTelemetryReadings({
    type: "telemetry", readings: [{ ...VOLTAGE_CHANNELS[3], value: 1_650_000 }],
  }), [{ ...VOLTAGE_CHANNELS[3], value: 1_650_000 }]);
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

test("decodeTelemetrySamples retains only four current channels from an eight-reading frame", () => {
  const samples = decodeTelemetrySamples({
    type: "telemetry",
    readings: compactReadings(),
    sample_sequence: 9,
    device_t_mono_us: 12345,
  });

  assert.equal(samples.length, 1);
  const sample = samples[0];
  assert.equal(sample?.offset, 0);
  assert.equal(sample?.triggered, false);
  assert.equal(sample?.sampleSequence, 9);
  assert.equal(sample?.deviceTimeUs, 12345);
  assert.deepEqual(sample?.readings, CURRENT_CHANNELS.map((channel, index) => ({
    ...channel,
    value: VALUES[index],
    power_enabled: true,
    voltage_valid: true,
    voltage_uv: 5_000_000,
    current_ua: VALUES[index],
    raw: null,
    mv: 0,
    sensor_channel: "current",
  })));
});

test("decodeTelemetrySamples decodes every batch sample in order", () => {
  const samples = decodeTelemetrySamples({
    type: "telemetry-batch",
    channels: CHANNELS,
    samples: [
      { sample_sequence: 7, device_t_mono_us: 7000, values: VALUES, power_enabled_mask: 0b1111 },
      { sample_sequence: 8, device_t_mono_us: 8000, values: VALUES, power_enabled_mask: 0b0000 },
      { sample_sequence: 9, device_t_mono_us: 9000, values: VALUES, power_enabled_mask: 0b1111 },
    ],
  });

  assert.equal(samples.length, 3);
  assert.deepEqual(
    samples.map((sample) => [sample.sampleSequence, sample.deviceTimeUs]),
    [[7, 7000], [8, 8000], [9, 9000]],
  );
  assert.equal(samples[0]?.readings[0]?.power_enabled, true);
  assert.equal(samples[1]?.readings[0]?.power_enabled, false);
  assert.equal(samples[1]?.readings.length, 4);
  assert.deepEqual(samples[1].readings.map(({ name }) => name), CURRENT_CHANNELS.map(({ name }) => name));
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
