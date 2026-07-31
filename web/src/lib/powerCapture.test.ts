import assert from "node:assert/strict";
import test from "node:test";
import {
  appendPowerCapturePreview,
  appendPowerCaptureSummary,
  calculateCaptureWindow,
  calculateManualCaptureWindow,
  calculateTimedCaptureWindow,
  createPowerCaptureAccumulator,
  estimateFiveVoltBattery,
  finalizePowerCaptureSummaries,
  MAX_POWER_CAPTURE_PRE_TRIGGER_SAMPLES,
  powerCapturePreTriggerLimitSeconds,
  powerCapturePreTriggerSamples,
  summarizePowerCapture,
} from "./powerCapture.ts";
import type { PowerCapture } from "./types";

function capture(currentsUa: number[]): PowerCapture {
  return {
    id: 1,
    trigger: "manual",
    source: "5v_out",
    edge: "either",
    thresholdUa: 0,
    rateHz: 1,
    preSamples: 0,
    postSamples: Math.max(1, currentsUa.length - 1),
    triggerOffset: 0,
    capturedAt: 0,
    samples: currentsUa.map((currentUa, index) => ({
      offset: index,
      triggered: index === 0,
      sampleSequence: index,
      deviceTimeUs: index * 1_000_000,
      readings: [{
        name: "5v_out",
        signal: "",
        power_enabled: true,
        raw: null,
        mv: 0,
        sensor_channel: "",
        unit: "uA",
        current_ua: currentUa,
      }],
    })),
  };
}

test("explains a capture window in samples and elapsed time", () => {
  assert.deepEqual(calculateCaptureWindow(100, 100, 300, 2048), {
    rateHz: 100,
    intervalMs: 10,
    preSamples: 100,
    postSamples: 300,
    preDurationMs: 1000,
    postDurationMs: 3000,
    totalDurationMs: 4000,
    totalSamples: 401,
    overCapacity: false,
  });
  assert.equal(calculateCaptureWindow(1000, 1024, 1024, 2048).overCapacity, true);
});

test("converts timed and manual stop settings into the firmware sample window", () => {
  const timed = calculateTimedCaptureWindow(50, 1, 30, 2048);
  assert.equal(timed.preSamples, 50);
  assert.equal(timed.postSamples, 1500);
  assert.equal(timed.totalSamples, 1551);
  assert.equal(timed.overCapacity, false);

  const manual = calculateManualCaptureWindow(100, 1, 2048);
  assert.equal(manual.preSamples, 100);
  assert.equal(manual.postSamples, 1947);
  assert.equal(manual.totalSamples, 2048);
  assert.equal(manual.postDurationMs, 19_470);
  assert.equal(manual.overCapacity, false);
});

test("integrates charge and estimated energy from device timestamps", () => {
  const summary = summarizePowerCapture(capture([1_000_000, 1_000_000, 1_000_000]), "5v_out");

  assert.equal(summary.durationMs, 2000);
  assert.equal(summary.averageCurrentA, 1);
  assert.equal(summary.peakCurrentA, 1);
  assert.ok(Math.abs(summary.milliampHours - 0.5555555556) < 1e-9);
  assert.ok(Math.abs(summary.wattHours - 0.0027777778) < 1e-9);
  assert.equal(summary.averagePowerW, 5);
});

test("converts a full capture into 5 V capacity and runtime estimates", () => {
  const summary = summarizePowerCapture(capture([1_000_000, 1_000_000, 1_000_000]), "5v_out");
  const estimate = estimateFiveVoltBattery(summary, 10_000, 90, 8);

  assert.ok(Math.abs(estimate.equivalentCapacityMah - 0.6172839506) < 1e-9);
  assert.equal(estimate.availableEnergyWh, 45);
  assert.equal(estimate.runtimeHours, 9);
  assert.ok(Math.abs((estimate.requiredCapacityMah ?? 0) - 8888.8888889) < 1e-7);
  assert.ok(Math.abs((estimate.repeatCount ?? 0) - 16200) < 1e-6);
});

test("does not claim infinite runtime when the capture has no load", () => {
  const summary = summarizePowerCapture(capture([0, 0]), "5v_out");
  const estimate = estimateFiveVoltBattery(summary, 10_000, 90, 8);

  assert.equal(estimate.runtimeHours, null);
  assert.equal(estimate.requiredCapacityMah, null);
  assert.equal(estimate.repeatCount, null);
});

test("builds exact long-running summaries without retaining every preview sample", () => {
  const source = capture(Array.from({ length: 10_001 }, () => 500_000)).samples;
  const accumulator = createPowerCaptureAccumulator();
  const preview: typeof source = [];
  let stride = 1;

  for (let index = 0; index < source.length; index += 137) {
    const chunk = source.slice(index, index + 137);
    appendPowerCaptureSummary(accumulator, chunk);
    stride = appendPowerCapturePreview(preview, chunk, stride, 256);
  }

  const summary = finalizePowerCaptureSummaries(accumulator)["5v_out"];
  assert.equal(preview.length <= 256, true);
  assert.equal(preview[0]?.offset, 0);
  assert.equal(summary.durationMs, 10_000_000);
  assert.ok(Math.abs(summary.averageCurrentA - 0.5) < 1e-12);
  assert.equal(summary.peakCurrentA, 0.5);
  assert.ok(Math.abs(summary.milliampHours - 1388.8888889) < 1e-6);
});

test("bounds rich pre-trigger history independently from long recordings", () => {
  assert.equal(powerCapturePreTriggerLimitSeconds(1000), 60);
  assert.equal(
    powerCapturePreTriggerSamples(1000, 4 * 60 * 60),
    MAX_POWER_CAPTURE_PRE_TRIGGER_SAMPLES,
  );
  assert.equal(powerCapturePreTriggerSamples(100, 5), 500);
});
