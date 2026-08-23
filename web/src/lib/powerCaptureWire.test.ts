import assert from "node:assert/strict";
import test from "node:test";
import { POWER_CAPTURE_PROTOCOL } from "./powerCapture.ts";
import {
  appendLegacyCaptureSamples,
  completeLegacyCapture,
  createLegacyCaptureBuilder,
  powerCaptureArmMessage,
} from "./powerCaptureWire.ts";
import type { CaptureConfig } from "./types.ts";

function config(overrides: Partial<CaptureConfig> = {}): CaptureConfig {
  return {
    trigger: "manual",
    source: "5v_out",
    edge: "rising",
    thresholdUa: 2500,
    rateHz: 100,
    preSamples: 10,
    postSamples: 20,
    ...overrides,
  };
}

test("powerCaptureArmMessage builds the exact capture_arm payload for a rail trigger", () => {
  // Given a non-gpio trigger configuration
  // When the arm wire message is built
  const message = powerCaptureArmMessage(config());

  // Then every protocol field keeps its exact name and value
  assert.deepEqual(message, {
    type: "command",
    command: "capture_arm",
    id: "web-capture",
    mode: POWER_CAPTURE_PROTOCOL,
    trigger: "manual",
    output: "5v_out",
    gpio: "",
    edge: "rising",
    threshold_ua: 2500,
    rate_hz: 100,
    pre_samples: 0,
    post_samples: 1,
  });
});

test("powerCaptureArmMessage routes the source to gpio for a gpio trigger", () => {
  // Given a gpio trigger configuration
  // When the arm wire message is built
  const message = powerCaptureArmMessage(config({ trigger: "gpio", source: "GP7" }));

  // Then the source is carried by gpio and output stays empty
  assert.equal(message.output, "");
  assert.equal(message.gpio, "GP7");
});

test("createLegacyCaptureBuilder maps every capture_begin field", () => {
  // Given a complete capture_begin frame
  // When the legacy builder is created
  const builder = createLegacyCaptureBuilder({
    capture_id: 7,
    trigger: "current",
    source: "12v_out",
    edge: "falling",
    threshold_ua: 900,
    rate_hz: 1000,
    pre_samples: 5,
    post_samples: 6,
    trigger_offset: 5,
    sample_count: 11,
  });

  // Then all numeric and string fields are converted
  assert.deepEqual(builder, {
    id: 7,
    trigger: "current",
    source: "12v_out",
    edge: "falling",
    thresholdUa: 900,
    rateHz: 1000,
    preSamples: 5,
    postSamples: 6,
    triggerOffset: 5,
    expected: 11,
    samples: [],
  });
});

test("createLegacyCaptureBuilder falls back to zero and empty string for missing fields", () => {
  // Given an empty capture_begin frame
  // When the legacy builder is created
  const builder = createLegacyCaptureBuilder({});

  // Then every field keeps its documented fallback
  assert.deepEqual(builder, {
    id: 0,
    trigger: "",
    source: "",
    edge: "",
    thresholdUa: 0,
    rateHz: 0,
    preSamples: 0,
    postSamples: 0,
    triggerOffset: 0,
    expected: 0,
    samples: [],
  });
});

test("appendLegacyCaptureSamples applies offset and sequence fallbacks and skips non-record frames", () => {
  // Given a builder with one existing sample and mixed incoming frames
  const builder = createLegacyCaptureBuilder({});
  appendLegacyCaptureSamples(builder, [{
    offset: 0,
    triggered: true,
    sample_sequence: 40,
    device_t_mono_us: 1_000_000,
    readings: [{ name: "5v_out", power_enabled: true, current_ua: 1234 }],
  }]);

  // When frames with missing fields and non-record entries are appended
  appendLegacyCaptureSamples(builder, [
    null,
    "not-a-record",
    { readings: [{ name: "12v_out", power_enabled: false, current_ua: -5 }, { name: "bad" }] },
  ]);

  // Then the offset defaults to the current length, flags require === true, and
  // sequence/time fall back to zero while only valid current readings survive
  assert.equal(builder.samples.length, 2);
  const appended = builder.samples[1];
  assert.equal(appended.offset, 1);
  assert.equal(appended.triggered, false);
  assert.equal(appended.sampleSequence, 0);
  assert.equal(appended.deviceTimeUs, 0);
  assert.deepEqual(appended.readings, [{
    kind: "current",
    unit: "uA",
    name: "12v_out",
    signal: "",
    value: -5,
    power_enabled: false,
    current_ua: -5,
    raw: null,
    mv: 0,
    sensor_channel: "current",
  }]);
  assert.equal(builder.samples[0].triggered, true);
});

test("completeLegacyCapture assembles the capture record with the given timestamp", () => {
  // Given a builder with collected samples
  const builder = createLegacyCaptureBuilder({
    capture_id: 3,
    trigger: "manual",
    source: "20v_out",
    edge: "either",
    sample_count: 1,
  });
  appendLegacyCaptureSamples(builder, [{
    offset: 0,
    triggered: false,
    sample_sequence: 9,
    device_t_mono_us: 50,
    readings: [],
  }]);

  // When the capture completes
  const capture = completeLegacyCapture(builder, 1_700_000_000_000);

  // Then the record carries the builder fields, samples, and capture time
  assert.equal(capture.id, 3);
  assert.equal(capture.trigger, "manual");
  assert.equal(capture.source, "20v_out");
  assert.equal(capture.samples, builder.samples);
  assert.equal(capture.capturedAt, 1_700_000_000_000);
  assert.equal(capture.archiveId, undefined);
});
