import assert from "node:assert/strict";
import test from "node:test";

import {
  AVAILABLE_PINS,
  classifyLogicAnalyzerStreamStop,
  DEFAULT_CONFIG,
  LOGIC_ANALYZER_CONFIG_V2_EXACT_WIDE11_PINS,
  LOGIC_ANALYZER_HIGH_RATE_MAX_SAMPLES,
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
  calculateMaxSamplesForCapabilities,
  extractLogicAnalyzerErrorMessage,
  formatSamplePeriod,
  getLogicAnalyzerActualSampleRate,
  getLogicAnalyzerBackend,
  getLogicAnalyzerMaxSamplesForConfig,
  getLogicAnalyzerNegotiatedCapabilityReason,
  getLogicAnalyzerRequestedSampleRate,
  getLogicAnalyzerSamplePeriodPs,
  getLogicAnalyzerPreTriggerReason,
  getLogicAnalyzerSelectionMaxSamples,
  getLogicAnalyzerStreamLimitReason,
  getLogicAnalyzerUnsupportedRateReason,
  hasExactWide11PinSelection,
  isFast8PhysicalSpanSelection,
  isWebSigrokPinSupported,
  mapSigrokLogicalChannel,
  normalizeLogicAnalyzerCapture,
  normalizeLogicAnalyzerConfig,
  normalizeLogicAnalyzerLocalConfig,
  normalizeLogicAnalyzerSampleRate,
  supportsLegacyConfigV2ExactWide11PackedBurst,
  supportsHighRatePackedBurst,
} from "./logicAnalyzer.ts";
import {
  buildSigrokFrame,
  parseSigrokFrame,
  parseSigrokHeader,
  SigrokFrameType,
  SigrokModeId,
  SigrokModeFlag,
  SigrokTriggerType,
  type SigrokCapsResp,
} from "./sigrokClient.ts";

const PRE_TRIGGER_MODE_FLAGS =
  SigrokModeFlag.CONTINUOUS |
  SigrokModeFlag.TRIGGER_NONE |
  SigrokModeFlag.TRIGGER_RISING |
  SigrokModeFlag.TRIGGER_FALLING |
  SigrokModeFlag.TRIGGER_EITHER |
  SigrokModeFlag.PRE_TRIGGER;

const PRE_TRIGGER_CAPS: SigrokCapsResp = {
  modeCount: 2,
  modes: [
    {
      modeId: SigrokModeId.FAST8,
      modeFlags: PRE_TRIGGER_MODE_FLAGS,
      channelCount: 8,
      sampleBytes: 1,
      maxSamplerateKhz: 125000,
      compression: 3,
    },
    {
      modeId: SigrokModeId.WIDE11,
      modeFlags: PRE_TRIGGER_MODE_FLAGS,
      channelCount: 11,
      sampleBytes: 2,
      maxSamplerateKhz: 125000,
      compression: 3,
    },
  ],
};

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
    300000,
    500000,
    1000000,
    2000000,
    5000000,
    10000000,
    16000000,
    25000000,
    50000000,
    100000000,
    125000000,
  ]);
});

test("publishes bounded sample-rate options including sub-1MHz presets", () => {
  assert.deepEqual(
    SAMPLE_RATES.map((rate) => rate.value),
    [100000, 300000, 500000, 1000000, 2000000, 5000000, 10000000, 16000000, 25000000, 50000000, 100000000, 125000000]
  );
  assert.deepEqual(
    SAMPLE_RATES.slice(0, 5).map((rate) => rate.label),
    ["100 kHz", "300 kHz", "500 kHz", "1 MHz", "2 MHz"]
  );
});

test("publishes the proven 16 MHz protocol-v2 operating point", () => {
  assert.deepEqual(
    SAMPLE_RATES.find((rate) => rate.value === 16000000),
    { value: 16000000, label: "16 MHz" }
  );
});

test("publishes the proven 300 kHz WebSocket operating point", () => {
  assert.equal(LOGIC_ANALYZER_SAMPLE_RATES_HZ.includes(300000), true);
  assert.deepEqual(
    SAMPLE_RATES.find((rate) => rate.value === 300000),
    { value: 300000, label: "300 kHz" }
  );
});

test("local normalization disables pre-trigger for no trigger without limiting post depth", () => {
  const config = normalizeLogicAnalyzerLocalConfig({
    ...DEFAULT_CONFIG,
    sampleRateHz: 50000000,
    preSamples: 300,
    postSamples: LOGIC_ANALYZER_MAX_SAMPLES,
  });

  assert.equal(config.sampleRateHz, 50000000);
  assert.equal(config.preSamples, 0);
  assert.equal(config.postSamples, LOGIC_ANALYZER_MAX_SAMPLES);
});

test("preserves valid bounded pre-trigger locally and clamps the combined depth", () => {
  const config = normalizeLogicAnalyzerLocalConfig({
    ...DEFAULT_CONFIG,
    selectedPins: [10, 18],
    sampleRateHz: 1000000,
    preSamples: 120,
    postSamples: 393,
    triggerType: "rising",
    triggerPin: 1,
  });

  assert.deepEqual(config.selectedPins, [10, 18]);
  assert.equal(config.preSamples, 120);
  assert.equal(config.postSamples, 392);
  assert.equal(config.triggerPin, 1);
});

test("local normalization resets infeasible pre-trigger without limiting ordinary post depth", () => {
  const config = normalizeLogicAnalyzerLocalConfig({
    ...DEFAULT_CONFIG,
    selectedPins: [10, 11],
    sampleRateHz: 25000000,
    preSamples: 128,
    postSamples: LOGIC_ANALYZER_MAX_SAMPLES,
    triggerType: "rising",
  });

  assert.equal(config.preSamples, 0);
  assert.equal(config.postSamples, LOGIC_ANALYZER_MAX_SAMPLES);
});

test("publishes generic high-rate max depth helpers", () => {
  assert.equal(calculateMaxSamplesForCapabilities(), LOGIC_ANALYZER_MAX_SAMPLES);
  assert.equal(
    calculateMaxSamplesForCapabilities({ supportsConfigV2: true }),
    LOGIC_ANALYZER_HIGH_RATE_MAX_SAMPLES
  );

  assert.equal(
    getLogicAnalyzerSelectionMaxSamples({ selectedPins: [10], sampleRateHz: 125000000 }),
    LOGIC_ANALYZER_HIGH_RATE_MAX_SAMPLES
  );
  assert.equal(
    getLogicAnalyzerMaxSamplesForConfig(
      { selectedPins: [10], sampleRateHz: 125000000 },
      { supportsConfigV2: false }
    ),
    LOGIC_ANALYZER_MAX_SAMPLES
  );
  assert.equal(
    getLogicAnalyzerMaxSamplesForConfig(
      { selectedPins: [10], sampleRateHz: 125000000 },
      { supportsConfigV2: true }
    ),
    LOGIC_ANALYZER_MAX_SAMPLES
  );
  assert.equal(
    getLogicAnalyzerMaxSamplesForConfig(
      { selectedPins: [10], sampleRateHz: 125000000 },
      { supportsConfigV2: true, supportsGenericPackedBurst: true }
    ),
    LOGIC_ANALYZER_HIGH_RATE_MAX_SAMPLES
  );
  assert.equal(
    getLogicAnalyzerMaxSamplesForConfig(
      {
        selectedPins: [...LOGIC_ANALYZER_CONFIG_V2_EXACT_WIDE11_PINS],
        sampleRateHz: 100000000,
      },
      { supportsConfigV2: true }
    ),
    LOGIC_ANALYZER_HIGH_RATE_MAX_SAMPLES
  );
});

test("publishes CONFIG_V2 bounded depth at low sample rates", () => {
  const maxSamples = getLogicAnalyzerMaxSamplesForConfig(
    { selectedPins: [10], sampleRateHz: 1000000 },
    { supportsConfigV2: true, supportsGenericPackedBurst: true }
  );

  assert.equal(maxSamples, LOGIC_ANALYZER_HIGH_RATE_MAX_SAMPLES);
});

test("accepts low-rate CONFIG_V2 bounded captures on the common packed pipeline", () => {
  const config = normalizeLogicAnalyzerConfig(
    {
      ...DEFAULT_CONFIG,
      selectedPins: [10],
      sampleRateHz: 1000000,
      preSamples: 0,
      postSamples: 100000,
    },
    { supportsConfigV2: true, supportsGenericPackedBurst: true }
  );

  assert.equal(config.postSamples, 100000);
});

test("permits generic high-rate CONFIG_V2 depth cases only when bit1 generic packed burst is negotiated", () => {
  const fast8SinglePin = normalizeLogicAnalyzerConfig(
    {
      ...DEFAULT_CONFIG,
      selectedPins: [10],
      sampleRateHz: 125000000,
      preSamples: 0,
      postSamples: 100000,
    },
    { supportsConfigV2: true, supportsGenericPackedBurst: true }
  );

  assert.equal(fast8SinglePin.preSamples, 0);
  assert.equal(fast8SinglePin.postSamples, 100000);
  assert.deepEqual(fast8SinglePin.selectedPins, [10]);

  const config = normalizeLogicAnalyzerConfig(
    {
      ...DEFAULT_CONFIG,
      selectedPins: [...LOGIC_ANALYZER_CONFIG_V2_EXACT_WIDE11_PINS],
      sampleRateHz: 100000000,
      preSamples: 0,
      postSamples: 100000,
    },
    { supportsConfigV2: true }
  );

  assert.equal(config.preSamples, 0);
  assert.equal(config.postSamples, 100000);
  assert.deepEqual(config.selectedPins, [...LOGIC_ANALYZER_CONFIG_V2_EXACT_WIDE11_PINS]);
});

test("allows local deep-burst normalization without live capability discovery", () => {
  const config = normalizeLogicAnalyzerLocalConfig({
    ...DEFAULT_CONFIG,
    selectedPins: [13],
    sampleRateHz: 100000000,
    postSamples: 100000,
  });

  assert.equal(config.postSamples, 100000);
  assert.equal(config.sampleRateHz, 100000000);
  assert.deepEqual(config.selectedPins, [13]);
});

test("accepts FAST8 physical-span subsets for 100/125MHz packed burst", () => {
  assert.equal(isFast8PhysicalSpanSelection([10]), true);
  assert.equal(isFast8PhysicalSpanSelection([10, 13, 17]), true);
  assert.equal(isFast8PhysicalSpanSelection([10, 18]), false);
  assert.equal(
    supportsHighRatePackedBurst({ selectedPins: [10], sampleRateHz: 100000000 }),
    true
  );
  assert.equal(
    supportsHighRatePackedBurst({ selectedPins: [10], sampleRateHz: 125000000 }),
    true
  );
  assert.equal(
    supportsHighRatePackedBurst({ selectedPins: [10, 13, 17], sampleRateHz: 125000000 }),
    true
  );
  assert.equal(
    supportsLegacyConfigV2ExactWide11PackedBurst({
      selectedPins: [...LOGIC_ANALYZER_CONFIG_V2_EXACT_WIDE11_PINS],
      sampleRateHz: 100000000,
    }),
    true
  );
});

test("rejects WIDE11 at 125MHz with a precise message", () => {
  assert.equal(
    getLogicAnalyzerUnsupportedRateReason({ selectedPins: [10, 18, 20], sampleRateHz: 125000000 }),
    "WIDE11 at 125 MHz is not supported; use GP10-GP17 only or drop to 100 MHz"
  );
  assert.equal(
    getLogicAnalyzerStreamLimitReason({ selectedPins: [10, 18, 20], sampleRateHz: 125000000 }),
    "WIDE11 at 125 MHz is not supported; use GP10-GP17 only or drop to 100 MHz"
  );

  assert.throws(
    () =>
      normalizeLogicAnalyzerConfig(
        {
          ...DEFAULT_CONFIG,
          selectedPins: [10, 18, 20],
          sampleRateHz: 125000000,
          postSamples: 65535,
        },
        { supportsConfigV2: true }
      ),
    /WIDE11 at 125 MHz is not supported/
  );
});

test("publishes a named helper for the exact WIDE11 deep-burst pin contract", () => {
  assert.equal(hasExactWide11PinSelection(LOGIC_ANALYZER_CONFIG_V2_EXACT_WIDE11_PINS), true);
  assert.equal(hasExactWide11PinSelection([10, 18, 20]), false);
});

test("builds a FAST8 bounded sigrok request with logical trigger-channel mapping", () => {
  const request = buildSigrokCaptureRequest({
    ...DEFAULT_CONFIG,
    selectedPins: [10, 11, 17],
    sampleRateHz: 50000000,
    triggerType: "falling",
    triggerPin: 2,
    preSamples: 0,
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

test("builds a supported 1MHz bounded pre-trigger request", () => {
  const request = buildSigrokCaptureRequest(
    {
      ...DEFAULT_CONFIG,
      selectedPins: [13],
      sampleRateHz: 1000000,
      triggerType: "rising",
      triggerPin: 0,
      preSamples: 128,
      postSamples: 128,
    },
    { caps: PRE_TRIGGER_CAPS }
  );

  assert.equal(request.modeId, SigrokModeId.FAST8);
  assert.equal(request.triggerType, SigrokTriggerType.RISING);
  assert.equal(request.preSamples, 128);
  assert.equal(request.postSamples, 128);
});

test("rejects supported local pre-trigger when connected CAPS lacks the mode flag", () => {
  assert.throws(
    () =>
      normalizeLogicAnalyzerConfig(
        {
          ...DEFAULT_CONFIG,
          selectedPins: [13],
          sampleRateHz: 1000000,
          triggerType: "rising",
          preSamples: 128,
          postSamples: 128,
        },
        { caps: null }
      ),
    /PRE_TRIGGER.*FAST8/
  );
});

test("accepts the bounded pre-trigger total of 512 samples", () => {
  const request = buildSigrokCaptureRequest(
    {
      ...DEFAULT_CONFIG,
      selectedPins: [13],
      sampleRateHz: 1000000,
      triggerType: "either",
      preSamples: 128,
      postSamples: 384,
    },
    { caps: PRE_TRIGGER_CAPS }
  );

  assert.equal(request.preSamples + request.postSamples, 512);
});

test("rejects a bounded pre-trigger total above 512 samples", () => {
  assert.throws(
    () =>
      buildSigrokCaptureRequest(
        {
          ...DEFAULT_CONFIG,
          selectedPins: [13],
          sampleRateHz: 1000000,
          triggerType: "rising",
          preSamples: 128,
          postSamples: 385,
        },
        { caps: PRE_TRIGGER_CAPS }
      ),
    /512/
  );
});

test("rejects pre-trigger for trigger none and rates below 1MHz", () => {
  assert.throws(
    () =>
      buildSigrokCaptureRequest(
        {
          ...DEFAULT_CONFIG,
          selectedPins: [13],
          sampleRateHz: 1000000,
          triggerType: "none",
          preSamples: 1,
          postSamples: 1,
        },
        { caps: PRE_TRIGGER_CAPS }
      ),
    /trigger/i
  );
  assert.throws(
    () =>
      buildSigrokCaptureRequest(
        {
          ...DEFAULT_CONFIG,
          selectedPins: [13],
          sampleRateHz: 500000,
          triggerType: "rising",
          preSamples: 1,
          postSamples: 1,
        },
        { caps: PRE_TRIGGER_CAPS }
      ),
    /1 MHz/
  );
  assert.throws(
    () =>
      buildSigrokCaptureRequest(
        {
          ...DEFAULT_CONFIG,
          selectedPins: [13],
          sampleRateHz: 1000000,
          triggerType: "rising",
          preSamples: 1,
          postSamples: 0,
        },
        { caps: PRE_TRIGGER_CAPS }
      ),
    /post-trigger sample/
  );
});

test("reports physical plan limits for bounded pre-trigger", () => {
  assert.equal(
    getLogicAnalyzerPreTriggerReason(
      { selectedPins: [13], sampleRateHz: 25000000, triggerType: "rising", preSamples: 1, postSamples: 1 },
      { caps: PRE_TRIGGER_CAPS }
    ),
    null
  );
  assert.equal(
    getLogicAnalyzerPreTriggerReason(
      { selectedPins: [10, 11], sampleRateHz: 10000000, triggerType: "rising", preSamples: 1, postSamples: 1 },
      { caps: PRE_TRIGGER_CAPS }
    ),
    null
  );
  assert.match(
    getLogicAnalyzerPreTriggerReason(
      { selectedPins: [10, 11], sampleRateHz: 25000000, triggerType: "rising", preSamples: 1, postSamples: 1 },
      { caps: PRE_TRIGGER_CAPS }
    ) ?? "",
    /FAST8|15\.36|capacity/
  );
  assert.equal(
    getLogicAnalyzerPreTriggerReason(
      { selectedPins: [10, 18], sampleRateHz: 5000000, triggerType: "rising", preSamples: 1, postSamples: 1 },
      { caps: PRE_TRIGGER_CAPS }
    ),
    null
  );
  assert.match(
    getLogicAnalyzerPreTriggerReason(
      { selectedPins: [10, 18], sampleRateHz: 10000000, triggerType: "rising", preSamples: 1, postSamples: 1 },
      { caps: PRE_TRIGGER_CAPS }
    ) ?? "",
    /WIDE11|7\.168|capacity/
  );
});

test("rejects missing or mismatched PRE_TRIGGER mode flags", () => {
  const wide11MissingPreTrigger: SigrokCapsResp = {
    ...PRE_TRIGGER_CAPS,
    modes: PRE_TRIGGER_CAPS.modes.map((mode) =>
      mode.modeId === SigrokModeId.WIDE11
        ? { ...mode, modeFlags: mode.modeFlags & ~SigrokModeFlag.PRE_TRIGGER }
        : mode
    ),
  };

  assert.throws(
    () =>
      buildSigrokCaptureRequest(
        {
          ...DEFAULT_CONFIG,
          selectedPins: [10, 18],
          sampleRateHz: 5000000,
          triggerType: "rising",
          preSamples: 1,
          postSamples: 1,
        },
        { caps: wide11MissingPreTrigger }
      ),
    /PRE_TRIGGER.*WIDE11/
  );
});

test("preserves exact requested post samples including 513", () => {
  const request = buildSigrokCaptureRequest({
    ...DEFAULT_CONFIG,
    selectedPins: [13],
    sampleRateHz: 5000000,
    postSamples: 513,
  });

  assert.equal(request.postSamples, 513);
});

test("builds a WIDE11 bounded sigrok request with GP10-GP20 logical mapping", () => {
  const request = buildSigrokCaptureRequest({
    ...DEFAULT_CONFIG,
    selectedPins: [10, 18, 20],
    triggerType: "either",
    triggerPin: 2,
    postSamples: 4096,
  });

  assert.equal(request.modeId, SigrokModeId.WIDE11);
  assert.equal(request.triggerType, SigrokTriggerType.EITHER);
  assert.equal(request.triggerChannel, 10);
  assert.equal(request.channelMask, 0x0501);
  assert.equal(request.preSamples, 0);
  assert.equal(request.postSamples, 4096);
});

test("builds exact WIDE11 100MHz post=100000 request when CONFIG_V2 is negotiated", () => {
  const request = buildSigrokCaptureRequest(
    {
      ...DEFAULT_CONFIG,
      selectedPins: [...LOGIC_ANALYZER_CONFIG_V2_EXACT_WIDE11_PINS],
      sampleRateHz: 100000000,
      triggerType: "either",
      triggerPin: 10,
      postSamples: 100000,
    },
    { supportsConfigV2: true }
  );

  assert.equal(request.modeId, SigrokModeId.WIDE11);
  assert.equal(request.triggerType, SigrokTriggerType.EITHER);
  assert.equal(request.triggerChannel, 10);
  assert.equal(request.channelMask, 0x07ff);
  assert.equal(request.preSamples, 0);
  assert.equal(request.postSamples, 100000);
  assert.equal(request.samplerateKhz, 100000);
});

test("builds generic FAST8 125MHz post=100000 requests only when bit1 generic packed burst is negotiated", () => {
  const exact513 = buildSigrokCaptureRequest(
    {
      ...DEFAULT_CONFIG,
      selectedPins: [10],
      sampleRateHz: 125000000,
      postSamples: 513,
    },
    { supportsConfigV2: true, supportsGenericPackedBurst: true }
  );
  assert.equal(exact513.modeId, SigrokModeId.FAST8);
  assert.equal(exact513.postSamples, 513);
  assert.equal(exact513.samplerateKhz, 125000);

  const singlePin = buildSigrokCaptureRequest(
    {
      ...DEFAULT_CONFIG,
      selectedPins: [10],
      sampleRateHz: 125000000,
      postSamples: 100000,
    },
    { supportsConfigV2: true, supportsGenericPackedBurst: true }
  );
  assert.equal(singlePin.modeId, SigrokModeId.FAST8);
  assert.equal(singlePin.channelMask, 0x0001);
  assert.equal(singlePin.postSamples, 100000);
  assert.equal(singlePin.samplerateKhz, 125000);

  const sparseFast8 = buildSigrokCaptureRequest(
    {
      ...DEFAULT_CONFIG,
      selectedPins: [10, 13, 17],
      sampleRateHz: 100000000,
      postSamples: 100000,
      triggerType: "either",
      triggerPin: 2,
    },
    { supportsConfigV2: true, supportsGenericPackedBurst: true }
  );
  assert.equal(sparseFast8.modeId, SigrokModeId.FAST8);
  assert.equal(sparseFast8.channelMask, 0x0089);
  assert.equal(sparseFast8.triggerChannel, 7);
  assert.equal(sparseFast8.postSamples, 100000);
});

test("rejects generic high-rate bounded requests when no capability flags are advertised", () => {
  assert.throws(
    () =>
      buildSigrokCaptureRequest({
        ...DEFAULT_CONFIG,
        selectedPins: [10],
        sampleRateHz: 125000000,
        postSamples: 100000,
      }),
    /require CONFIG_V2/
  );
});

test("rejects generic high-rate bounded requests on bit0-only legacy firmware", () => {
  assert.equal(
    getLogicAnalyzerNegotiatedCapabilityReason(
      {
        selectedPins: [10],
        sampleRateHz: 125000000,
        postSamples: 513,
      },
      { supportsConfigV2: true }
    ),
    "Connected firmware did not advertise GENERIC_PACKED_BURST; generic FAST8 high-rate capture requires HELLO server_flags bit1"
  );
  assert.throws(
    () =>
      buildSigrokCaptureRequest(
        {
          ...DEFAULT_CONFIG,
          selectedPins: [10],
          sampleRateHz: 125000000,
          postSamples: 513,
        },
        { supportsConfigV2: true }
      ),
    /GENERIC_PACKED_BURST/
  );

  assert.equal(
    getLogicAnalyzerNegotiatedCapabilityReason(
      {
        selectedPins: [10],
        sampleRateHz: 125000000,
        postSamples: 100000,
      },
      { supportsConfigV2: true }
    ),
    "Connected firmware did not advertise GENERIC_PACKED_BURST; generic bounded post-trigger samples above 65535 require HELLO server_flags bit1"
  );
  assert.throws(
    () =>
      buildSigrokCaptureRequest(
        {
          ...DEFAULT_CONFIG,
          selectedPins: [10],
          sampleRateHz: 125000000,
          postSamples: 100000,
        },
        { supportsConfigV2: true }
      ),
    /GENERIC_PACKED_BURST/
  );
});

test("keeps legacy exact WIDE11 100MHz post=100000 on bit0-only firmware", () => {
  assert.equal(
    getLogicAnalyzerNegotiatedCapabilityReason(
      {
        selectedPins: [...LOGIC_ANALYZER_CONFIG_V2_EXACT_WIDE11_PINS],
        sampleRateHz: 100000000,
        postSamples: 100000,
      },
      { supportsConfigV2: true }
    ),
    null
  );
  const request = buildSigrokCaptureRequest(
    {
      ...DEFAULT_CONFIG,
      selectedPins: [...LOGIC_ANALYZER_CONFIG_V2_EXACT_WIDE11_PINS],
      sampleRateHz: 100000000,
      postSamples: 100000,
    },
    { supportsConfigV2: true }
  );
  assert.equal(request.modeId, SigrokModeId.WIDE11);
  assert.equal(request.postSamples, 100000);
});

test("rejects high-rate stream on bit0-only legacy firmware and keeps lower-rate stream behavior", () => {
  const request = buildSigrokCaptureRequest(
    {
      ...DEFAULT_CONFIG,
        selectedPins: [10, 18, 20],
      triggerType: "rising",
      triggerPin: 1,
      postSamples: 2048,
    },
    { stream: true }
  );

  assert.equal(request.modeId, SigrokModeId.WIDE11);
  assert.equal(request.triggerType, SigrokTriggerType.RISING);
  assert.equal(request.triggerChannel, 8);
  assert.equal(request.postSamples, 0);

  assert.throws(
    () =>
      buildSigrokCaptureRequest(
        {
          ...DEFAULT_CONFIG,
          selectedPins: [10],
          sampleRateHz: 125000000,
          postSamples: 2048,
        },
        { supportsConfigV2: true, stream: true }
      ),
    /GENERIC_PACKED_BURST/
  );
});

test("builds generic high-rate stream requests with post=0 only when bit1 generic packed burst is negotiated", () => {
  const request = buildSigrokCaptureRequest(
    {
      ...DEFAULT_CONFIG,
      selectedPins: [10],
      sampleRateHz: 125000000,
      postSamples: 100000,
    },
    { supportsConfigV2: true, supportsGenericPackedBurst: true, stream: true }
  );

  assert.equal(request.modeId, SigrokModeId.FAST8);
  assert.equal(request.preSamples, 0);
  assert.equal(request.postSamples, 0);
  assert.equal(request.samplerateKhz, 125000);
});

test("forces stream pre and post samples to zero", () => {
  const request = buildSigrokCaptureRequest(
    {
      ...DEFAULT_CONFIG,
      selectedPins: [13],
      sampleRateHz: 1000000,
      triggerType: "rising",
      preSamples: 128,
      postSamples: 128,
    },
    { ...PRE_TRIGGER_CAPS, stream: true }
  );

  assert.equal(request.preSamples, 0);
  assert.equal(request.postSamples, 0);
});

test("classifies exact device-capacity stop, 99999 unexpected stop, and manual stop", () => {
  assert.equal(
    classifyLogicAnalyzerStreamStop({
      config: { selectedPins: [10], sampleRateHz: 125000000 },
      sampleCount: LOGIC_ANALYZER_HIGH_RATE_MAX_SAMPLES,
      userInitiated: false,
    }),
    "auto_buffer_full"
  );
  assert.equal(
    classifyLogicAnalyzerStreamStop({
      config: { selectedPins: [10], sampleRateHz: 125000000 },
      sampleCount: LOGIC_ANALYZER_HIGH_RATE_MAX_SAMPLES,
      userInitiated: true,
    }),
    "manual"
  );
  assert.equal(
    classifyLogicAnalyzerStreamStop({
      config: { selectedPins: [10], sampleRateHz: 125000000 },
      sampleCount: 99999,
      userInitiated: false,
    }),
    "unexpected_stop"
  );
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
    [10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20]
  );
  assert.deepEqual(LOGIC_ANALYZER_SIGROK_DISABLED_PINS, [7, 8, 9]);
  assert.equal(isWebSigrokPinSupported(10), true);
  assert.equal(isWebSigrokPinSupported(29), false);
  assert.equal(isWebSigrokPinSupported(7), false);
  assert.equal(isWebSigrokPinSupported(9), false);
  assert.equal(LOGIC_ANALYZER_WEB_STREAM_MAX_SAMPLE_RATE_HZ, 25000000);
});

test("maps logical channels for FAST8 and WIDE11 safely", () => {
  assert.equal(mapSigrokLogicalChannel(10, SigrokModeId.FAST8), 0);
  assert.equal(mapSigrokLogicalChannel(17, SigrokModeId.FAST8), 7);
  assert.equal(mapSigrokLogicalChannel(18, SigrokModeId.FAST8), null);
  assert.equal(mapSigrokLogicalChannel(18, SigrokModeId.WIDE11), 8);
  assert.equal(mapSigrokLogicalChannel(20, SigrokModeId.WIDE11), 10);
  assert.equal(mapSigrokLogicalChannel(29, SigrokModeId.WIDE11), null);
  assert.equal(mapSigrokLogicalChannel(7, SigrokModeId.WIDE11), null);
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

  assert.deepEqual(capture.config.selectedPins, [8, 18]);
  assert.equal(capture.config.pinCount, 2);
  assert.equal(capture.config.pinBase, 8);
  assert.equal(capture.config.triggerPin, 1);
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
