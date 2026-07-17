import assert from "node:assert/strict";
import test from "node:test";
import {
  buildLogicAnalyzerArmRequest,
  DEFAULT_CONFIG,
  LOGIC_ANALYZER_MAX_SAMPLE_RATE_HZ,
  LOGIC_ANALYZER_SAMPLE_RATES_HZ,
  LOGIC_ANALYZER_MAX_SAMPLES,
  LOGIC_ANALYZER_MIN_SAMPLE_RATE_HZ,
  calculateMaxSamples,
  extractLogicAnalyzerErrorMessage,
  formatSamplePeriod,
  getLogicAnalyzerActualSampleRate,
  getLogicAnalyzerBackend,
  getLogicAnalyzerRequestedSampleRate,
  getLogicAnalyzerSamplePeriodPs,
  normalizeLogicAnalyzerCapture,
  normalizeLogicAnalyzerSampleRate,
  normalizeLogicAnalyzerConfig,
} from "./logicAnalyzer.ts";

test("uses the firmware-aligned 512 sample cap", () => {
  assert.equal(calculateMaxSamples(), 512);
  assert.equal(LOGIC_ANALYZER_MAX_SAMPLES, 512);
  assert.equal(DEFAULT_CONFIG.postSamples, 512);
});

test("normalizes sample rates into the firmware-supported MHz set", () => {
  assert.equal(normalizeLogicAnalyzerSampleRate(-25), LOGIC_ANALYZER_MIN_SAMPLE_RATE_HZ);
  assert.equal(normalizeLogicAnalyzerSampleRate(0), LOGIC_ANALYZER_MIN_SAMPLE_RATE_HZ);
  assert.equal(normalizeLogicAnalyzerSampleRate(1234.9), LOGIC_ANALYZER_MIN_SAMPLE_RATE_HZ);
  assert.equal(normalizeLogicAnalyzerSampleRate(6000000), 5000000);
  assert.equal(normalizeLogicAnalyzerSampleRate(75000000), 50000000);
  assert.equal(normalizeLogicAnalyzerSampleRate(126000000), LOGIC_ANALYZER_MAX_SAMPLE_RATE_HZ);
  assert.deepEqual(LOGIC_ANALYZER_SAMPLE_RATES_HZ, [
    1000000,
    5000000,
    10000000,
    25000000,
    50000000,
    100000000,
    125000000,
  ]);
});

test("forces free-run captures to use zero pre-trigger samples and clamps post samples to 512", () => {
  const config = normalizeLogicAnalyzerConfig({
    ...DEFAULT_CONFIG,
    sampleRateHz: 200000000,
    preSamples: 300,
    postSamples: 900,
  });

  assert.equal(config.sampleRateHz, LOGIC_ANALYZER_MAX_SAMPLE_RATE_HZ);
  assert.equal(config.preSamples, 0);
  assert.equal(config.postSamples, LOGIC_ANALYZER_MAX_SAMPLES);
});

test("preserves pre-trigger samples for edge triggers", () => {
  const config = normalizeLogicAnalyzerConfig({
    ...DEFAULT_CONFIG,
    selectedPins: [29, 4, 18, 18, 7, 100],
    preSamples: 120,
    postSamples: 600,
    triggerType: "rising",
    triggerPin: 9,
  });

  assert.deepEqual(config.selectedPins, [7, 18, 29]);
  assert.equal(config.preSamples, 120);
  assert.equal(config.postSamples, LOGIC_ANALYZER_MAX_SAMPLES);
  assert.equal(config.triggerPin, 2);
});

test("forces free-run captures to use zero pre-trigger samples", () => {
  const config = normalizeLogicAnalyzerConfig({
    ...DEFAULT_CONFIG,
    selectedPins: [29, 4, 18, 18, 7, 100],
    preSamples: 120,
    postSamples: 600,
    triggerType: "none",
    triggerPin: 9,
  });

  assert.deepEqual(config.selectedPins, [7, 18, 29]);
  assert.equal(config.preSamples, 0);
  assert.equal(config.postSamples, LOGIC_ANALYZER_MAX_SAMPLES);
  assert.equal(config.triggerPin, 2);
});

test("filters unsupported pins without reintroducing a default selection", () => {
  const config = normalizeLogicAnalyzerConfig({
    ...DEFAULT_CONFIG,
    selectedPins: [4, 21, 28],
  });

  assert.deepEqual(config.selectedPins, []);
  assert.equal(config.triggerPin, 0);
});

test("normalizes stale configs with out-of-range sample rates before use", () => {
  const config = normalizeLogicAnalyzerConfig({
    ...DEFAULT_CONFIG,
    sampleRateHz: Number.NaN,
  });

  assert.equal(config.sampleRateHz, LOGIC_ANALYZER_MIN_SAMPLE_RATE_HZ);
});

test("builds sparse selected-pin arm requests with selected pin count", () => {
  const request = buildLogicAnalyzerArmRequest({
    ...DEFAULT_CONFIG,
    selectedPins: [13, 29],
  });

  assert.deepEqual(request, {
    selected_pins: [13, 29],
    pin_base: 13,
    pin_count: 2,
    sample_rate_hz: 1000000,
    pre_samples: 0,
    post_samples: 512,
    trigger: "none",
    trigger_pin: 0,
  });
});

test("dedupes, sorts, and filters unsafe pins when building arm requests", () => {
  const request = buildLogicAnalyzerArmRequest({
    ...DEFAULT_CONFIG,
    selectedPins: [29, 13, 13, 4],
    sampleRateHz: 6000000,
    postSamples: 999,
  });

  assert.deepEqual(request, {
    selected_pins: [13, 29],
    pin_base: 13,
    pin_count: 2,
    sample_rate_hz: 5000000,
    pre_samples: 0,
    post_samples: 512,
    trigger: "none",
    trigger_pin: 0,
  });
});

test("clamps out-of-range trigger index to the normalized selected-pin index", () => {
  const request = buildLogicAnalyzerArmRequest({
    ...DEFAULT_CONFIG,
    selectedPins: [29, 13, 13, 4],
    triggerType: "rising",
    triggerPin: 99,
  });

  assert.equal(request.trigger, "rising");
  assert.equal(request.trigger_pin, 1);
});

test("builds single-pin arm requests without expanding pin count", () => {
  const request = buildLogicAnalyzerArmRequest({
    ...DEFAULT_CONFIG,
    selectedPins: [29],
    triggerType: "either",
    triggerPin: 8,
  });

  assert.deepEqual(request, {
    selected_pins: [29],
    pin_base: 29,
    pin_count: 1,
    sample_rate_hz: 1000000,
    pre_samples: 0,
    post_samples: 512,
    trigger: "either",
    trigger_pin: 0,
  });
});

test("prefers backend requested and actual sample-rate metadata when present", () => {
  const captureConfig = {
    pinCount: 2,
    pinBase: 7,
    sampleRateHz: 1000000,
    backend: "rp2350-pio-dma-single-shot",
    requestedSampleRateHz: 125000000,
    actualSampleRateHz: 124800000,
    samplePeriodPs: 8012.820512820513,
  };

  assert.equal(getLogicAnalyzerBackend(captureConfig), "rp2350-pio-dma-single-shot");
  assert.equal(getLogicAnalyzerRequestedSampleRate(captureConfig), 125000000);
  assert.equal(getLogicAnalyzerActualSampleRate(captureConfig), 124800000);
  assert.equal(getLogicAnalyzerSamplePeriodPs(captureConfig), 8012.820512820513);
  assert.equal(formatSamplePeriod(getLogicAnalyzerSamplePeriodPs(captureConfig)), "8.013 ns");
});

test("derives actual sample rate from sample period for high-speed captures", () => {
  const captureConfig = {
    pinCount: 1,
    pinBase: 7,
    requestedSampleRateHz: 100000000,
    samplePeriodPs: 8000,
  };

  assert.equal(getLogicAnalyzerRequestedSampleRate(captureConfig), 100000000);
  assert.equal(getLogicAnalyzerActualSampleRate(captureConfig), 125000000);
  assert.equal(getLogicAnalyzerSamplePeriodPs(captureConfig), 8000);
  assert.equal(formatSamplePeriod(getLogicAnalyzerSamplePeriodPs(captureConfig)), "8.000 ns");
});

test("falls back to legacy sampleRateHz metadata when new fields are absent", () => {
  const captureConfig = {
    pinCount: 1,
    pinBase: 7,
    sampleRateHz: 5000000,
  };

  assert.equal(getLogicAnalyzerRequestedSampleRate(captureConfig), 5000000);
  assert.equal(getLogicAnalyzerActualSampleRate(captureConfig), 5000000);
  assert.equal(getLogicAnalyzerSamplePeriodPs(captureConfig), 200000);
  assert.equal(formatSamplePeriod(getLogicAnalyzerSamplePeriodPs(captureConfig)), "200.000 ns");
});

test("normalizes capture payload bounds and unsafe pin metadata", () => {
  const capture = normalizeLogicAnalyzerCapture({
    state: "done",
    config: {
      pinCount: 99,
      pinBase: 4,
      selectedPins: [29, 4, 8, 8, 18],
      sampleRateHz: 1000000,
      triggerPin: 77,
    },
    sampleCount: 900,
    triggerIndex: 999,
    samples: Array.from({ length: 520 }, (_, index) => ({
      timestampUs: index,
      values: index,
    })),
  });

  assert.deepEqual(capture.config.selectedPins, [8, 18, 29]);
  assert.equal(capture.config.pinCount, 3);
  assert.equal(capture.config.pinBase, 8);
  assert.equal(capture.config.triggerPin, 2);
  assert.equal(capture.sampleCount, 512);
  assert.equal(capture.triggerIndex, 511);
  assert.equal(capture.samples.length, 512);
});

test("reads API errors from either the root message or error.message", () => {
  assert.equal(extractLogicAnalyzerErrorMessage({ message: "root" }), "root");
  assert.equal(
    extractLogicAnalyzerErrorMessage({ error: { message: "nested" } }),
    "nested"
  );
  assert.equal(extractLogicAnalyzerErrorMessage({ error: { code: 409 } }), null);
});
