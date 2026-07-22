import {
  getLogicAnalyzerActualSampleRate,
  getLogicAnalyzerSamplePeriodPs,
} from "./logicAnalyzer.ts";
import type { LogicAnalyzerCapture, LogicAnalyzerCaptureConfig } from "./types";

const DECODER_MODULE_PATH = "assets/decoder/logic-decoder.js";

export const LOGIC_DECODER_REQUEST_SCHEMA_VERSION = "radxa.logic-decoder.request.v1";
export const LOGIC_DECODER_RESULT_SCHEMA_VERSION = "radxa.logic-decoder.result.v1";

export type LogicDecoderProtocolName = "uart" | "i2c" | "spi";
export type LogicDecoderParity = "none" | "even" | "odd";
export type LogicDecoderBitOrder = "msbfirst" | "lsbfirst";
export type LogicDecoderDiagnosticSeverity = "info" | "warning" | "error";
export type LogicDecoderSignalName = "rx" | "scl" | "sda" | "sclk" | "mosi" | "miso" | "cs";

export interface LogicDecoderChannelMapping {
  name: string;
  bit: number;
}

export interface LogicDecoderPackedSamples {
  sampleRateHz: number;
  samplePeriodPs: number;
  sampleCount: number;
  channels: LogicDecoderChannelMapping[];
  words: number[];
}

export interface LogicDecoderAnnotation {
  startSample: number;
  endSample: number;
  row: string;
  class: string;
  shortText: string;
  longText: string;
  data: unknown;
}

export interface LogicDecoderDiagnostic {
  startSample: number;
  endSample: number;
  severity: LogicDecoderDiagnosticSeverity;
  code: string;
  message: string;
}

export interface LogicDecoderResult {
  schemaVersion: string;
  annotations: LogicDecoderAnnotation[];
  diagnostics: LogicDecoderDiagnostic[];
}

export interface LogicDecoderUartConfig {
  rxPin: number | null;
  baud: number;
  dataBits: number;
  parity: LogicDecoderParity;
  stopBits: number;
  inverted: boolean;
}

export interface LogicDecoderI2cConfig {
  sclPin: number | null;
  sdaPin: number | null;
}

export interface LogicDecoderSpiConfig {
  sclkPin: number | null;
  mosiPin: number | null;
  misoPin: number | null;
  csPin: number | null;
  csActiveHigh: boolean;
  mode: number;
  bitOrder: LogicDecoderBitOrder;
  bitsPerWord: number;
}

export interface LogicDecoderProtocolConfigs {
  uart: LogicDecoderUartConfig;
  i2c: LogicDecoderI2cConfig;
  spi: LogicDecoderSpiConfig;
}

export interface LogicDecoderRequest {
  schemaVersion: string;
  samples: LogicDecoderPackedSamples;
  protocol:
    | { name: "uart"; options: { rx: string; baud: number; dataBits: number; parity: LogicDecoderParity; stopBits: number; inverted: boolean } }
    | { name: "i2c"; options: { scl: string; sda: string } }
    | { name: "spi"; options: { sclk: string; mosi: string | null; miso: string | null; cs: string | null; csActiveHigh: boolean; mode: number; bitOrder: LogicDecoderBitOrder; bitsPerWord: number } };
}

export interface PositionedLogicDecoderAnnotation extends LogicDecoderAnnotation {
  lane: number;
  rowOrder: number;
}

export interface LogicDecoderAnnotationLayout {
  annotations: PositionedLogicDecoderAnnotation[];
  laneCount: number;
}

interface LogicDecoderSuccessEnvelope {
  ok: true;
  result: LogicDecoderResult;
}

interface LogicDecoderFailureEnvelope {
  ok: false;
  error: string;
}

type LogicDecoderEnvelope = LogicDecoderSuccessEnvelope | LogicDecoderFailureEnvelope;

interface LoadedLogicDecoderModule {
  decodeLogic: (requestJson: string) => LogicDecoderEnvelope;
}

type Translator = (key: string) => string;

let cachedModulePromise: Promise<LoadedLogicDecoderModule> | null = null;

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

function isFiniteInteger(value: unknown): value is number {
  return typeof value === "number" && Number.isFinite(value) && Number.isInteger(value);
}

function isNonNegativeInteger(value: unknown): value is number {
  return isFiniteInteger(value) && value >= 0;
}

function isPositiveInteger(value: unknown): value is number {
  return isFiniteInteger(value) && value > 0;
}

function isBoolean(value: unknown): value is boolean {
  return typeof value === "boolean";
}

function isString(value: unknown): value is string {
  return typeof value === "string";
}

function isFunction(value: unknown): value is (...args: unknown[]) => unknown {
  return typeof value === "function";
}

function nextDistinctPin(
  availablePins: readonly number[],
  usedPins: Set<number>,
  preferredPin: number | null,
  allowNull: boolean
): number | null {
  if (
    preferredPin != null &&
    availablePins.includes(preferredPin) &&
    !usedPins.has(preferredPin)
  ) {
    usedPins.add(preferredPin);
    return preferredPin;
  }

  const fallback = availablePins.find((pin) => !usedPins.has(pin));
  if (fallback == null) {
    return allowNull ? null : availablePins[0] ?? null;
  }

  usedPins.add(fallback);
  return fallback;
}

function coerceOptionalPin(value: number | null, availablePins: readonly number[]): number | null {
  return value != null && availablePins.includes(value) ? value : null;
}

function coerceRequiredPin(value: number | null, availablePins: readonly number[]): number | null {
  if (value != null && availablePins.includes(value)) {
    return value;
  }
  return availablePins[0] ?? null;
}

function roundPositiveInteger(value: number | null): number {
  if (value == null || !Number.isFinite(value) || value <= 0) {
    return 1;
  }
  return Math.max(1, Math.round(value));
}

function captureBitMask(bitCount: number): number {
  if (bitCount <= 0) return 0;
  if (bitCount >= 16) return 0xffff;
  return (1 << bitCount) - 1;
}

function validateAnnotation(value: unknown, index: number): LogicDecoderAnnotation {
  if (!isRecord(value)) {
    throw new Error(`Decoder annotation ${index} must be an object`);
  }

  const annotation: LogicDecoderAnnotation = {
    startSample: readNonNegativeInteger(value.startSample, `annotations[${index}].startSample`),
    endSample: readPositiveInteger(value.endSample, `annotations[${index}].endSample`),
    row: readString(value.row, `annotations[${index}].row`),
    class: readString(value.class, `annotations[${index}].class`),
    shortText: readString(value.shortText, `annotations[${index}].shortText`),
    longText: readString(value.longText, `annotations[${index}].longText`),
    data: value.data,
  };

  if (annotation.endSample <= annotation.startSample) {
    throw new Error(`Decoder annotation ${index} must end after it starts`);
  }

  return annotation;
}

function validateDiagnostic(value: unknown, index: number): LogicDecoderDiagnostic {
  if (!isRecord(value)) {
    throw new Error(`Decoder diagnostic ${index} must be an object`);
  }

  const severity = readString(value.severity, `diagnostics[${index}].severity`);
  if (severity !== "info" && severity !== "warning" && severity !== "error") {
    throw new Error(`diagnostics[${index}].severity must be info, warning, or error`);
  }

  const diagnostic: LogicDecoderDiagnostic = {
    startSample: readNonNegativeInteger(value.startSample, `diagnostics[${index}].startSample`),
    endSample: readPositiveInteger(value.endSample, `diagnostics[${index}].endSample`),
    severity,
    code: readString(value.code, `diagnostics[${index}].code`),
    message: readString(value.message, `diagnostics[${index}].message`),
  };

  if (diagnostic.endSample <= diagnostic.startSample) {
    throw new Error(`Decoder diagnostic ${index} must end after it starts`);
  }

  return diagnostic;
}

function readString(value: unknown, path: string): string {
  if (!isString(value) || value.length === 0) {
    throw new Error(`${path} must be a non-empty string`);
  }
  return value;
}

function readNonNegativeInteger(value: unknown, path: string): number {
  if (!isNonNegativeInteger(value)) {
    throw new Error(`${path} must be a non-negative integer`);
  }
  return value;
}

function readPositiveInteger(value: unknown, path: string): number {
  if (!isPositiveInteger(value)) {
    throw new Error(`${path} must be a positive integer`);
  }
  return value;
}

function validateSampleBounds(startSample: number, endSample: number, sampleCount: number, path: string) {
  if (startSample >= sampleCount) {
    throw new Error(`${path} start exceeds sample count`);
  }
  if (endSample > sampleCount) {
    throw new Error(`${path} end exceeds sample count`);
  }
}

function validateDecoderResult(value: unknown, sampleCount?: number): LogicDecoderResult {
  if (!isRecord(value)) {
    throw new Error("Decoder result must be an object");
  }

  const schemaVersion = readString(value.schemaVersion, "result.schemaVersion");
  if (schemaVersion !== LOGIC_DECODER_RESULT_SCHEMA_VERSION) {
    throw new Error(`Unsupported decoder result schema: ${schemaVersion}`);
  }
  if (!Array.isArray(value.annotations)) {
    throw new Error("result.annotations must be an array");
  }
  if (!Array.isArray(value.diagnostics)) {
    throw new Error("result.diagnostics must be an array");
  }

  const annotations = value.annotations.map(validateAnnotation);
  const diagnostics = value.diagnostics.map(validateDiagnostic);

  if (sampleCount != null) {
    annotations.forEach((annotation, index) => {
      validateSampleBounds(
        annotation.startSample,
        annotation.endSample,
        sampleCount,
        `annotations[${index}]`
      );
    });
    diagnostics.forEach((diagnostic, index) => {
      validateSampleBounds(
        diagnostic.startSample,
        diagnostic.endSample,
        sampleCount,
        `diagnostics[${index}]`
      );
    });
  }

  return {
    schemaVersion,
    annotations,
    diagnostics,
  };
}

export function parseLogicDecoderEnvelope(
  jsonText: string,
  sampleCount?: number
): LogicDecoderEnvelope {
  let parsed: unknown;
  try {
    parsed = JSON.parse(jsonText) as unknown;
  } catch (error) {
    throw new Error(
      `Decoder returned invalid JSON: ${error instanceof Error ? error.message : String(error)}`
    );
  }

  if (!isRecord(parsed) || !isBoolean(parsed.ok)) {
    throw new Error("Decoder response must contain an ok boolean");
  }

  if (parsed.ok) {
    return {
      ok: true,
      result: validateDecoderResult(parsed.result, sampleCount),
    };
  }

  return {
    ok: false,
    error: readString(parsed.error, "error"),
  };
}

export function resolveLogicDecoderModuleUrl(baseUri?: string | null): string {
  const resolvedBaseUri =
    baseUri ??
    (typeof document !== "undefined" && typeof document.baseURI === "string"
      ? document.baseURI
      : null);

  if (!resolvedBaseUri) {
    return `/${DECODER_MODULE_PATH}`;
  }

  return new URL(DECODER_MODULE_PATH, resolvedBaseUri).toString();
}

function replaceTokens(template: string, values: Record<string, string | number>): string {
  return Object.entries(values).reduce(
    (text, [key, value]) => text.replaceAll(`{${key}}`, String(value)),
    template
  );
}

function localizeTemplate(
  translator: Translator,
  key: string,
  values?: Record<string, string | number>
): string {
  const template = translator(key);
  return values ? replaceTokens(template, values) : template;
}

export function localizeLogicDecoderErrorMessage(
  message: string,
  translator: Translator
): string {
  const capturePinMatch = /^([A-Z0-9_]+) pin GP(\d+) is not present in this capture$/.exec(message);
  if (capturePinMatch) {
    return localizeTemplate(translator, "logicAnalyzer.decoder.error.pinMissing", {
      signal: capturePinMatch[1] ?? "",
      pin: capturePinMatch[2] ?? "",
    });
  }

  const duplicateMatch = /^([A-Z0-9_]+) and ([A-Z0-9_]+) cannot use the same channel$/.exec(message);
  if (duplicateMatch) {
    return localizeTemplate(translator, "logicAnalyzer.decoder.error.duplicateChannel", {
      signal: duplicateMatch[1] ?? "",
      other: duplicateMatch[2] ?? "",
    });
  }

  if (message === "Decode requires a completed capture with at least one sample") {
    return translator("logicAnalyzer.decoder.error.noSamples");
  }
  if (message === "Select a capture channel for UART RX") {
    return translator("logicAnalyzer.decoder.error.uartRx");
  }
  if (message === "Select capture channels for I2C SCL and SDA") {
    return translator("logicAnalyzer.decoder.error.i2cPins");
  }
  if (message === "Select a capture channel for SPI clock") {
    return translator("logicAnalyzer.decoder.error.spiClock");
  }
  if (message === "Select at least one SPI data channel") {
    return translator("logicAnalyzer.decoder.error.spiData");
  }
  if (message === "Decoder module did not return a module namespace object") {
    return translator("logicAnalyzer.decoder.error.moduleNamespace");
  }
  if (message === "Decoder module is missing its default WASM initializer") {
    return translator("logicAnalyzer.decoder.error.moduleInit");
  }
  if (message === "Decoder module is missing decodeLogic(requestJson)") {
    return translator("logicAnalyzer.decoder.error.moduleDecodeLogic");
  }
  if (message === "Decoder returned a non-string response") {
    return translator("logicAnalyzer.decoder.error.invalidResponseType");
  }

  if (
    message.startsWith("Decoder returned invalid JSON:") ||
    message === "Decoder response must contain an ok boolean" ||
    message === "Decoder result must be an object" ||
    message.startsWith("Unsupported decoder result schema:") ||
    message === "result.annotations must be an array" ||
    message === "result.diagnostics must be an array" ||
    message.startsWith("annotations[") ||
    message.startsWith("diagnostics[") ||
    message.startsWith("Decoder annotation ") ||
    message.startsWith("Decoder diagnostic ")
  ) {
    return translator("logicAnalyzer.decoder.error.invalidResult");
  }

  return message;
}

export function getLogicDecoderCapturePins(config: LogicAnalyzerCaptureConfig): number[] {
  if (Array.isArray(config.selectedPins) && config.selectedPins.length > 0) {
    return config.selectedPins.slice(0, config.pinCount);
  }

  return Array.from({ length: config.pinCount }, (_, index) => config.pinBase + index);
}

export function createDefaultLogicDecoderConfigs(
  availablePins: readonly number[]
): LogicDecoderProtocolConfigs {
  return reconcileLogicDecoderConfigs(
    {
      uart: {
        rxPin: availablePins[0] ?? null,
        baud: 115200,
        dataBits: 8,
        parity: "none",
        stopBits: 1,
        inverted: false,
      },
      i2c: {
        sclPin: availablePins[0] ?? null,
        sdaPin: availablePins[1] ?? availablePins[0] ?? null,
      },
      spi: {
        sclkPin: availablePins[0] ?? null,
        mosiPin: availablePins[1] ?? null,
        misoPin: availablePins[2] ?? null,
        csPin: availablePins[3] ?? null,
        csActiveHigh: false,
        mode: 0,
        bitOrder: "msbfirst",
        bitsPerWord: 8,
      },
    },
    availablePins
  );
}

export function reconcileLogicDecoderConfigs(
  configs: LogicDecoderProtocolConfigs,
  availablePins: readonly number[]
): LogicDecoderProtocolConfigs {
  const uartUsed = new Set<number>();
  const i2cUsed = new Set<number>();
  const spiUsed = new Set<number>();

  const uart: LogicDecoderUartConfig = {
    ...configs.uart,
    rxPin: nextDistinctPin(availablePins, uartUsed, configs.uart.rxPin, false),
  };

  const i2c: LogicDecoderI2cConfig = {
    sclPin: nextDistinctPin(availablePins, i2cUsed, configs.i2c.sclPin, false),
    sdaPin: nextDistinctPin(availablePins, i2cUsed, configs.i2c.sdaPin, false),
  };

  const spi: LogicDecoderSpiConfig = {
    ...configs.spi,
    sclkPin: nextDistinctPin(availablePins, spiUsed, configs.spi.sclkPin, false),
    mosiPin: nextDistinctPin(availablePins, spiUsed, configs.spi.mosiPin, true),
    misoPin: nextDistinctPin(availablePins, spiUsed, configs.spi.misoPin, true),
    csPin: nextDistinctPin(availablePins, spiUsed, configs.spi.csPin, true),
  };

  return {
    uart: {
      ...uart,
      baud: roundPositiveInteger(configs.uart.baud),
      dataBits: Math.min(9, Math.max(5, roundPositiveInteger(configs.uart.dataBits))),
      stopBits: Math.min(2, Math.max(1, roundPositiveInteger(configs.uart.stopBits))),
    },
    i2c: {
      sclPin: i2c.sclPin,
      sdaPin: i2c.sdaPin,
    },
    spi: {
      ...spi,
      mode: Math.min(3, Math.max(0, Math.trunc(configs.spi.mode))),
      bitsPerWord: Math.min(16, Math.max(1, roundPositiveInteger(configs.spi.bitsPerWord))),
    },
  };
}

export function updateLogicDecoderSignalPin(
  configs: LogicDecoderProtocolConfigs,
  protocol: LogicDecoderProtocolName,
  signal: LogicDecoderSignalName,
  pin: number | null,
  availablePins: readonly number[]
): LogicDecoderProtocolConfigs {
  const next = {
    uart: { ...configs.uart },
    i2c: { ...configs.i2c },
    spi: { ...configs.spi },
  };

  if (protocol === "uart" && signal === "rx") {
    next.uart.rxPin = coerceRequiredPin(pin, availablePins);
  }

  if (protocol === "i2c") {
    if (signal === "scl") next.i2c.sclPin = coerceRequiredPin(pin, availablePins);
    if (signal === "sda") next.i2c.sdaPin = coerceRequiredPin(pin, availablePins);
  }

  if (protocol === "spi") {
    if (signal === "sclk") next.spi.sclkPin = coerceRequiredPin(pin, availablePins);
    if (signal === "mosi") next.spi.mosiPin = coerceOptionalPin(pin, availablePins);
    if (signal === "miso") next.spi.misoPin = coerceOptionalPin(pin, availablePins);
    if (signal === "cs") next.spi.csPin = coerceOptionalPin(pin, availablePins);
  }

  return reconcileLogicDecoderConfigs(next, availablePins);
}

function getMappedChannelBit(
  capturePins: readonly number[],
  pin: number,
  signal: string
): LogicDecoderChannelMapping {
  const bit = capturePins.indexOf(pin);
  if (bit < 0) {
    throw new Error(`${signal.toUpperCase()} pin GP${pin} is not present in this capture`);
  }
  return { name: signal, bit };
}

function validateUniquePins(entries: Array<[string, number | null]>) {
  const seen = new Map<number, string>();
  for (const [signal, pin] of entries) {
    if (pin == null) continue;
    const previous = seen.get(pin);
    if (previous) {
      throw new Error(`${signal.toUpperCase()} and ${previous.toUpperCase()} cannot use the same channel`);
    }
    seen.set(pin, signal);
  }
}

function buildPackedWords(capture: LogicAnalyzerCapture): number[] {
  const pinCount = getLogicDecoderCapturePins(capture.config).length;
  const mask = captureBitMask(pinCount);

  return capture.samples.slice(0, capture.sampleCount).map((sample) => Math.trunc(sample.values) & mask);
}

export function buildLogicDecoderRequest(
  capture: LogicAnalyzerCapture,
  protocol: LogicDecoderProtocolName,
  configs: LogicDecoderProtocolConfigs
): LogicDecoderRequest {
  const capturePins = getLogicDecoderCapturePins(capture.config);
  const sampleRateHz = roundPositiveInteger(getLogicAnalyzerActualSampleRate(capture.config));
  const samplePeriodPs = roundPositiveInteger(getLogicAnalyzerSamplePeriodPs(capture.config));
  const samples: LogicDecoderPackedSamples = {
    sampleRateHz,
    samplePeriodPs,
    sampleCount: capture.sampleCount,
    channels: [],
    words: buildPackedWords(capture),
  };

  if (capture.sampleCount === 0) {
    throw new Error("Decode requires a completed capture with at least one sample");
  }

  if (protocol === "uart") {
    const uart = configs.uart;
    if (uart.rxPin == null) {
      throw new Error("Select a capture channel for UART RX");
    }

    samples.channels = [getMappedChannelBit(capturePins, uart.rxPin, "rx")];
    return {
      schemaVersion: LOGIC_DECODER_REQUEST_SCHEMA_VERSION,
      samples,
      protocol: {
        name: "uart",
        options: {
          rx: "rx",
          baud: roundPositiveInteger(uart.baud),
          dataBits: Math.min(9, Math.max(5, Math.trunc(uart.dataBits))),
          parity: uart.parity,
          stopBits: Math.min(2, Math.max(1, Math.trunc(uart.stopBits))),
          inverted: uart.inverted,
        },
      },
    };
  }

  if (protocol === "i2c") {
    const i2c = configs.i2c;
    if (i2c.sclPin == null || i2c.sdaPin == null) {
      throw new Error("Select capture channels for I2C SCL and SDA");
    }
    validateUniquePins([
      ["scl", i2c.sclPin],
      ["sda", i2c.sdaPin],
    ]);

    samples.channels = [
      getMappedChannelBit(capturePins, i2c.sclPin, "scl"),
      getMappedChannelBit(capturePins, i2c.sdaPin, "sda"),
    ];

    return {
      schemaVersion: LOGIC_DECODER_REQUEST_SCHEMA_VERSION,
      samples,
      protocol: {
        name: "i2c",
        options: {
          scl: "scl",
          sda: "sda",
        },
      },
    };
  }

  const spi = configs.spi;
  if (spi.sclkPin == null) {
    throw new Error("Select a capture channel for SPI clock");
  }
  if (spi.mosiPin == null && spi.misoPin == null) {
    throw new Error("Select at least one SPI data channel");
  }

  validateUniquePins([
    ["sclk", spi.sclkPin],
    ["mosi", spi.mosiPin],
    ["miso", spi.misoPin],
    ["cs", spi.csPin],
  ]);

  const channels: LogicDecoderChannelMapping[] = [getMappedChannelBit(capturePins, spi.sclkPin, "sclk")];
  if (spi.mosiPin != null) {
    channels.push(getMappedChannelBit(capturePins, spi.mosiPin, "mosi"));
  }
  if (spi.misoPin != null) {
    channels.push(getMappedChannelBit(capturePins, spi.misoPin, "miso"));
  }
  if (spi.csPin != null) {
    channels.push(getMappedChannelBit(capturePins, spi.csPin, "cs"));
  }
  samples.channels = channels;

  return {
    schemaVersion: LOGIC_DECODER_REQUEST_SCHEMA_VERSION,
    samples,
    protocol: {
      name: "spi",
      options: {
        sclk: "sclk",
        mosi: spi.mosiPin == null ? null : "mosi",
        miso: spi.misoPin == null ? null : "miso",
        cs: spi.csPin == null ? null : "cs",
        csActiveHigh: spi.csActiveHigh,
        mode: Math.min(3, Math.max(0, Math.trunc(spi.mode))),
        bitOrder: spi.bitOrder,
        bitsPerWord: Math.min(16, Math.max(1, Math.trunc(spi.bitsPerWord))),
      },
    },
  };
}

export function layoutLogicDecoderAnnotations(
  annotations: readonly LogicDecoderAnnotation[]
): LogicDecoderAnnotationLayout {
  const rowOrder = new Map<string, number>();
  const perRow = new Map<string, LogicDecoderAnnotation[]>();

  for (const annotation of annotations) {
    if (!rowOrder.has(annotation.row)) {
      rowOrder.set(annotation.row, rowOrder.size);
    }
    const rowAnnotations = perRow.get(annotation.row);
    if (rowAnnotations) {
      rowAnnotations.push(annotation);
    } else {
      perRow.set(annotation.row, [annotation]);
    }
  }

  const positioned: PositionedLogicDecoderAnnotation[] = [];
  let laneOffset = 0;

  for (const [row, rowAnnotations] of perRow) {
    const laneEnds: number[] = [];
    const sorted = [...rowAnnotations].sort((left, right) => {
      if (left.startSample !== right.startSample) {
        return left.startSample - right.startSample;
      }
      return left.endSample - right.endSample;
    });

    for (const annotation of sorted) {
      let laneIndex = laneEnds.findIndex((endSample) => annotation.startSample >= endSample);
      if (laneIndex < 0) {
        laneIndex = laneEnds.length;
        laneEnds.push(annotation.endSample);
      } else {
        laneEnds[laneIndex] = annotation.endSample;
      }

      positioned.push({
        ...annotation,
        lane: laneOffset + laneIndex,
        rowOrder: rowOrder.get(row) ?? 0,
      });
    }

    laneOffset += Math.max(1, laneEnds.length);
  }

  return {
    annotations: positioned,
    laneCount: laneOffset,
  };
}

async function importLogicDecoderModule(): Promise<LoadedLogicDecoderModule> {
  const namespace: unknown = await import(
    /* @vite-ignore */ resolveLogicDecoderModuleUrl()
  );
  if (!isRecord(namespace)) {
    throw new Error("Decoder module did not return a module namespace object");
  }

  const init = namespace.default;
  const decodeLogic = namespace.decodeLogic;

  if (!isFunction(init)) {
    throw new Error("Decoder module is missing its default WASM initializer");
  }
  if (!isFunction(decodeLogic)) {
    throw new Error("Decoder module is missing decodeLogic(requestJson)");
  }

  await Promise.resolve(init());

  return {
    decodeLogic: (requestJson: string) => {
      const decodeLogicFunction: (requestJson: string) => unknown = decodeLogic;
      const response = decodeLogicFunction(requestJson);
      if (!isString(response)) {
        throw new Error("Decoder returned a non-string response");
      }
      return parseLogicDecoderEnvelope(response);
    },
  };
}

export async function loadLogicDecoderModule(): Promise<LoadedLogicDecoderModule> {
  if (!cachedModulePromise) {
    cachedModulePromise = importLogicDecoderModule().catch((error) => {
      cachedModulePromise = null;
      throw error;
    });
  }

  return cachedModulePromise;
}

export async function decodeLogicCapture(
  request: LogicDecoderRequest
): Promise<LogicDecoderResult> {
  const module = await loadLogicDecoderModule();
  const envelope = module.decodeLogic(JSON.stringify(request));
  if (!envelope.ok) {
    throw new Error(envelope.error);
  }
  return validateDecoderResult(envelope.result, request.samples.sampleCount);
}
