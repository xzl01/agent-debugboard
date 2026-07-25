import assert from "node:assert/strict";
import test from "node:test";

import {
  AVAILABLE_PINS,
  DEFAULT_CONFIG,
  LOGIC_ANALYZER_MAX_SAMPLE_RATE_HZ,
  LOGIC_ANALYZER_MAX_SAMPLES,
  LOGIC_ANALYZER_MIN_SAMPLE_RATE_HZ,
  LOGIC_ANALYZER_SAMPLE_RATES_HZ,
  LOGIC_ANALYZER_SIGROK_DISABLED_PINS,
  LOGIC_ANALYZER_WEB_STREAM_MAX_SAMPLE_RATE_HZ,
  SAMPLE_RATES,
  assembleBoundedSigrokCapture,
  buildSigrokCaptureRequest,
  calculateMaxSamples,
  extractLogicAnalyzerErrorMessage,
  formatSamplePeriod,
  getLogicAnalyzerActualSampleRate,
  getLogicAnalyzerBackend,
  getLogicAnalyzerRequestedSampleRate,
  getLogicAnalyzerSamplePeriodPs,
  isWebSigrokPinSupported,
  mapSigrokLogicalChannel,
  normalizeLogicAnalyzerCapture,
  normalizeLogicAnalyzerConfig,
  normalizeLogicAnalyzerSampleRate,
} from "./logicAnalyzer.ts";
import {
  buildSigrokFrame,
  parseSigrokFrame,
  parseSigrokHeader,
  SigrokFrameType,
  SigrokModeId,
  SigrokTriggerType,
} from "./sigrokClient.ts";

function buildDataFramePayload({
  sampleIndex,
  sampleCount,
  compression,
  channelMask,
  samples,
}: {
  sampleIndex: number;
  sampleCount: number;
  compression: number;
  channelMask: number;
  samples: readonly number[] | Uint8Array;
}): Uint8Array {
  const sampleBytes = samples instanceof Uint8Array ? samples : Uint8Array.from(samples);
  return new Uint8Array([
    sampleIndex & 0xff,
    (sampleIndex >> 8) & 0xff,
    (sampleIndex >> 16) & 0xff,
    sampleCount & 0xff,
    (sampleCount >> 8) & 0xff,
    compression,
    channelMask & 0xff,
    (channelMask >> 8) & 0xff,
    ...sampleBytes,
  ]);
}

function parseDataFrame(data: Uint8Array) {
  const header = parseSigrokHeader(data);
  assert.ok(header);
  const frame = parseSigrokFrame(header, data);
  assert.ok(frame);
  assert.equal(frame.type, SigrokFrameType.DATA);
  return frame;
}

test("uses the WS sigrok uint16 sample cap", () => {
  assert.equal(calculateMaxSamples(), 65535);
  assert.equal(LOGIC_ANALYZER_MAX_SAMPLES, 65535);
  assert.equal(DEFAULT_CONFIG.preSamples, 0);
  assert.equal(DEFAULT_CONFIG.postSamples, 65535);
});

test("normalizes sample rates into the firmware-supported MHz set", () => {
  assert.equal(normalizeLogicAnalyzerSampleRate(-25), LOGIC_ANALYZER_MIN_SAMPLE_RATE_HZ);
  assert.equal(normalizeLogicAnalyzerSampleRate(0), LOGIC_ANALYZER_MIN_SAMPLE_RATE_HZ);
  assert.equal(normalizeLogicAnalyzerSampleRate(1234.9), LOGIC_ANALYZER_MIN_SAMPLE_RATE_HZ);
  assert.equal(normalizeLogicAnalyzerSampleRate(100000), LOGIC_ANALYZER_MIN_SAMPLE_RATE_HZ);
  assert.equal(normalizeLogicAnalyzerSampleRate(6000000), 5000000);
  assert.equal(normalizeLogicAnalyzerSampleRate(75000000), 50000000);
  assert.equal(normalizeLogicAnalyzerSampleRate(126000000), LOGIC_ANALYZER_MAX_SAMPLE_RATE_HZ);
  assert.equal(LOGIC_ANALYZER_MIN_SAMPLE_RATE_HZ, 100000);
  assert.deepEqual(LOGIC_ANALYZER_SAMPLE_RATES_HZ, [
    100000,
    500000,
    1000000,
    2000000,
    5000000,
    10000000,
    25000000,
    50000000,
    100000000,
    125000000,
  ]);
});

test("publishes bounded sample-rate options including sub-1MHz presets", () => {
  assert.deepEqual(
    SAMPLE_RATES.map((rate) => rate.value),
    [100000, 500000, 1000000, 2000000, 5000000, 10000000, 25000000, 50000000, 100000000, 125000000]
  );
  assert.deepEqual(
    SAMPLE_RATES.slice(0, 4).map((rate) => rate.label),
    ["100 kHz", "500 kHz", "1 MHz", "2 MHz"]
  );
});

test("forces pre-trigger to zero and clamps post samples to uint16", () => {
  const config = normalizeLogicAnalyzerConfig({
    ...DEFAULT_CONFIG,
    sampleRateHz: 200000000,
    preSamples: 300,
    postSamples: 70000,
  });

  assert.equal(config.sampleRateHz, LOGIC_ANALYZER_MAX_SAMPLE_RATE_HZ);
  assert.equal(config.preSamples, 0);
  assert.equal(config.postSamples, LOGIC_ANALYZER_MAX_SAMPLES);
});

test("filters unsupported Web sigrok pins and disables pre-trigger even for edge triggers", () => {
  const config = normalizeLogicAnalyzerConfig({
    ...DEFAULT_CONFIG,
    selectedPins: [29, 4, 18, 18, 7, 10, 100],
    preSamples: 120,
    postSamples: 70000,
    triggerType: "rising",
    triggerPin: 9,
  });

  assert.deepEqual(config.selectedPins, [10, 18, 29]);
  assert.equal(config.preSamples, 0);
  assert.equal(config.postSamples, LOGIC_ANALYZER_MAX_SAMPLES);
  assert.equal(config.triggerPin, 2);
});

test("builds a FAST8 bounded sigrok request with logical trigger-channel mapping", () => {
  const request = buildSigrokCaptureRequest({
    ...DEFAULT_CONFIG,
    selectedPins: [10, 11, 17],
    sampleRateHz: 50000000,
    triggerType: "falling",
    triggerPin: 2,
    preSamples: 999,
    postSamples: 65535,
  });

  assert.equal(request.modeId, SigrokModeId.FAST8);
  assert.equal(request.triggerType, SigrokTriggerType.FALLING);
  assert.equal(request.triggerChannel, 7);
  assert.equal(request.channelMask, 0x0083);
  assert.equal(request.preSamples, 0);
  assert.equal(request.postSamples, 65535);
  assert.equal(request.samplerateKhz, 50000);
});

test("builds a WIDE12 bounded sigrok request with GP29 on logical bit 11", () => {
  const request = buildSigrokCaptureRequest({
    ...DEFAULT_CONFIG,
    selectedPins: [10, 18, 20, 29],
    triggerType: "either",
    triggerPin: 3,
    postSamples: 4096,
  });

  assert.equal(request.modeId, SigrokModeId.WIDE12);
  assert.equal(request.triggerType, SigrokTriggerType.EITHER);
  assert.equal(request.triggerChannel, 11);
  assert.equal(request.channelMask, 0x0d01);
  assert.equal(request.preSamples, 0);
  assert.equal(request.postSamples, 4096);
});

test("builds streaming requests with post=0 while keeping trigger mode", () => {
  const request = buildSigrokCaptureRequest(
    {
      ...DEFAULT_CONFIG,
      selectedPins: [10, 18, 29],
      triggerType: "rising",
      triggerPin: 1,
      postSamples: 2048,
    },
    { stream: true }
  );

  assert.equal(request.modeId, SigrokModeId.WIDE12);
  assert.equal(request.triggerType, SigrokTriggerType.RISING);
  assert.equal(request.triggerChannel, 8);
  assert.equal(request.postSamples, 0);
});

test("filters unsupported Web sigrok pins without reintroducing a default selection", () => {
  const config = normalizeLogicAnalyzerConfig({
    ...DEFAULT_CONFIG,
    selectedPins: [4, 7, 8, 9, 21, 28],
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

test("publishes the Web sigrok pin allowlist and disabled pins", () => {
  assert.deepEqual(
    AVAILABLE_PINS.map((pin) => pin.pin),
    [10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 29]
  );
  assert.deepEqual(LOGIC_ANALYZER_SIGROK_DISABLED_PINS, [7, 8, 9]);
  assert.equal(isWebSigrokPinSupported(10), true);
  assert.equal(isWebSigrokPinSupported(29), true);
  assert.equal(isWebSigrokPinSupported(7), false);
  assert.equal(isWebSigrokPinSupported(9), false);
  assert.equal(LOGIC_ANALYZER_WEB_STREAM_MAX_SAMPLE_RATE_HZ, 25000000);
});

test("maps logical channels for FAST8 and WIDE12 safely", () => {
  assert.equal(mapSigrokLogicalChannel(10, SigrokModeId.FAST8), 0);
  assert.equal(mapSigrokLogicalChannel(17, SigrokModeId.FAST8), 7);
  assert.equal(mapSigrokLogicalChannel(18, SigrokModeId.FAST8), null);
  assert.equal(mapSigrokLogicalChannel(18, SigrokModeId.WIDE12), 8);
  assert.equal(mapSigrokLogicalChannel(20, SigrokModeId.WIDE12), 10);
  assert.equal(mapSigrokLogicalChannel(29, SigrokModeId.WIDE12), 11);
  assert.equal(mapSigrokLogicalChannel(7, SigrokModeId.WIDE12), null);
});

test("prefers backend requested and actual sample-rate metadata when present", () => {
  const captureConfig = {
    pinCount: 2,
    pinBase: 10,
    sampleRateHz: 1000000,
    backend: "rp2350-sigrok-live",
    requestedSampleRateHz: 125000000,
    actualSampleRateHz: 124800000,
    samplePeriodPs: 8012.820512820513,
  };

  assert.equal(getLogicAnalyzerBackend(captureConfig), "rp2350-sigrok-live");
  assert.equal(getLogicAnalyzerRequestedSampleRate(captureConfig), 125000000);
  assert.equal(getLogicAnalyzerActualSampleRate(captureConfig), 124800000);
  assert.equal(getLogicAnalyzerSamplePeriodPs(captureConfig), 8012.820512820513);
  assert.equal(formatSamplePeriod(getLogicAnalyzerSamplePeriodPs(captureConfig)), "8.013 ns");
});

test("derives actual sample rate from sample period for high-speed captures", () => {
  const captureConfig = {
    pinCount: 1,
    pinBase: 10,
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
    pinBase: 10,
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
  assert.equal(capture.sampleCount, 520);
  assert.equal(capture.triggerIndex, 519);
  assert.equal(capture.samples.length, 520);
});

test("reads API errors from either the root message or error.message", () => {
  assert.equal(extractLogicAnalyzerErrorMessage({ message: "root" }), "root");
  assert.equal(extractLogicAnalyzerErrorMessage({ error: { message: "nested" } }), "nested");
  assert.equal(extractLogicAnalyzerErrorMessage({ error: { code: 409 } }), null);
});

test("assembles bounded multi-frame captures and computes trigger index from sample indices", () => {
  const capture = assembleBoundedSigrokCapture({
    channelMask: 0x0003,
    triggerSampleIndex: 102,
    frames: [
      {
        meta: { sampleIndex: 100, sampleCount: 2, channelMask: 0x0003 },
        samples: new Uint8Array([0x01, 0x02]),
      },
      {
        meta: { sampleIndex: 102, sampleCount: 3, channelMask: 0x0003 },
        samples: new Uint8Array([0x03, 0x00, 0x01]),
      },
    ],
  });

  assert.equal(capture.firstSampleIndex, 100);
  assert.equal(capture.sampleCount, 5);
  assert.equal(capture.triggerIndex, 2);
  assert.deepEqual(capture.values, [0x01, 0x02, 0x03, 0x00, 0x01]);
});

test("assembles bounded captures across modulo-24 sample-index wrap", () => {
  const capture = assembleBoundedSigrokCapture({
    channelMask: 0x0001,
    triggerSampleIndex: 0,
    frames: [
      {
        meta: { sampleIndex: 0xfffffe, sampleCount: 2, channelMask: 0x0001 },
        samples: new Uint8Array([0x00, 0x01]),
      },
      {
        meta: { sampleIndex: 0x000000, sampleCount: 2, channelMask: 0x0001 },
        samples: new Uint8Array([0x01, 0x00]),
      },
    ],
  });

  assert.equal(capture.firstSampleIndex, 0xfffffe);
  assert.equal(capture.sampleCount, 4);
  assert.equal(capture.triggerIndex, 2);
  assert.deepEqual(capture.values, [0x00, 0x01, 0x01, 0x00]);
});

test("rejects bounded captures with DATA gaps", () => {
  assert.throws(
    () =>
      assembleBoundedSigrokCapture({
        channelMask: 0x0001,
        triggerSampleIndex: null,
        frames: [
          {
            meta: { sampleIndex: 200, sampleCount: 2, channelMask: 0x0001 },
            samples: new Uint8Array([0x00, 0x01]),
          },
          {
            meta: { sampleIndex: 203, sampleCount: 1, channelMask: 0x0001 },
            samples: new Uint8Array([0x01]),
          },
        ],
      }),
    /Gap detected between DATA frames/
  );
});

test("rejects bounded captures with out-of-order DATA frames", () => {
  assert.throws(
    () =>
      assembleBoundedSigrokCapture({
        channelMask: 0x0001,
        triggerSampleIndex: null,
        frames: [
          {
            meta: { sampleIndex: 200, sampleCount: 2, channelMask: 0x0001 },
            samples: new Uint8Array([0x00, 0x01]),
          },
          {
            meta: { sampleIndex: 199, sampleCount: 1, channelMask: 0x0001 },
            samples: new Uint8Array([0x01]),
          },
        ],
      }),
    /Out-of-order DATA frame detected/
  );
});

test("rejects bounded captures when the trigger sample lies outside assembled data", () => {
  assert.throws(
    () =>
      assembleBoundedSigrokCapture({
        channelMask: 0x0001,
        triggerSampleIndex: 99,
        frames: [
          {
            meta: { sampleIndex: 100, sampleCount: 2, channelMask: 0x0001 },
            samples: new Uint8Array([0x00, 0x01]),
          },
        ],
      }),
    /Trigger sample index lies outside captured data window/
  );
});

test("assembles bounded captures across mixed BIT_PACK and BIT_PACK_RLE frames", () => {
  const firstFrame = parseDataFrame(
    buildSigrokFrame(
      31,
      SigrokFrameType.DATA,
      buildDataFramePayload({
        sampleIndex: 100,
        sampleCount: 3,
        compression: 1,
        channelMask: 0x0007,
        samples: [0x01, 0x05, 0x00],
      })
    )
  );
  const secondFrame = parseDataFrame(
    buildSigrokFrame(
      32,
      SigrokFrameType.DATA,
      buildDataFramePayload({
        sampleIndex: 103,
        sampleCount: 4,
        compression: 3,
        channelMask: 0x0007,
        samples: [0x07, 0x02, 0x00, 0x03, 0x02, 0x00],
      })
    )
  );

  const capture = assembleBoundedSigrokCapture({
    channelMask: 0x0007,
    triggerSampleIndex: 104,
    frames: [
      { meta: firstFrame.meta, samples: firstFrame.samples },
      { meta: secondFrame.meta, samples: secondFrame.samples },
    ],
  });

  assert.equal(capture.firstSampleIndex, 100);
  assert.equal(capture.sampleCount, 7);
  assert.equal(capture.triggerIndex, 4);
  assert.deepEqual(capture.values, [0x01, 0x05, 0x00, 0x07, 0x07, 0x03, 0x03]);
});
