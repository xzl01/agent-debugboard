import assert from "node:assert/strict";
import test from "node:test";
import {
  buildLogicDecoderRequest,
  createDefaultLogicDecoderConfigs,
  layoutLogicDecoderAnnotations,
  localizeLogicDecoderErrorMessage,
  parseLogicDecoderEnvelope,
  resolveLogicDecoderModuleUrl,
  updateLogicDecoderSignalPin,
} from "./logicDecoder.ts";
import type { LogicAnalyzerCapture } from "./types.ts";

function captureFixture(): LogicAnalyzerCapture {
  return {
    state: "done",
    config: {
      pinCount: 2,
      pinBase: 13,
      selectedPins: [13, 29],
      sampleRateHz: 100000000,
      requestedSampleRateHz: 125000000,
      actualSampleRateHz: 124800000,
      samplePeriodPs: 8012.820512820513,
      triggerPin: 0,
    },
    sampleCount: 4,
    triggerIndex: 1,
    samples: [
      { timestampUs: 0, values: 0b00 },
      { timestampUs: 1, values: 0b10 },
      { timestampUs: 2, values: 0b01 },
      { timestampUs: 3, values: 0b11 },
    ],
  };
}

test("builds decoder requests against packed capture bit positions", () => {
  const capture = captureFixture();
  const configs = createDefaultLogicDecoderConfigs([13, 29]);
  configs.uart.rxPin = 29;

  const request = buildLogicDecoderRequest(capture, "uart", configs);

  assert.equal(request.samples.sampleRateHz, 124800000);
  assert.equal(request.samples.samplePeriodPs, 8013);
  assert.deepEqual(request.samples.words, [0, 2, 1, 3]);
  assert.deepEqual(request.samples.channels, [{ name: "rx", bit: 1 }]);
  assert.equal(request.protocol.name, "uart");
  assert.equal(request.protocol.options.rx, "rx");
});

test("reconciles duplicate signal assignments onto distinct capture pins", () => {
  const configs = createDefaultLogicDecoderConfigs([7, 8, 9]);

  const next = updateLogicDecoderSignalPin(configs, "i2c", "sda", 7, [7, 8, 9]);

  assert.equal(next.i2c.sclPin, 7);
  assert.equal(next.i2c.sdaPin, 8);
});

test("validates decoder success and failure envelopes", () => {
  const success = parseLogicDecoderEnvelope(
    JSON.stringify({
      ok: true,
      result: {
        schemaVersion: "radxa.logic-decoder.result.v1",
        annotations: [
          {
            startSample: 1,
            endSample: 3,
            row: "uart",
            class: "data",
            shortText: "0x55",
            longText: "UART data 0x55",
            data: { value: 0x55 },
          },
        ],
        diagnostics: [],
      },
    })
  );

  assert.equal(success.ok, true);
  if (success.ok) {
    assert.equal(success.result.annotations[0]?.shortText, "0x55");
  }

  const failure = parseLogicDecoderEnvelope(JSON.stringify({ ok: false, error: "bad request" }));
  assert.equal(failure.ok, false);
  if (!failure.ok) {
    assert.equal(failure.error, "bad request");
  }

  assert.throws(
    () =>
      parseLogicDecoderEnvelope(
        JSON.stringify({
          ok: true,
          result: {
            schemaVersion: "wrong.schema.v1",
            annotations: [],
            diagnostics: [],
          },
        })
      ),
    /Unsupported decoder result schema/
  );
});

test("rejects annotations and diagnostics that exceed capture bounds", () => {
  assert.throws(
    () =>
      parseLogicDecoderEnvelope(
        JSON.stringify({
          ok: true,
          result: {
            schemaVersion: "radxa.logic-decoder.result.v1",
            annotations: [
              {
                startSample: 3,
                endSample: 5,
                row: "uart",
                class: "data",
                shortText: "0x55",
                longText: "UART data 0x55",
                data: {},
              },
            ],
            diagnostics: [],
          },
        }),
        4
      ),
    /annotations\[0\] end exceeds sample count/
  );

  assert.throws(
    () =>
      parseLogicDecoderEnvelope(
        JSON.stringify({
          ok: true,
          result: {
            schemaVersion: "radxa.logic-decoder.result.v1",
            annotations: [],
            diagnostics: [
              {
                startSample: 4,
                endSample: 5,
                severity: "warning",
                code: "overflow",
                message: "too far",
              },
            ],
          },
        }),
        4
      ),
    /diagnostics\[0\] start exceeds sample count/
  );
});

test("resolves decoder module URL against baseURI and preserves root fallback", () => {
  assert.equal(
    resolveLogicDecoderModuleUrl("https://example.com/agent-debugboard/"),
    "https://example.com/agent-debugboard/assets/decoder/logic-decoder.js"
  );
  assert.equal(
    resolveLogicDecoderModuleUrl("https://example.com/"),
    "https://example.com/assets/decoder/logic-decoder.js"
  );
  assert.equal(resolveLogicDecoderModuleUrl(null), "/assets/decoder/logic-decoder.js");
});

test("localizes UI-authored decoder errors while preserving unknown messages", () => {
  const t = (key: string) => key;

  assert.equal(
    localizeLogicDecoderErrorMessage("Select a capture channel for UART RX", t),
    "logicAnalyzer.decoder.error.uartRx"
  );
  assert.equal(
    localizeLogicDecoderErrorMessage("SCL and SDA cannot use the same channel", t),
    "logicAnalyzer.decoder.error.duplicateChannel"
  );
  assert.equal(
    localizeLogicDecoderErrorMessage("decoder-native-error", t),
    "decoder-native-error"
  );
});

test("lays overlapping annotations into separate lanes while preserving rows", () => {
  const layout = layoutLogicDecoderAnnotations([
    {
      startSample: 0,
      endSample: 10,
      row: "spi",
      class: "word",
      shortText: "MOSI 0xAA",
      longText: "SPI MOSI 0xAA",
      data: {},
    },
    {
      startSample: 4,
      endSample: 12,
      row: "spi",
      class: "word",
      shortText: "MISO 0x55",
      longText: "SPI MISO 0x55",
      data: {},
    },
    {
      startSample: 2,
      endSample: 3,
      row: "marker",
      class: "event",
      shortText: "S",
      longText: "START",
      data: {},
    },
  ]);

  assert.equal(layout.laneCount, 3);
  assert.deepEqual(
    layout.annotations.map((annotation) => ({ row: annotation.row, lane: annotation.lane })),
    [
      { row: "spi", lane: 0 },
      { row: "spi", lane: 1 },
      { row: "marker", lane: 2 },
    ]
  );
});
