/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

import { memo, useCallback, useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import {
  Activity,
  Binary,
  Download,
  Loader2,
  Radio,
  Square,
  Trash2,
  TriangleAlert,
  Zap,
} from "lucide-react";
import { Badge, Button, Card, Toggle } from "./ui";
import { LogicAnalyzerPinSelector } from "./LogicAnalyzerPinSelector";
import { activityWaveformPoints, stepWaveformPath, type WaveformPoint } from "./logicAnalyzerWaveform";
import { resolveStreamWindow, type FrozenSampleWindow } from "./logicAnalyzerStreamWindow";
import { useSigrokScope } from "@/hooks/useSigrokScope";
import type {
  LogicAnalyzerCapture,
  LogicAnalyzerConfig,
  LogicAnalyzerState,
  LogicAnalyzerTriggerType,
  SafeGpio,
  GpioPhysicalLayout,
} from "@/lib/types";
import {
  AVAILABLE_PINS,
  DEFAULT_CONFIG,
  LOGIC_ANALYZER_MAX_PRE_TRIGGER_SAMPLES,
  LOGIC_ANALYZER_MAX_PRE_TRIGGER_SAMPLE_RATE_HZ,
  LOGIC_ANALYZER_MAX_PRE_TRIGGER_TOTAL_SAMPLES,
  LOGIC_ANALYZER_MIN_PRE_TRIGGER_SAMPLE_RATE_HZ,
  SAMPLE_RATES,
  assembleBoundedSigrokCapture,
  buildSigrokCaptureRequest,
  classifyLogicAnalyzerStreamStop,
  getLogicAnalyzerMaxSamplesForConfig,
  getLogicAnalyzerPreTriggerReason,
  getLogicAnalyzerSelectionMaxSamples,
  getLogicAnalyzerStreamLimitReason,
  getLogicAnalyzerUnsupportedRateReason,
  exportToCsv,
  exportToSr,
  formatDuration,
  formatSampleCount,
  formatSamplePeriod,
  formatSampleRate,
  getLogicAnalyzerActualSampleRate,
  getLogicAnalyzerBackend,
  getLogicAnalyzerNegotiatedCapabilityReason,
  getLogicAnalyzerRequestedSampleRate,
  getLogicAnalyzerSamplePeriodPs,
  getSigrokModeForPins,
  normalizeLogicAnalyzerCapture,
  normalizeLogicAnalyzerLocalConfig,
  supportsHighRatePackedBurst,
  unpackSigrokSamples,
} from "@/lib/logicAnalyzer";
import {
  SigrokEventCode,
  SigrokModeFlag,
  SigrokModeId,
  formatSigrokErrorMessage,
  isSigrokDataFrame,
  isSigrokErrorFrame,
  isSigrokEventFrame,
  sigrokEventCodeName,
  type SigrokCapsResp,
  type SigrokEventFrame,
} from "@/lib/sigrokClient";
import {
  buildLogicDecoderRequest,
  createDefaultLogicDecoderConfigs,
  decodeLogicCapture,
  getLogicDecoderCapturePins,
  layoutLogicDecoderAnnotations,
  localizeLogicDecoderErrorMessage,
  reconcileLogicDecoderConfigs,
  updateLogicDecoderSignalPin,
  type LogicDecoderAnnotation,
  type LogicDecoderProtocolConfigs,
  type LogicDecoderProtocolName,
  type LogicDecoderResult,
  type LogicDecoderSignalName,
} from "@/lib/logicDecoder";
import { useI18n } from "@/lib/i18n";

const WAVEFORM_COLORS = [
  "rgb(var(--c-brand))",
  "rgb(var(--c-ok))",
  "rgb(var(--c-warn))",
  "rgb(var(--c-ink-dim))",
  "rgb(var(--c-brand) / 0.65)",
  "rgb(var(--c-ok) / 0.65)",
  "rgb(var(--c-warn) / 0.7)",
  "rgb(var(--c-ink) / 0.65)",
  "rgb(var(--c-brand) / 0.45)",
  "rgb(var(--c-ok) / 0.45)",
  "rgb(var(--c-warn) / 0.5)",
  "rgb(var(--c-ink-dim) / 0.55)",
  "rgb(var(--c-brand) / 0.8)",
  "rgb(var(--c-ok) / 0.8)",
  "rgb(var(--c-warn) / 0.85)",
  "rgb(var(--c-ink) / 0.8)",
];

const WIDTH = 800;
const PLOT_LEFT = 48;
const PLOT_RIGHT = 12;
const TOP_PADDING = 16;
const BOTTOM_PADDING = 16;
const CHANNEL_HEIGHT = 28;
const CHANNEL_GAP = 4;
const ANNOTATION_HEIGHT = 18;
const ANNOTATION_GAP = 4;
const ANNOTATION_MARGIN = 10;
const STREAM_BUFFER_CAP = 1000000;
const STREAM_FLUSH_MS = 150;
const STREAM_DECODE_MS = 600;
const STREAM_DECODE_MAX_SAMPLES = 8192;
const STREAM_SPAN_OPTIONS = [1024, 4096, 16384, 65536, 262144] as const;
const PROTOCOL_OPTIONS: Array<{ id: LogicDecoderProtocolName; label: string }> = [
  { id: "uart", label: "UART" },
  { id: "i2c", label: "I2C" },
  { id: "spi", label: "SPI" },
];

// CAPS only arrive after a device connection. Before that, local pre-trigger
// editing must stay available when the generic policy passes, so the policy is
// probed with a PRE_TRIGGER-optimistic capability view until real CAPS exist.
const PRE_TRIGGER_ASSUMED_CAPS: SigrokCapsResp = {
  modeCount: 2,
  modes: [SigrokModeId.FAST8, SigrokModeId.WIDE11].map((modeId) => ({
    modeId,
    modeFlags: SigrokModeFlag.PRE_TRIGGER,
    channelCount: 0,
    sampleBytes: 0,
    maxSamplerateKhz: 0,
    compression: 0,
  })),
};

function warnSigrokCleanup(action: string, cleanupError: unknown) {
  const message = cleanupError instanceof Error ? cleanupError.message : String(cleanupError);
  console.warn(`[LogicAnalyzerCard] ${action} failed: ${message}`);
}


interface DecoderRunState {
  status: "idle" | "loading" | "done";
  result: LogicDecoderResult | null;
  error: string | null;
  lastRequestSignature: string | null;
}

interface InlineNotice {
  tone: "brand" | "warn";
  message: string;
}

interface CaptureOperation {
  lifecycle: object;
  cancelled: boolean;
  cleanup: Promise<void> | null;
  stream?: { config: LogicAnalyzerConfig; actualRateHz: number };
}

// A stopped stream owns its bounded backing buffer, not a trigger capture. It
// must not inherit the bounded-capture decoder/export completion contract.
interface StreamSnapshot {
  source: "stream";
  config: LogicAnalyzerConfig;
  actualRateHz: number;
  samples: readonly number[];
  sampleCount: number;
  stopReason: "auto_buffer_full" | "manual" | "incomplete";
}

const INITIAL_DECODER_STATE: DecoderRunState = {
  status: "idle",
  result: null,
  error: null,
  lastRequestSignature: null,
};

function pinLabel(pin: number | null): string {
  return pin == null ? "—" : `GP${pin}`;
}

// Samples occupy intervals [index, index + 1). Waveforms, trigger markers and
// protocol annotations all use this same time-to-pixel mapping.
function waveformX(samplePosition: number, sampleCount: number): number {
  return PLOT_LEFT + (samplePosition / Math.max(1, sampleCount)) * (WIDTH - PLOT_LEFT - PLOT_RIGHT);
}

function replaceTokens(template: string, values: Record<string, string | number>): string {
  return Object.entries(values).reduce(
    (text, [key, value]) => text.replaceAll(`{${key}}`, String(value)),
    template
  );
}

interface StepWaveformProps {
  readonly channels: readonly (readonly WaveformPoint[])[];
  readonly pinKeys: readonly number[];
  readonly channelTop: number;
  readonly triggerLineX?: number | null;
  readonly svgHeight?: number;
}

const StepWaveform = memo(function StepWaveform({
  channels,
  pinKeys,
  channelTop,
  triggerLineX,
  svgHeight,
}: StepWaveformProps) {
  // Trigger/annotation updates can rerender the plot without changing its
  // geometry. Keep the large path strings until samples or vertical layout change.
  const paths = useMemo(() => channels.map((points, channelIndex) =>
    stepWaveformPath(points, channelTop + channelIndex * (CHANNEL_HEIGHT + CHANNEL_GAP))
  ), [channels, channelTop]);
  return (
    <>
      {pinKeys.map((pin, channelIndex) => {
        const yOffset = channelTop + channelIndex * (CHANNEL_HEIGHT + CHANNEL_GAP);
        return (
          <g key={pin}>
            <text
              x={4}
              y={yOffset + CHANNEL_HEIGHT / 2 + 4}
              className="fill-ink-dim text-[10px]"
              fontFamily="monospace"
            >
              {pinLabel(pin)}
            </text>
            <line
              x1={PLOT_LEFT}
              x2={WIDTH - PLOT_RIGHT}
              y1={yOffset + CHANNEL_HEIGHT / 2}
              y2={yOffset + CHANNEL_HEIGHT / 2}
              stroke="rgb(var(--c-line))"
              strokeDasharray="2 4"
              strokeWidth={0.5}
            />
          </g>
        );
      })}
      {triggerLineX != null && svgHeight != null && (
        <line
          data-logic-trigger=""
          x1={triggerLineX}
          x2={triggerLineX}
          y1={10}
          y2={svgHeight - 10}
          stroke="rgb(var(--c-danger))"
          strokeDasharray="4 3"
          strokeWidth={1}
        />
      )}
      {channels.map((channelPoints, channelIndex) => {
        if (channelPoints.length === 0) return null;
        return (
          <path
            key={pinKeys[channelIndex] ?? channelIndex}
            data-logic-waveform-channel={pinKeys[channelIndex]}
            d={paths[channelIndex]}
            fill="none"
            stroke={WAVEFORM_COLORS[channelIndex % WAVEFORM_COLORS.length]}
            strokeWidth={1.5}
            vectorEffect="non-scaling-stroke"
          />
        );
      })}
    </>
  );
});

function areDecoderConfigsEqual(
  left: LogicDecoderProtocolConfigs,
  right: LogicDecoderProtocolConfigs
): boolean {
  return (
    left.uart.rxPin === right.uart.rxPin &&
    left.uart.baud === right.uart.baud &&
    left.uart.dataBits === right.uart.dataBits &&
    left.uart.parity === right.uart.parity &&
    left.uart.stopBits === right.uart.stopBits &&
    left.uart.inverted === right.uart.inverted &&
    left.i2c.sclPin === right.i2c.sclPin &&
    left.i2c.sdaPin === right.i2c.sdaPin &&
    left.spi.sclkPin === right.spi.sclkPin &&
    left.spi.mosiPin === right.spi.mosiPin &&
    left.spi.misoPin === right.spi.misoPin &&
    left.spi.csPin === right.spi.csPin &&
    left.spi.csActiveHigh === right.spi.csActiveHigh &&
    left.spi.mode === right.spi.mode &&
    left.spi.bitOrder === right.spi.bitOrder &&
    left.spi.bitsPerWord === right.spi.bitsPerWord
  );
}

function annotationPalette(className: string) {
  switch (className) {
    case "start":
    case "restart":
    case "stop":
      return {
        fill: "rgb(var(--c-ok) / 0.14)",
        stroke: "rgb(var(--c-ok) / 0.45)",
        text: "rgb(var(--c-ok))",
      };
    case "address":
      return {
        fill: "rgb(var(--c-warn) / 0.14)",
        stroke: "rgb(var(--c-warn) / 0.45)",
        text: "rgb(var(--c-warn))",
      };
    default:
      return {
        fill: "rgb(var(--c-brand) / 0.14)",
        stroke: "rgb(var(--c-brand) / 0.45)",
        text: "rgb(var(--c-brand))",
      };
  }
}

function diagnosticTone(severity: string): "neutral" | "warn" | "danger" {
  if (severity === "error") return "danger";
  if (severity === "warning") return "warn";
  return "neutral";
}

function formatAnnotationRange(
  annotation: LogicDecoderAnnotation,
  sampleRateHz: number
): string {
  const startUs = (annotation.startSample / sampleRateHz) * 1_000_000;
  const endUs = (annotation.endSample / sampleRateHz) * 1_000_000;
  return `${startUs.toFixed(2)}-${endUs.toFixed(2)} us`;
}

function decoderSignalPins(
  protocol: LogicDecoderProtocolName,
  configs: LogicDecoderProtocolConfigs
): Record<string, number | null> {
  if (protocol === "uart") {
    return { rx: configs.uart.rxPin };
  }
  if (protocol === "i2c") {
    return { scl: configs.i2c.sclPin, sda: configs.i2c.sdaPin };
  }
  return {
    sclk: configs.spi.sclkPin,
    mosi: configs.spi.mosiPin,
    miso: configs.spi.misoPin,
    cs: configs.spi.csPin,
  };
}

// allow: SIZE_OK — one logic-analyzer state machine owns capture, stream, decode, and cleanup.
export function LogicAnalyzerCard({
  boardGpios,
  physicalLayout,
  workspaceTabs,
  onActivityChange,
}: {
  boardGpios?: SafeGpio[];
  physicalLayout?: GpioPhysicalLayout | null;
  workspaceTabs?: ReactNode;
  onActivityChange?: (active: boolean) => void;
}) {
  const { t } = useI18n();
  const [config, setConfig] = useState<LogicAnalyzerConfig>(DEFAULT_CONFIG);
  const [state, setState] = useState<LogicAnalyzerState>("idle");
  const [capture, setCapture] = useState<LogicAnalyzerCapture | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [isArming, setIsArming] = useState(false);
  const [decoderProtocol, setDecoderProtocol] = useState<LogicDecoderProtocolName>("uart");
  const [decoderConfigs, setDecoderConfigs] = useState<LogicDecoderProtocolConfigs>(() =>
    createDefaultLogicDecoderConfigs(DEFAULT_CONFIG.selectedPins)
  );
  const [decoderState, setDecoderState] = useState<DecoderRunState>(INITIAL_DECODER_STATE);
  const lifecycleRef = useRef<object | null>(null);
  const captureOperationRef = useRef<CaptureOperation | null>(null);
  const activityCallbackRef = useRef(onActivityChange);
  const [isStopping, setIsStopping] = useState(false);
  const boundedDecodeRunRef = useRef(0);
  const streamSamplesRef = useRef<number[]>([]);
  const streamSequenceRef = useRef(0);
  const streamTotalRef = useRef(0);
  const streamDirtyRef = useRef(false);
  const [streamSampleCount, setStreamSampleCount] = useState(0);
  const [streamWaveformVersion, setStreamWaveformVersion] = useState(0);
  const [streamSpan, setStreamSpan] = useState<number>(4096);
  const [frozenStreamWindow, setFrozenStreamWindow] = useState<FrozenSampleWindow | null>(null);
  const streamFollow = frozenStreamWindow == null;
  const [liveAnnotations, setLiveAnnotations] = useState<LogicDecoderAnnotation[]>([]);
  const [liveDecodeError, setLiveDecodeError] = useState<string | null>(null);
  const [streamNotice, setStreamNotice] = useState<InlineNotice | null>(null);
  const [streamSnapshot, setStreamSnapshot] = useState<StreamSnapshot | null>(null);

  const {
    close: sigrokClose,
    configure: sigrokConfigure,
    start: sigrokStart,
    stop: sigrokStop,
    ensureConnected: sigrokEnsureConnected,
    getServerCapabilities: sigrokGetServerCapabilities,
    readCaptureFrame: sigrokReadCaptureFrame,
  } = useSigrokScope();
  const [streaming, setStreaming] = useState(false);
  const [streamStarting, setStreamStarting] = useState(false);
  const [activeStreamConfig, setActiveStreamConfig] = useState<LogicAnalyzerConfig | null>(null);
  const [activeStreamRate, setActiveStreamRate] = useState<number | null>(null);
  const streamTimeline = streaming || streamSnapshot != null;
  const displayedStreamConfig = streaming ? activeStreamConfig : streamSnapshot?.config;
  const displayedStreamRate = streaming ? activeStreamRate : streamSnapshot?.actualRateHz;

  useEffect(() => { activityCallbackRef.current = onActivityChange; }, [onActivityChange]);

  useEffect(() => {
    const flush = () => {
      if (!streamDirtyRef.current) return;
      streamDirtyRef.current = false;
      setStreamSampleCount(streamTotalRef.current);
      setStreamWaveformVersion((version) => version + 1);
    };
    if (!streaming) {
      flush();
      return;
    }
    const timer = window.setInterval(flush, STREAM_FLUSH_MS);
    return () => window.clearInterval(timer);
  }, [streaming]);

  const updateConfig = useCallback((updater: (current: LogicAnalyzerConfig) => LogicAnalyzerConfig) => {
    setConfig((current) => normalizeLogicAnalyzerLocalConfig(updater(current)));
  }, []);

  const normalizedConfig = useMemo(() => normalizeLogicAnalyzerLocalConfig(config), [config]);

  const configuredRate = normalizedConfig.sampleRateHz;
  const sigrokCapabilities = sigrokGetServerCapabilities();
  const highRatePackedBurst = supportsHighRatePackedBurst(normalizedConfig);
  const selectionMaxSamples = getLogicAnalyzerSelectionMaxSamples(normalizedConfig);
  const maxSamples =
    sigrokCapabilities.hello == null
      ? selectionMaxSamples
      : getLogicAnalyzerMaxSamplesForConfig(normalizedConfig, sigrokCapabilities);
  const unsupportedRateReason = getLogicAnalyzerUnsupportedRateReason(normalizedConfig);
  const streamLimitReason = getLogicAnalyzerStreamLimitReason(normalizedConfig);
  const streamRateExceeded = streamLimitReason != null;
  const boundedCapabilityReason = getLogicAnalyzerNegotiatedCapabilityReason(
    normalizedConfig,
    sigrokCapabilities
  );
  const streamCapabilityReason = getLogicAnalyzerNegotiatedCapabilityReason(normalizedConfig, {
    ...sigrokCapabilities,
    stream: true,
  });
  const totalSamples = normalizedConfig.preSamples + normalizedConfig.postSamples;
  const preTriggerProbe = useMemo(
    () =>
      normalizedConfig.preSamples > 0
        ? normalizedConfig
        : { ...normalizedConfig, preSamples: 1, postSamples: 1 },
    [normalizedConfig]
  );
  const sigrokCaps = sigrokCapabilities.caps;
  const preTriggerGenericReason = useMemo(
    () => getLogicAnalyzerPreTriggerReason(preTriggerProbe, { caps: PRE_TRIGGER_ASSUMED_CAPS }),
    [preTriggerProbe]
  );
  const preTriggerReason = useMemo(
    () =>
      getLogicAnalyzerPreTriggerReason(preTriggerProbe, {
        caps: sigrokCaps ?? PRE_TRIGGER_ASSUMED_CAPS,
      }),
    [preTriggerProbe, sigrokCaps]
  );
  const preTriggerHelperText = useMemo(() => {
    if (preTriggerGenericReason != null) {
      if (preTriggerProbe.triggerType === "none") {
        return t("logicAnalyzer.preTriggerOnlyEdge");
      }
      if (
        preTriggerProbe.sampleRateHz < LOGIC_ANALYZER_MIN_PRE_TRIGGER_SAMPLE_RATE_HZ ||
        preTriggerProbe.sampleRateHz > LOGIC_ANALYZER_MAX_PRE_TRIGGER_SAMPLE_RATE_HZ
      ) {
        return t("logicAnalyzer.preTriggerRateRange");
      }
      if (
        preTriggerProbe.preSamples + preTriggerProbe.postSamples >
        LOGIC_ANALYZER_MAX_PRE_TRIGGER_TOTAL_SAMPLES
      ) {
        return replaceTokens(t("logicAnalyzer.preTriggerTotalMax"), {
          max: LOGIC_ANALYZER_MAX_PRE_TRIGGER_TOTAL_SAMPLES,
        });
      }
      return preTriggerGenericReason;
    }
    if (preTriggerReason != null) {
      const modeId = getSigrokModeForPins(preTriggerProbe.selectedPins);
      return replaceTokens(t("logicAnalyzer.preTriggerMissingCapability"), {
        mode: modeId === SigrokModeId.WIDE11 ? "WIDE11" : "FAST8",
      });
    }
    if (normalizedConfig.preSamples > 0) {
      return replaceTokens(t("logicAnalyzer.preTriggerTotalMax"), {
        max: LOGIC_ANALYZER_MAX_PRE_TRIGGER_TOTAL_SAMPLES,
      });
    }
    return null;
  }, [
    normalizedConfig.preSamples,
    preTriggerGenericReason,
    preTriggerProbe,
    preTriggerReason,
    t,
  ]);
  const preTriggerAvailable = preTriggerReason == null;
  const controlsDisabled =
    state === "armed" || state === "capturing" || isArming || streamStarting || streaming || isStopping;
  const decodeAvailable = state === "done" && capture != null && capture.sampleCount > 0;
  const captureRequestedRate = capture ? getLogicAnalyzerRequestedSampleRate(capture.config) : null;
  const captureActualRate = capture ? getLogicAnalyzerActualSampleRate(capture.config) : null;
  const captureSamplePeriod = capture
    ? formatSamplePeriod(getLogicAnalyzerSamplePeriodPs(capture.config))
    : null;
  const captureBackend = capture ? getLogicAnalyzerBackend(capture.config) : null;
  const capturedPins = useMemo(
    () => capture ? getLogicDecoderCapturePins(capture.config) : null,
    [capture]
  );
  const capturePins = displayedStreamConfig?.selectedPins ?? capturedPins ?? config.selectedPins;
  const capturePinsKey = capturePins.join(",");
  const highRateInfoNotice = useMemo<InlineNotice | null>(() => {
    if (!highRatePackedBurst || unsupportedRateReason != null) {
      return null;
    }

    if (sigrokCapabilities.hello == null) {
      return {
        tone: "brand",
        message: t("logicAnalyzer.highRateConfigV2Required"),
      };
    }

    if (boundedCapabilityReason != null) {
      return {
        tone: "warn",
        message: boundedCapabilityReason,
      };
    }

    if (streamCapabilityReason != null && normalizedConfig.sampleRateHz > 25000000) {
      return {
        tone: "warn",
        message: streamCapabilityReason,
      };
    }

    return {
      tone: "brand",
      message: t("logicAnalyzer.streamHighRateAutoStopHint"),
    };
  }, [
    boundedCapabilityReason,
    highRatePackedBurst,
    normalizedConfig.sampleRateHz,
    sigrokCapabilities.hello,
    streamCapabilityReason,
    t,
    unsupportedRateReason,
  ]);

  useEffect(() => {
    setDecoderConfigs((current) => {
      const next = reconcileLogicDecoderConfigs(current, capturePins);
      return areDecoderConfigsEqual(current, next) ? current : next;
    });
  }, [capturePins, capturePinsKey]);

  const updateDecoderConfigs = useCallback(
    (updater: (current: LogicDecoderProtocolConfigs) => LogicDecoderProtocolConfigs) => {
      setDecoderConfigs((current) => {
        const next = reconcileLogicDecoderConfigs(updater(current), capturePins);
        return areDecoderConfigsEqual(current, next) ? current : next;
      });
    },
    [capturePins, capturePinsKey]
  );

  const ownsOperation = useCallback((operation: CaptureOperation) =>
    captureOperationRef.current === operation && lifecycleRef.current === operation.lifecycle, []);
  const operationIsActive = useCallback((operation: CaptureOperation) =>
    ownsOperation(operation) && !operation.cancelled, [ownsOperation]);

  const beginOperation = useCallback(() => {
    if (!lifecycleRef.current || captureOperationRef.current) return null;
    const operation: CaptureOperation = {
      lifecycle: lifecycleRef.current, cancelled: false, cleanup: null,
    };
    captureOperationRef.current = operation;
    return operation;
  }, []);

  const finishOperation = useCallback((operation: CaptureOperation) => {
    if (captureOperationRef.current !== operation) return;
    captureOperationRef.current = null;
    if (lifecycleRef.current !== operation.lifecycle) return;
    if (operation.cancelled) setState("idle");
    setIsArming(false);
    setStreamStarting(false);
    setIsStopping(false);
    setStreaming(false);
    setActiveStreamConfig(null);
    setActiveStreamRate(null);
    activityCallbackRef.current?.(false);
  }, []);

  const stopAndCloseSigrok = useCallback(
    (operation: CaptureOperation, action: string): Promise<void> => {
      if (operation.cleanup) return operation.cleanup;
      if (captureOperationRef.current !== operation) return Promise.resolve();
      operation.cleanup = (async () => {
        try {
          await sigrokStop();
        } catch (cleanupError) {
          warnSigrokCleanup(`${action} stop`, cleanupError);
        }
        // An obsolete cleanup must never disconnect a later operation.
        if (captureOperationRef.current !== operation) return;
        try {
          sigrokClose();
        } catch (cleanupError) {
          warnSigrokCleanup(`${action} close`, cleanupError);
        }
      })();
      return operation.cleanup;
    },
    [sigrokClose, sigrokStop]
  );

  const clearStreamData = useCallback(() => {
    streamSamplesRef.current = [];
    streamSequenceRef.current = 0;
    streamTotalRef.current = 0;
    streamDirtyRef.current = false;
    setStreamSampleCount(0);
    setStreamSnapshot(null);
    setStreamNotice(null);
    setFrozenStreamWindow(null);
    setLiveAnnotations([]);
    setLiveDecodeError(null);
    setStreamWaveformVersion((version) => version + 1);
  }, []);

  const preserveStreamSnapshot = useCallback((operation: CaptureOperation, stopReason: StreamSnapshot["stopReason"]) => {
    if (!ownsOperation(operation) || !operation.stream || streamSamplesRef.current.length === 0) return;
    // No more frames can be accepted after this operation stops/cancels. Share
    // the capped buffer without copying it; the next operation replaces it.
    setStreamSnapshot({
      source: "stream",
      ...operation.stream,
      samples: streamSamplesRef.current,
      sampleCount: streamTotalRef.current,
      stopReason,
    });
    // STOPPED may arrive before the first 150 ms UI flush (or even before
    // React commits streaming=true), so publish the final data synchronously.
    streamDirtyRef.current = false;
    setStreamSampleCount(streamTotalRef.current);
    setStreamWaveformVersion((version) => version + 1);
    setStreamNotice({
      tone: stopReason === "incomplete" ? "warn" : "brand",
      message: t(stopReason === "auto_buffer_full" ? "logicAnalyzer.streamAutoStopped"
        : stopReason === "manual" ? "logicAnalyzer.streamManuallyStopped" : "logicAnalyzer.streamIncomplete"),
    });
  }, [ownsOperation, t]);

  const handleArm = useCallback(async () => {
    if (captureOperationRef.current != null) return;
    setError(null);
    const nextConfig = normalizedConfig;
    setConfig(nextConfig);
    if (nextConfig.selectedPins.length === 0) { setError("Select at least one pin"); return; }
    const operation = beginOperation();
    if (!operation) return;
    clearStreamData();
    setCapture(null);
    setDecoderState(INITIAL_DECODER_STATE);
    setIsArming(true);
    const hasTrigger = nextConfig.triggerType !== "none";
    try {
      await sigrokEnsureConnected();
      if (!operationIsActive(operation)) return;
      const sigrokRequest = buildSigrokCaptureRequest(nextConfig, sigrokGetServerCapabilities());
      const requestedSampleTarget = sigrokRequest.preSamples + sigrokRequest.postSamples;
      const ack = await sigrokConfigure(sigrokRequest);
      if (!operationIsActive(operation)) return;
      const actualRate = ack.actualRateKhz * 1000;
      await sigrokStart();
      if (!operationIsActive(operation)) return;
      setState(hasTrigger ? "armed" : "capturing");
      activityCallbackRef.current?.(true);

      const dataFrames: Array<{ meta: { sampleIndex: number; sampleCount: number; channelMask: number }; samples: Uint8Array }> = [];
      let receivedSampleCount = 0;
      let triggerSampleIndex: number | null = null;
      let triggerObserved = !hasTrigger;
      let clientStopIssued = false;
      let stopped = false;

      while (!stopped && operationIsActive(operation)) {
        const frame = await sigrokReadCaptureFrame(triggerObserved ? 5000 : 15000);
        if (!operationIsActive(operation)) return;
        if (!frame) {
          throw new Error(triggerObserved ? "Timed out waiting for capture completion" : "Trigger timeout");
        }

        if (isSigrokDataFrame(frame)) {
          dataFrames.push({
            meta: {
              sampleIndex: frame.meta.sampleIndex,
              sampleCount: frame.meta.sampleCount,
              channelMask: frame.meta.channelMask,
            },
            samples: frame.samples,
          });
          receivedSampleCount += frame.meta.sampleCount;
          setState("capturing");
          if (triggerObserved && receivedSampleCount >= requestedSampleTarget) {
            clientStopIssued = true;
            await stopAndCloseSigrok(operation, "bounded capture sample completion cleanup");
            if (!operationIsActive(operation)) return;
            stopped = true;
          }
          continue;
        }

        if (isSigrokErrorFrame(frame)) {
          throw new Error(formatSigrokErrorMessage(frame.payload));
        }

        if (!isSigrokEventFrame(frame)) {
          throw new Error("Unexpected capture frame");
        }

        const eventFrame: SigrokEventFrame = frame;

        switch (eventFrame.payload.typeDetail) {
          case SigrokEventCode.ARMED:
            setState("armed");
            break;
          case SigrokEventCode.TRIGGERED:
            triggerSampleIndex = eventFrame.payload.sampleIndex;
            triggerObserved = true;
            setState("capturing");
            if (receivedSampleCount >= requestedSampleTarget) {
              clientStopIssued = true;
              await stopAndCloseSigrok(operation, "bounded capture trigger completion cleanup");
              if (!operationIsActive(operation)) return;
              stopped = true;
            }
            break;
          case SigrokEventCode.RUNNING:
            setState("capturing");
            break;
          case SigrokEventCode.STOPPED:
            stopped = true;
            break;
          case SigrokEventCode.OVERRUN:
            throw new Error("Capture overrun");
          case SigrokEventCode.ERROR:
            throw new Error("Capture failed");
          default:
            throw new Error(
              `Unexpected capture event ${sigrokEventCodeName(eventFrame.payload.typeDetail)}`
            );
        }
      }

      if (!clientStopIssued) {
        await stopAndCloseSigrok(operation, "capture completion cleanup");
      }

      if (!operationIsActive(operation)) return;

      if (hasTrigger && triggerSampleIndex == null) {
        throw new Error("Capture completed without TRIGGERED event");
      }
      if (receivedSampleCount < requestedSampleTarget) {
        throw new Error("Capture stopped before requested sample count was received");
      }

      const assembledCapture = assembleBoundedSigrokCapture({
        frames: dataFrames,
        channelMask: sigrokRequest.channelMask,
        triggerSampleIndex,
      });

      setCapture(normalizeLogicAnalyzerCapture({
        state: "done",
        config: {
          pinCount: nextConfig.selectedPins.length,
          pinBase: nextConfig.selectedPins[0] ?? 10,
          selectedPins: [...nextConfig.selectedPins],
          requestedSampleRateHz: nextConfig.sampleRateHz,
          actualSampleRateHz: actualRate,
          backend: "sigrok-live-ws",
          triggerType: nextConfig.triggerType,
          triggerPin: nextConfig.triggerType === "none" ? undefined : nextConfig.triggerPin,
        },
        sampleCount: assembledCapture.sampleCount,
        triggerIndex: assembledCapture.triggerIndex,
        samples: assembledCapture.values.map((value, index) => ({
          timestampUs: (index * 1_000_000) / actualRate,
          values: value,
        })),
      }));
      setState("done");
    } catch (err) {
      if (operationIsActive(operation)) {
        setError(err instanceof Error ? err.message : "Network error");
        setState("idle");
      }
    } finally {
      await stopAndCloseSigrok(operation, "capture completion cleanup");
      finishOperation(operation);
    }
  }, [beginOperation, clearStreamData, finishOperation, normalizedConfig, operationIsActive, sigrokConfigure, sigrokEnsureConnected, sigrokGetServerCapabilities, sigrokReadCaptureFrame, sigrokStart, stopAndCloseSigrok]);

  const handleCancel = useCallback(async () => {
    const operation = captureOperationRef.current;
    if (!operation || !operationIsActive(operation)) return;
    operation.cancelled = true;
    setError(null);
    setStreamNotice(null);
    preserveStreamSnapshot(operation, "manual");
    setIsStopping(true);
    setStreaming(false);
    await stopAndCloseSigrok(operation, "manual cancel");
    finishOperation(operation);
  }, [finishOperation, operationIsActive, preserveStreamSnapshot, stopAndCloseSigrok]);

  const stopStream = handleCancel;

  const startStream = useCallback(async () => {
    if (
      captureOperationRef.current != null ||
      state !== "idle"
    ) return;
    setError(null);
    const cfg = normalizedConfig;
    if (cfg.selectedPins.length === 0) {
      setError("Select at least one pin");
      return;
    }
    const operation = beginOperation();
    if (!operation) return;
    clearStreamData();
    setCapture(null);
    setDecoderState(INITIAL_DECODER_STATE);
    setStreamStarting(true);
    try {
      await sigrokEnsureConnected();
      if (!operationIsActive(operation)) return;
      const sigrokRequest = buildSigrokCaptureRequest(cfg, { ...sigrokGetServerCapabilities(), stream: true });
      const ack = await sigrokConfigure(sigrokRequest);
      if (!operationIsActive(operation)) return;
      await sigrokStart();
      if (!operationIsActive(operation)) return;
      operation.stream = {
        config: { ...cfg, selectedPins: [...cfg.selectedPins] },
        actualRateHz: ack.actualRateKhz * 1000,
      };
      setActiveStreamConfig(operation.stream.config);
      setActiveStreamRate(operation.stream.actualRateHz);
      setStreamStarting(false);
      setStreaming(true);
      activityCallbackRef.current?.(true);
      let stopped = false;
      while (!stopped && operationIsActive(operation)) {
        const frame = await sigrokReadCaptureFrame(8000);
        if (!operationIsActive(operation)) return;
        if (!frame) {
          throw new Error("Timed out waiting for stream data");
        }

        if (isSigrokDataFrame(frame)) {
          const values = unpackSigrokSamples(frame.samples, sigrokRequest.channelMask);
          const arr = streamSamplesRef.current;
          for (const value of values) arr.push(value);
          if (arr.length > STREAM_BUFFER_CAP) arr.splice(0, arr.length - STREAM_BUFFER_CAP);
          streamSequenceRef.current += 1;
          streamTotalRef.current += values.length;
          streamDirtyRef.current = true;
          continue;
        }

        if (isSigrokErrorFrame(frame)) {
          throw new Error(formatSigrokErrorMessage(frame.payload));
        }

        if (!isSigrokEventFrame(frame)) {
          throw new Error("Unexpected stream frame");
        }

        switch (frame.payload.typeDetail) {
          case SigrokEventCode.ARMED:
          case SigrokEventCode.TRIGGERED:
          case SigrokEventCode.RUNNING:
            break;
          case SigrokEventCode.STOPPED: {
            const stopReason = classifyLogicAnalyzerStreamStop({
              config: cfg,
              sampleCount: streamTotalRef.current,
              userInitiated: false,
            });
            if (stopReason === "auto_buffer_full") {
              preserveStreamSnapshot(operation, "auto_buffer_full");
            } else if (stopReason === "unexpected_stop") {
              throw new Error(t("logicAnalyzer.streamUnexpectedStop"));
            }
            stopped = true;
            break;
          }
          case SigrokEventCode.OVERRUN:
            throw new Error("Capture overrun");
          case SigrokEventCode.ERROR:
            throw new Error("Capture failed");
          default:
            throw new Error(
              `Unexpected stream event ${sigrokEventCodeName(frame.payload.typeDetail)}`
            );
        }
      }
    } catch (err) {
      if (operationIsActive(operation)) {
        preserveStreamSnapshot(operation, "incomplete");
        setError(err instanceof Error ? err.message : "Network error");
      }
    } finally {
      if (ownsOperation(operation)) {
        setIsStopping(true);
        setStreaming(false);
      }
      await stopAndCloseSigrok(operation, "stream completion cleanup");
      finishOperation(operation);
    }
  }, [
    normalizedConfig,
    beginOperation,
    clearStreamData,
    finishOperation,
    operationIsActive,
    ownsOperation,
    preserveStreamSnapshot,
    sigrokConfigure,
    sigrokEnsureConnected,
    sigrokGetServerCapabilities,
    sigrokReadCaptureFrame,
    sigrokStart,
    state,
    stopAndCloseSigrok,
    t,
  ]);
  useEffect(() => {
    // StrictMode may replay this lifecycle, but a cancelled operation's identity
    // is never reused or reactivated by the next setup.
    const lifecycle = {};
    lifecycleRef.current = lifecycle;
    return () => {
      if (lifecycleRef.current !== lifecycle) return;
      lifecycleRef.current = null;
      streamSamplesRef.current = [];
      streamTotalRef.current = 0;
      streamDirtyRef.current = false;
      activityCallbackRef.current?.(false);
      const operation = captureOperationRef.current;
      if (operation != null) {
        operation.cancelled = true;
        void stopAndCloseSigrok(operation, "component cleanup").then(() => finishOperation(operation));
        return;
      }
      try {
        sigrokClose();
      } catch (cleanupError) {
        warnSigrokCleanup("component cleanup close", cleanupError);
      }
    };
  }, [finishOperation, sigrokClose, stopAndCloseSigrok]);

  const handleClear = useCallback(() => {
    if (captureOperationRef.current) return;
    clearStreamData();
    setCapture(null);
    setState("idle");
    setError(null);
    setDecoderState(INITIAL_DECODER_STATE);
  }, [clearStreamData]);

  // The trigger is stored as an index into the normalized, sorted pin list,
  // so these handlers resolve pin identity instead of trusting raw indices.
  const handleTogglePin = useCallback(
    (pin: number) => {
      if (controlsDisabled) return;
      updateConfig((current) => {
        const triggerActive = current.triggerType !== "none";
        const triggerPinNumber = triggerActive ? current.selectedPins[current.triggerPin] ?? null : null;
        const removing = current.selectedPins.includes(pin);
        const selectedPins = removing
          ? current.selectedPins.filter((selectedPin) => selectedPin !== pin)
          : [...current.selectedPins, pin].sort((left, right) => left - right);
        // Dropping the trigger pin, or the last selected pin, invalidates the
        // trigger entirely; it never silently moves to another pin.
        if (removing && (selectedPins.length === 0 || pin === triggerPinNumber)) {
          return { ...current, selectedPins, triggerPin: 0, triggerType: "none" };
        }
        const triggerPin =
          triggerPinNumber == null ? 0 : Math.max(0, selectedPins.indexOf(triggerPinNumber));
        return { ...current, selectedPins, triggerPin };
      });
    },
    [controlsDisabled, updateConfig]
  );

  const handleSetTriggerPin = useCallback(
    (pin: number | null) => {
      if (controlsDisabled) return;
      if (pin == null) {
        updateConfig((current) => ({ ...current, triggerType: "none" }));
        return;
      }
      updateConfig((current) => {
        const selectedPins = current.selectedPins.includes(pin)
          ? [...current.selectedPins]
          : [...current.selectedPins, pin].sort((left, right) => left - right);
        return {
          ...current,
          selectedPins,
          triggerPin: selectedPins.indexOf(pin),
          triggerType: current.triggerType === "none" ? "either" : current.triggerType,
        };
      });
    },
    [controlsDisabled, updateConfig]
  );

  const waveformData = useMemo(() => {
    if (!capture || capture.sampleCount === 0) return [];

    return Array.from({ length: capturePins.length }, (_, channelIndex) => {
      const channelPoints: WaveformPoint[] = [];

      for (let sampleIndex = 0; sampleIndex < capture.sampleCount; sampleIndex += 1) {
        const sample = capture.samples[sampleIndex];
        if (!sample) continue;

        const bit = (sample.values >> channelIndex) & 1;
        channelPoints.push({
          x: waveformX(sampleIndex, capture.sampleCount),
          y: bit ? CHANNEL_GAP : CHANNEL_HEIGHT - CHANNEL_GAP,
        });
      }

      const last = channelPoints.at(-1);
      if (last) channelPoints.push({ x: waveformX(capture.sampleCount, capture.sampleCount), y: last.y });
      return channelPoints;
    });
  }, [capture, capturePins]);

  const streamWindow = useMemo(() => {
    const samples = streamSnapshot?.samples ?? streamSamplesRef.current;
    const total = streamSnapshot?.sampleCount ?? streamTotalRef.current;
    return { samples, ...resolveStreamWindow(total, samples.length, streamSpan, frozenStreamWindow) };
    // streamSamplesRef content is versioned through streamWaveformVersion
  }, [streamWaveformVersion, streamSpan, frozenStreamWindow, streamSnapshot]);

  const streamWaveformData = useMemo(() => {
    if (!streamTimeline) return [];
    const pins = displayedStreamConfig?.selectedPins ?? [];
    if (pins.length === 0 || streamWindow.len === 0) return [];
    const { samples, start, end } = streamWindow;
    return pins.map((_, channelIndex) => activityWaveformPoints({
      samples, channelIndex, start, end, left: PLOT_LEFT, width: WIDTH - PLOT_LEFT - PLOT_RIGHT,
      high: CHANNEL_GAP, low: CHANNEL_HEIGHT - CHANNEL_GAP,
    }));
  }, [streamTimeline, displayedStreamConfig, streamWindow]);

  const streamWindowRef = useRef(streamWindow);
  useEffect(() => {
    // Keep the decode scheduler on the latest window without mutating a ref
    // during render. This effect must stay before the decode effect below so
    // its immediate decode run observes the current stream window.
    streamWindowRef.current = streamWindow;
  }, [streamWindow]);
  const streamMaxStart = Math.max(0, streamWindow.len - streamWindow.span);

  const liveDecodePreparation = useMemo(() => {
    if (!streaming || !activeStreamConfig || activeStreamRate == null) {
      return { request: null, error: null as string | null };
    }
    // Validate protocol/pin mapping even before the first data frame arrives.
    // The scheduler replaces this one-sample template with its bounded window.
    try {
      return {
        request: buildLogicDecoderRequest({
          state: "done",
          config: {
            pinCount: activeStreamConfig.selectedPins.length,
            pinBase: activeStreamConfig.selectedPins[0] ?? 0,
            selectedPins: activeStreamConfig.selectedPins,
            actualSampleRateHz: activeStreamRate,
          },
          sampleCount: 1,
          triggerIndex: 0,
          samples: [{ timestampUs: 0, values: 0 }],
        }, decoderProtocol, decoderConfigs),
        error: null as string | null,
      };
    } catch (decodeError) {
      return {
        request: null,
        error: decodeError instanceof Error ? decodeError.message : String(decodeError),
      };
    }
  }, [streaming, activeStreamConfig, activeStreamRate, decoderProtocol, decoderConfigs]);

  useEffect(() => {
    setLiveAnnotations([]);
    setLiveDecodeError(null);
    const template = liveDecodePreparation.request;
    if (!template) return;
    const sampleMask = (1 << (activeStreamConfig?.selectedPins.length ?? 0)) - 1;
    let cancelled = false;
    let busy = false;
    const decodeWindow = async () => {
      if (busy) return;
      const { samples, start, end, firstSampleIndex } = streamWindowRef.current;
      const decodeLen = Math.min(end - start, STREAM_DECODE_MAX_SAMPLES);
      const decodeBase = firstSampleIndex + end - decodeLen;
      if (decodeLen <= 0) return;
      // The backing ring may advance between UI flushes. Read the same absolute
      // window, never silently relabel newer data with a frozen window's time.
      const currentFirst = streamTotalRef.current - samples.length;
      const decodeStart = decodeBase - currentFirst;
      if (decodeStart < 0) return;
      busy = true;
      try {
        const result = await decodeLogicCapture({
          ...template,
          samples: {
            ...template.samples,
            sampleCount: decodeLen,
            words: Array.from({ length: decodeLen }, (_, index) =>
              samples[decodeStart + index] & sampleMask),
          },
        });
        if (!cancelled) {
          setLiveDecodeError(null);
          setLiveAnnotations(result.annotations.map((annotation) => ({
            ...annotation,
            startSample: decodeBase + annotation.startSample,
            endSample: decodeBase + annotation.endSample,
          })));
        }
      } catch (decodeError) {
        if (!cancelled) {
          setLiveDecodeError(decodeError instanceof Error ? decodeError.message : String(decodeError));
          setLiveAnnotations([]);
        }
      } finally {
        busy = false;
      }
    };
    const timer = window.setInterval(() => { void decodeWindow(); }, STREAM_DECODE_MS);
    void decodeWindow();
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, [liveDecodePreparation, activeStreamConfig]);

  const uartConfig: LogicDecoderProtocolConfigs["uart"] = decoderConfigs.uart;
  const i2cConfig: LogicDecoderProtocolConfigs["i2c"] = decoderConfigs.i2c;
  const spiConfig: LogicDecoderProtocolConfigs["spi"] = decoderConfigs.spi;
  const decodePreparation = useMemo(() => {
    if (!capture || state !== "done") {
      return { request: null, requestSignature: null, error: null as string | null };
    }

    try {
      const request = buildLogicDecoderRequest(capture, decoderProtocol, decoderConfigs);
      return {
        request,
        requestSignature: JSON.stringify(request),
        error: null as string | null,
      };
    } catch (decodeError) {
      return {
        request: null,
        requestSignature: null,
        error: decodeError instanceof Error ? decodeError.message : String(decodeError),
      };
    }
  }, [capture, decoderConfigs, decoderProtocol, state]);

  useEffect(() => {
    // Configuration/capture changes cancel this UI job immediately, including
    // loading. A generation also prevents an old promise completing a newer job
    // when the user returns to an identical request.
    boundedDecodeRunRef.current += 1;
    setDecoderState((current) => current.status === "loading" ? INITIAL_DECODER_STATE : current);
    return () => { boundedDecodeRunRef.current += 1; };
  }, [decodePreparation]);

  const decodeIsStale =
    decoderState.lastRequestSignature != null &&
    decoderState.lastRequestSignature !== decodePreparation.requestSignature;
  const decodeResult = decodeIsStale ? null : decoderState.result;
  const decodeError = decodeIsStale ? null : decoderState.error;
  const waveformPins = streamTimeline ? displayedStreamConfig?.selectedPins ?? [] : capturePins;
  const waveformSampleStart = streamTimeline ? streamWindow.firstSampleIndex + streamWindow.start : 0;
  const waveformSampleCount = streamTimeline ? streamWindow.span : capture?.sampleCount ?? 0;
  const annotationLayout = useMemo(
    () => layoutLogicDecoderAnnotations(streamTimeline
      ? streaming ? liveAnnotations.filter((annotation) => annotation.endSample > waveformSampleStart &&
          annotation.startSample < waveformSampleStart + waveformSampleCount) : []
      : decodeResult?.annotations ?? []),
    [streamTimeline, streaming, liveAnnotations, waveformSampleStart, waveformSampleCount, decodeResult]
  );
  const decodeStatusKey = decodeIsStale
    ? "logicAnalyzer.decoder.status.stale"
    : decoderState.status === "loading"
      ? "logicAnalyzer.decoder.status.loading"
      : decodeError
        ? "logicAnalyzer.decoder.status.error"
        : decodeResult
          ? "logicAnalyzer.decoder.status.ready"
          : "logicAnalyzer.decoder.status.idle";
  const decodeStatusTone = decodeIsStale
    ? "warn"
    : decoderState.status === "loading"
      ? "brand"
      : decodeError
        ? "danger"
        : decodeResult
          ? "ok"
          : "neutral";
  const localizedDecodePreparationError =
    decodePreparation.error == null ? null : localizeLogicDecoderErrorMessage(decodePreparation.error, t);
  const localizedDecodeError =
    decodeError == null ? null : localizeLogicDecoderErrorMessage(decodeError, t);
  const liveDecodeMessage = liveDecodePreparation.error ?? liveDecodeError;
  const localizedLiveDecodeError = liveDecodeMessage == null
    ? null : localizeLogicDecoderErrorMessage(liveDecodeMessage, t);
  const decoderSignalMap = useMemo(
    () => decoderSignalPins(decoderProtocol, decoderConfigs),
    [decoderConfigs, decoderProtocol]
  );
  const annotationAreaHeight =
    annotationLayout.laneCount > 0
      ? annotationLayout.laneCount * ANNOTATION_HEIGHT +
        Math.max(0, annotationLayout.laneCount - 1) * ANNOTATION_GAP +
        ANNOTATION_MARGIN
      : 0;
  const channelTop = TOP_PADDING + annotationAreaHeight;

  const triggerLineX = useMemo(() => {
    if (streamTimeline || !capture || capture.sampleCount <= 0) return null;
    return waveformX(capture.triggerIndex, capture.sampleCount);
  }, [capture, streamTimeline]);

  const svgHeight = useMemo(() => {
    const pinCount = waveformPins.length || 1;
    return Math.max(
      140,
      channelTop +
        pinCount * CHANNEL_HEIGHT +
        Math.max(0, pinCount - 1) * CHANNEL_GAP +
        BOTTOM_PADDING
    );
  }, [waveformPins.length, channelTop]);

  const sampleRateForAnnotations = captureActualRate ?? 1;
  const waveformRate = streamTimeline ? displayedStreamRate : captureActualRate;
  const diagnosticCounts = useMemo(() => {
    return (decodeResult?.diagnostics ?? []).reduce(
      (counts, diagnostic) => {
        counts[diagnostic.severity] += 1;
        return counts;
      },
      { info: 0, warning: 0, error: 0 }
    );
  }, [decodeResult]);

  const runDecode = useCallback(async () => {
    if (!decodePreparation.request || !decodePreparation.requestSignature) {
      setDecoderState({
        status: "done",
        result: null,
        error: decodePreparation.error ?? "Invalid decoder configuration",
        lastRequestSignature: null,
      });
      return;
    }

    const requestSignature = decodePreparation.requestSignature;
    const runId = ++boundedDecodeRunRef.current;
    setDecoderState({
      status: "loading",
      result: null,
      error: null,
      lastRequestSignature: requestSignature,
    });

    try {
      const result = await decodeLogicCapture(decodePreparation.request);
      if (boundedDecodeRunRef.current !== runId) {
        return;
      }
      setDecoderState({
        status: "done",
        result,
        error: null,
        lastRequestSignature: requestSignature,
      });
    } catch (decodeRunError) {
      if (boundedDecodeRunRef.current !== runId) {
        return;
      }
      setDecoderState({
        status: "done",
        result: null,
        error:
          decodeRunError instanceof Error ? decodeRunError.message : String(decodeRunError),
        lastRequestSignature: requestSignature,
      });
    }
  }, [decodePreparation]);

  const renderChannelSelect = useCallback(
    (
      label: string,
      signal: LogicDecoderSignalName,
      value: number | null,
      options: readonly number[],
      allowNone: boolean
    ) => {
      return (
        <label className="text-[11px] text-ink-dim">
          {label}
          <select
            value={value ?? ""}
            onChange={(event) => {
              const nextValue = event.target.value === "" ? null : Number(event.target.value);
              setDecoderConfigs((current) =>
                updateLogicDecoderSignalPin(current, decoderProtocol, signal, nextValue, capturePins)
              );
            }}
            className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-2 text-xs text-ink"
          >
            {allowNone && <option value="">{t("logicAnalyzer.decoder.none")}</option>}
            {options.map((pin) => {
              const disabled = Object.entries(decoderSignalMap).some(
                ([otherSignal, otherPin]) => otherSignal !== signal && otherPin === pin
              );
              return (
                <option key={pin} value={pin} disabled={disabled}>
                  {pinLabel(pin)}
                </option>
              );
            })}
          </select>
        </label>
      );
    },
    [capturePins, decoderProtocol, decoderSignalMap]
  );

  return (
    <Card
      headerLeading={workspaceTabs}
      contentClassName="p-0"
    >
      <header className="flex flex-wrap items-center justify-between gap-3 border-b border-line/60 px-3 py-3 sm:px-4">
        <div className="flex min-w-0 items-center gap-3">
          <span className="grid h-9 w-9 shrink-0 place-items-center rounded-xl bg-brand/10 text-brand"><Activity size={17} /></span>
          <div className="min-w-0">
            <h2 className="text-sm font-semibold tracking-[-0.01em] text-ink">{t("logicAnalyzer.title")}</h2>
            <p className="mt-0.5 text-[11px] leading-4 text-ink-dim">{t("logicAnalyzer.subtitle")}</p>
          </div>
        </div>
          <Badge tone={state === "idle" ? "neutral" : state === "armed" ? "warn" : "brand"}>
            {t(`logicAnalyzer.state.${state}`)}
          </Badge>
      </header>

      <div className="p-3 sm:p-4">

      {error && (
        <div className="mb-3 rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
          {error}
        </div>
      )}

      <div
        data-testid="logic-analyzer-controls"
        className="grid items-start gap-3 lg:grid-cols-[minmax(0,320px)_minmax(0,1fr)]"
      >
        <section data-testid="logic-analyzer-sampling-settings" className="min-w-0 rounded-xl border border-line/60 bg-panel2/20 p-3">
          <div className="grid grid-cols-2 gap-x-3 gap-y-2">
            <label className="text-[11px] text-ink-dim">
              {t("logicAnalyzer.sampleRate")}
              <select
                value={config.sampleRateHz}
                onChange={(event) =>
                  updateConfig((current) => ({ ...current, sampleRateHz: Number(event.target.value) }))
                }
                disabled={controlsDisabled}
                className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-1.5 text-xs text-ink"
              >
                {SAMPLE_RATES.map((rate) => (
                  <option key={rate.value} value={rate.value}>{rate.label}</option>
                ))}
              </select>
            </label>

            <label className="text-[11px] text-ink-dim">
              {t("logicAnalyzer.triggerType")}
              <select
                value={config.triggerType}
                onChange={(event) =>
                  updateConfig((current) => ({ ...current, triggerType: event.target.value as LogicAnalyzerTriggerType }))
                }
                disabled={controlsDisabled}
                className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-1.5 text-xs text-ink"
              >
                <option value="none">{t("logicAnalyzer.trigger.none")}</option>
                <option value="rising">{t("logicAnalyzer.trigger.rising")}</option>
                <option value="falling">{t("logicAnalyzer.trigger.falling")}</option>
                <option value="either">{t("logicAnalyzer.trigger.either")}</option>
              </select>
            </label>

            <label className="text-[11px] text-ink-dim">
              {t("logicAnalyzer.preSamples")}
              <input
                type="number"
                min="0"
                max={LOGIC_ANALYZER_MAX_PRE_TRIGGER_SAMPLES}
                step="1"
                value={config.preSamples}
                onChange={(e) => updateConfig((c) => ({ ...c, preSamples: Number(e.target.value) }))}
                disabled={controlsDisabled || !preTriggerAvailable}
                className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-1.5 text-xs text-ink"
              />
            </label>

            <label className="text-[11px] text-ink-dim">
              {t("logicAnalyzer.postSamples")}
              <input
                type="number"
                min="1"
                max={
                  normalizedConfig.preSamples > 0
                    ? LOGIC_ANALYZER_MAX_PRE_TRIGGER_TOTAL_SAMPLES - normalizedConfig.preSamples
                    : undefined
                }
                step="1"
                value={config.postSamples}
                onChange={(e) => updateConfig((c) => ({ ...c, postSamples: Number(e.target.value) }))}
                disabled={controlsDisabled}
                className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-1.5 text-xs text-ink"
              />
            </label>
            {preTriggerHelperText != null && (
              <span className="col-span-2 -mt-1 block text-[9px] text-ink-dim/60">
                {preTriggerHelperText}
              </span>
            )}
            {config.triggerType !== "none" && (
              <label className="col-span-2 text-[11px] text-ink-dim">
                {t("logicAnalyzer.triggerPin")}
                <select
                  value={config.triggerPin}
                  onChange={(event) =>
                    updateConfig((current) => ({ ...current, triggerPin: Number(event.target.value) }))
                  }
                  disabled={controlsDisabled}
                  className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-1.5 text-xs text-ink"
                >
                  {config.selectedPins.map((pin, index) => (
                    <option key={pin} value={index}>GP{pin}</option>
                  ))}
                </select>
              </label>
            )}
          </div>

          <div className="mt-3 flex flex-wrap items-center gap-2 border-t border-line/50 pt-3">
            <span className="mr-1 text-[10px] font-medium uppercase tracking-wide text-ink-dim">
              {t("logicAnalyzer.captureActions")}
            </span>
            {streaming ? (
              <Button type="button" onClick={() => { void stopStream(); }}>
                <Square size={15} />
                {t("logicAnalyzer.stopStream")}
              </Button>
            ) : state === "idle" ? (
              <>
                <Button
                  type="button"
                  variant="primary"
                  onClick={handleArm}
                  disabled={controlsDisabled || config.selectedPins.length === 0}
                >
                  {isArming ? <Loader2 size={15} className="animate-spin" /> : <Radio size={15} />}
                  {isArming ? t("logicAnalyzer.arming") : t("logicAnalyzer.arm")}
                </Button>
                <Button
                  type="button"
                  variant="ghost"
                  title={streamRateExceeded ? t("logicAnalyzer.streamRateLimit") : undefined}
                  onClick={() => { void startStream(); }}
                  disabled={controlsDisabled || config.selectedPins.length === 0 || streamRateExceeded}
                >
                  {streamStarting ? <Loader2 size={15} className="animate-spin" /> : <Zap size={15} />}
                  {t("logicAnalyzer.startStream")}
                </Button>
                {(isArming || streamStarting || isStopping) && (
                  <Button type="button" onClick={handleCancel} disabled={isStopping}>
                    <Square size={15} />
                    {t("logicAnalyzer.cancel")}
                  </Button>
                )}
                {streamRateExceeded && (
                  <span className="text-[10px] text-ink-dim">
                    {t("logicAnalyzer.streamRateLimit")}
                  </span>
                )}
              </>
            ) : state === "armed" || state === "capturing" || isArming ? (
              <Button type="button" onClick={handleCancel} disabled={isStopping}>
                <Square size={15} />
                {t("logicAnalyzer.cancel")}
              </Button>
            ) : (
              <Button
                type="button"
                variant="primary"
                onClick={() => {
                  handleClear();
                  void handleArm();
                }}
              >
                <Radio size={15} />
                {t("logicAnalyzer.reCapture")}
              </Button>
            )}

            {capture && state === "done" && (
              <div className="ml-auto flex flex-wrap items-center gap-1 border-l border-line/50 pl-2">
                <Button type="button" variant="ghost" onClick={() => exportToCsv(capture)}>
                  <Download size={13} />
                  CSV
                </Button>
                <Button type="button" variant="ghost" onClick={() => exportToSr(capture)}>
                  <Download size={13} />
                  PulseView (.sr)
                </Button>
                <Button type="button" variant="ghost" onClick={handleClear} aria-label={t("logicAnalyzer.clear")}>
                  <Trash2 size={13} />
                </Button>
              </div>
            )}
            {streamSnapshot && (
              <Button type="button" variant="ghost" onClick={handleClear}
                disabled={controlsDisabled} aria-label={t("logicAnalyzer.clear")}>
                <Trash2 size={13} />
              </Button>
            )}
          </div>

          {highRateInfoNotice && <p role="status" data-testid="logic-analyzer-high-rate-notice"
            className={`mt-3 rounded-lg border px-3 py-2 text-[11px] leading-relaxed ${highRateInfoNotice.tone === "warn"
              ? "border-warn/30 bg-warn/10 text-warn" : "border-brand/20 bg-brand/5 text-ink-dim"}`}>
            {highRateInfoNotice.message}
          </p>}

        </section>

        <section data-testid="logic-analyzer-pin-select" className="min-w-0 rounded-xl border border-line/60 bg-panel2/35 p-3">
          <div className="flex items-center justify-between gap-3">
            <div className="text-xs font-semibold text-ink">{t("logicAnalyzer.selectPins")}</div>
            <Badge tone={config.selectedPins.length > 0 ? "brand" : "neutral"}>
              {config.selectedPins.length}/{AVAILABLE_PINS.length}
            </Badge>
          </div>
          <LogicAnalyzerPinSelector
            physicalLayout={physicalLayout ?? null}
            boardGpios={boardGpios ?? []}
            selectedPins={config.selectedPins}
            triggerPin={
              config.triggerType !== "none" && config.selectedPins.length > 0
                ? config.selectedPins[config.triggerPin] ?? null
                : null
            }
            triggerActive={config.triggerType !== "none"}
            disabled={controlsDisabled}
            onTogglePin={handleTogglePin}
            onSetTriggerPin={handleSetTriggerPin}
          />
        </section>
      </div>

      <section data-testid="logic-analyzer-waveform" aria-label={t("logicAnalyzer.waveform")}
        data-stream-stop-reason={streamSnapshot?.stopReason}
        className="mt-3 min-w-0 rounded-xl border border-line/60 bg-panel2/20 p-3">
        <div className="mb-3 flex flex-wrap items-center gap-3 text-[10px] text-ink-dim">
          <h3 className="mr-auto inline-flex items-center gap-2 text-xs font-semibold text-ink">
            <Activity size={14} className="text-brand" />
            {t(streamSnapshot ? "logicAnalyzer.streamSnapshot" : streaming ? "logicAnalyzer.streamWaveform" : "logicAnalyzer.waveform")}
          </h3>
          {waveformRate != null && <span data-testid="logic-analyzer-waveform-rate">
            {t("logicAnalyzer.actualRate")}: {formatSampleRate(waveformRate)}
          </span>}
          {streamTimeline && <>
            <Badge tone={streamSnapshot?.stopReason === "incomplete" ? "warn" : streaming ? "brand" : "neutral"}>
              {t(streaming ? "logicAnalyzer.streaming" : streamSnapshot?.stopReason === "incomplete"
                ? "logicAnalyzer.streamSnapshotIncomplete" : "logicAnalyzer.streamSnapshotReadOnly")}
            </Badge>
            {streamSnapshot && <span data-testid="logic-analyzer-stream-requested-rate">
              {t("logicAnalyzer.requestedRate")}: {formatSampleRate(streamSnapshot.config.sampleRateHz)}
            </span>}
            <span>{formatSampleCount(streamSnapshot?.sampleCount ?? streamSampleCount)} {t("logicAnalyzer.samples")}</span>
            {streamSnapshot && streamSnapshot.samples.length < streamSnapshot.sampleCount && <span>
              {replaceTokens(t("logicAnalyzer.streamRetainedSamples"), { count: formatSampleCount(streamSnapshot.samples.length) })}
            </span>}
            <label className="inline-flex items-center gap-1">
              <input type="checkbox" checked={streamFollow} disabled={streamWindow.len === 0} onChange={(event) => {
                setFrozenStreamWindow(event.target.checked ? null : {
                  start: streamWindow.firstSampleIndex + streamWindow.start,
                  end: streamWindow.firstSampleIndex + streamWindow.end,
                });
              }} />
              {t("logicAnalyzer.streamFollow")}
            </label>
            <select value={streamSpan} onChange={(event) => {
              const span = Number(event.target.value);
              setStreamSpan(span);
              if (!streamFollow) {
                const end = streamWindow.firstSampleIndex + streamWindow.end;
                setFrozenStreamWindow({ start: Math.max(streamWindow.firstSampleIndex, end - span), end });
              }
            }} className="rounded border border-line bg-panel px-1 py-0.5 text-[10px] text-ink"
              aria-label={t("logicAnalyzer.streamWindow")}>
              {STREAM_SPAN_OPTIONS.map((spanOption) => <option key={spanOption} value={spanOption}>{formatSampleCount(spanOption)}</option>)}
            </select>
            {!streamFollow && streamMaxStart > 0 && <input type="range" min={0} max={streamMaxStart}
              value={streamWindow.start}
              onChange={(event) => {
                const start = streamWindow.firstSampleIndex + Number(event.target.value);
                setFrozenStreamWindow({ start, end: start + streamWindow.span });
              }}
              className="min-w-24 flex-1" aria-label={t("logicAnalyzer.streamWindow")} />}
          </>}
        </div>
        {streamNotice && <p role="status" data-testid="logic-analyzer-stream-notice"
          className={`mb-3 rounded-lg border px-3 py-2 text-xs leading-relaxed ${streamNotice.tone === "warn"
            ? "border-warn/30 bg-warn/10 text-warn" : "border-brand/20 bg-brand/5 text-ink-dim"}`}>
          {streamNotice.message}
        </p>}
        {streamTimeline && streamWindow.historyEvicted && <p role="status" data-testid="logic-analyzer-history-evicted"
          className="mb-3 rounded-lg border border-warn/30 bg-warn/10 px-3 py-2 text-xs text-warn">
          {t("logicAnalyzer.streamHistoryEvicted")}
        </p>}
        {(streamTimeline ? streamWaveformData : waveformData).length > 0 ? <>
          <svg data-testid="logic-analyzer-waveform-plot" viewBox={`0 0 ${WIDTH} ${svgHeight}`}
            data-sample-start={waveformSampleStart} data-sample-end={waveformSampleStart + waveformSampleCount}
            className="block w-full rounded-lg border border-line/60 bg-panel"
            style={{ height: `${svgHeight}px` }} preserveAspectRatio="none" role="img"
            aria-label={t(streamSnapshot ? "logicAnalyzer.streamSnapshot" : streaming
              ? "logicAnalyzer.streamWaveform" : "logicAnalyzer.decoder.waveformAria")}>
            {annotationLayout.annotations.map((annotation, index) => {
              const start = Math.max(0, annotation.startSample - waveformSampleStart);
              const end = Math.min(waveformSampleCount, annotation.endSample - waveformSampleStart);
              const x1 = waveformX(start, waveformSampleCount);
              const width = Math.max(0.5, waveformX(end, waveformSampleCount) - x1);
              const y = TOP_PADDING + annotation.lane * (ANNOTATION_HEIGHT + ANNOTATION_GAP);
              const palette = annotationPalette(annotation.class);
              const labelFits = width >= annotation.shortText.length * 5.5 + 10;
              return <g key={`${annotation.row}-${annotation.startSample}-${annotation.endSample}-${index}`} data-logic-annotation="">
                <rect x={x1} y={y} width={width} height={ANNOTATION_HEIGHT} rx={4} ry={4}
                  fill={palette.fill} stroke={palette.stroke} />
                <title>{annotation.longText}</title>
                {labelFits && <text x={x1 + 4} y={y + 12} style={{ fill: palette.text }}
                  className="text-[9px] font-medium">{annotation.shortText}</text>}
              </g>;
            })}
            <StepWaveform channels={streamTimeline ? streamWaveformData : waveformData}
              pinKeys={waveformPins} channelTop={channelTop} triggerLineX={triggerLineX} svgHeight={svgHeight} />
          </svg>
          <div className="mt-1 flex justify-between font-mono text-[9px] text-ink-dim">
            <span>{formatDuration(waveformSampleStart, waveformRate ?? 1)}</span>
            {!streamTimeline && <span>{t("logicAnalyzer.triggerPoint")}</span>}
            <span>{formatDuration(waveformSampleStart + waveformSampleCount, waveformRate ?? 1)}</span>
          </div>
        </> : <p className="rounded-lg border border-dashed border-line/60 bg-panel px-3 py-8 text-center text-xs text-ink-dim">
          {t(streaming ? "logicAnalyzer.waveformWaiting" : "logicAnalyzer.waveformEmpty")}
        </p>}
      </section>

      <div data-testid="logic-analyzer-config-preview" className="mt-2 flex flex-wrap gap-4 text-[10px] text-ink-dim">
        <span>
          {t("logicAnalyzer.requestedRate")}: {formatSampleRate(configuredRate)}
        </span>
        <span>
          {t("logicAnalyzer.totalSamples")}: {formatSampleCount(totalSamples)}
        </span>
        <span>
          {t("logicAnalyzer.duration")}: {formatDuration(totalSamples, configuredRate)}
        </span>
        <span>
          {t("logicAnalyzer.maxSamples")}: {formatSampleCount(maxSamples)}
          <span className="ml-1 text-ink-dim/70">
            ({formatDuration(maxSamples, configuredRate)})
          </span>
        </span>
      </div>
      <details className="mt-2 rounded-xl border border-line/60 bg-panel2/25 px-3 py-2">
        <summary className="cursor-pointer text-[10px] font-medium text-ink-dim outline-none focus-visible:ring-2 focus-visible:ring-brand/30">
          {t("logicAnalyzer.capabilityDetails")}
        </summary>
        <div className="mt-2 space-y-2 border-t border-line/50 pt-2 text-[9px] leading-relaxed text-ink-dim/80">
          <p>{t("logicAnalyzer.captureSemantics")}</p>
          <p>{t("logicAnalyzer.highRateRange")}</p>
          <div className="flex flex-wrap gap-x-3 gap-y-1">
            <span>{t("logicAnalyzer.captureDurationByRate")}:</span>
            {SAMPLE_RATES.map((rate) => (
              <span key={rate.value}>
                {rate.label}={formatDuration(maxSamples, rate.value)}
              </span>
            ))}
          </div>
        </div>
      </details>

        <div className="mt-4">
          {!streaming && capture && capture.sampleCount > 0 && <>
          <div className="mb-2 flex items-center gap-2 text-xs text-ink-dim">
            <Zap size={12} />
            <span>
              {t("logicAnalyzer.captured")}
              {formatSampleCount(capture.sampleCount)} {t("logicAnalyzer.samples")} @{" "}
              {captureActualRate != null
                ? formatSampleRate(captureActualRate)
                : "—"}
            </span>
          </div>

          <div className="mb-2 flex flex-wrap gap-4 text-[10px] text-ink-dim">
            {captureBackend && (
              <span>
                {t("logicAnalyzer.backend")}: {captureBackend}
              </span>
            )}
            {captureRequestedRate != null && (
              <span>
                {t("logicAnalyzer.requestedRate")}: {formatSampleRate(captureRequestedRate)}
              </span>
            )}
            {captureActualRate != null && (
              <span>
                {t("logicAnalyzer.duration")}: {formatDuration(capture.sampleCount, captureActualRate)}
              </span>
            )}
            {captureSamplePeriod && (
              <span>
                {t("logicAnalyzer.samplePeriod")}: {captureSamplePeriod}
              </span>
            )}
          </div>
          </>}

            <section data-testid="logic-analyzer-decoder" className="mb-4 rounded-xl border border-line/60 bg-panel p-3">
              <div className="flex flex-wrap items-center justify-between gap-3">
                <div className="flex items-center gap-2 text-xs font-medium text-ink">
                  <Binary size={14} className="text-brand" />
                  <span>{t("logicAnalyzer.decoder.title")}</span>
                </div>
                <Badge tone={streaming ? liveDecodeMessage ? "danger" : "brand" : streamSnapshot ? "neutral" : decodeStatusTone}>
                  {t(streaming ? liveDecodeMessage ? "logicAnalyzer.decoder.status.error" : "logicAnalyzer.decoder.live"
                    : streamSnapshot ? "logicAnalyzer.decoder.stopped" : decodeStatusKey)}
                </Badge>
              </div>

              <fieldset disabled={controlsDisabled && !streaming} className="mt-3 grid min-w-0 items-start gap-3 xl:grid-cols-[170px_minmax(0,1fr)]">
              <div
                role="tablist"
                aria-label={t("logicAnalyzer.decoder.protocol")}
                className="inline-flex min-w-full rounded-lg border border-line/70 bg-panel2/30 p-1 xl:flex-col"
              >
                {PROTOCOL_OPTIONS.map((protocolOption) => {
                  const selected = decoderProtocol === protocolOption.id;
                  return (
                    <button
                      key={protocolOption.id}
                      type="button"
                      role="tab"
                      aria-selected={selected}
                      onClick={() => setDecoderProtocol(protocolOption.id)}
                      className={`flex min-h-9 flex-1 items-center justify-center rounded-md px-3 text-xs font-medium transition-colors ${
                        selected
                          ? "bg-brand/10 text-brand ring-1 ring-inset ring-brand/15"
                          : "text-ink-dim hover:text-ink"
                      }`}
                    >
                      {protocolOption.label}
                    </button>
                  );
                })}
              </div>

              <div className="min-w-0">
              <div className="grid gap-3 md:grid-cols-2 xl:grid-cols-3">
                {decoderProtocol === "uart" && (
                  <>
                    {renderChannelSelect("RX", "rx", uartConfig.rxPin, capturePins, false)}
                    <label className="text-[11px] text-ink-dim">
                      {t("logicAnalyzer.decoder.baud")}
                      <input
                        type="number"
                        min="1"
                        step="1"
                        value={uartConfig.baud}
                        onChange={(event) =>
                          updateDecoderConfigs((current) => ({
                            ...current,
                            uart: { ...current.uart, baud: Number(event.target.value) },
                          }))
                        }
                        className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-2 text-xs text-ink"
                      />
                    </label>
                    <label className="text-[11px] text-ink-dim">
                      {t("logicAnalyzer.decoder.dataBits")}
                      <select
                        value={uartConfig.dataBits}
                        onChange={(event) =>
                          updateDecoderConfigs((current) => ({
                            ...current,
                            uart: { ...current.uart, dataBits: Number(event.target.value) },
                          }))
                        }
                        className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-2 text-xs text-ink"
                      >
                        {[5, 6, 7, 8, 9].map((dataBits) => (
                          <option key={dataBits} value={dataBits}>
                            {dataBits}
                          </option>
                        ))}
                      </select>
                    </label>
                    <label className="text-[11px] text-ink-dim">
                      {t("logicAnalyzer.decoder.parity")}
                      <select
                        value={uartConfig.parity}
                        onChange={(event) =>
                          updateDecoderConfigs((current) => ({
                            ...current,
                            uart: {
                              ...current.uart,
                              parity: event.target.value as LogicDecoderProtocolConfigs["uart"]["parity"],
                            },
                          }))
                        }
                        className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-2 text-xs text-ink"
                      >
                        <option value="none">{t("logicAnalyzer.decoder.parity.none")}</option>
                        <option value="even">{t("logicAnalyzer.decoder.parity.even")}</option>
                        <option value="odd">{t("logicAnalyzer.decoder.parity.odd")}</option>
                      </select>
                    </label>
                    <label className="text-[11px] text-ink-dim">
                      {t("logicAnalyzer.decoder.stopBits")}
                      <select
                        value={uartConfig.stopBits}
                        onChange={(event) =>
                          updateDecoderConfigs((current) => ({
                            ...current,
                            uart: { ...current.uart, stopBits: Number(event.target.value) },
                          }))
                        }
                        className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-2 text-xs text-ink"
                      >
                        <option value={1}>1</option>
                        <option value={2}>2</option>
                      </select>
                    </label>
                    <div className="flex items-center justify-between border-b border-line/40 py-2 text-[11px] text-ink-dim md:self-end">
                      <span>{t("logicAnalyzer.decoder.inverted")}</span>
                      <Toggle
                        checked={uartConfig.inverted}
                        onChange={(next) =>
                          updateDecoderConfigs((current) => ({
                            ...current,
                            uart: { ...current.uart, inverted: next },
                          }))
                        }
                      />
                    </div>
                  </>
                )}

                {decoderProtocol === "i2c" && (
                  <>
                    {renderChannelSelect("SCL", "scl", i2cConfig.sclPin, capturePins, false)}
                    {renderChannelSelect("SDA", "sda", i2cConfig.sdaPin, capturePins, false)}
                  </>
                )}

                {decoderProtocol === "spi" && (
                  <>
                    {renderChannelSelect("SCLK", "sclk", spiConfig.sclkPin, capturePins, false)}
                    {renderChannelSelect("MOSI", "mosi", spiConfig.mosiPin, capturePins, true)}
                    {renderChannelSelect("MISO", "miso", spiConfig.misoPin, capturePins, true)}
                    {renderChannelSelect("CS", "cs", spiConfig.csPin, capturePins, true)}
                    <label className="text-[11px] text-ink-dim">
                      {t("logicAnalyzer.decoder.mode")}
                      <select
                        value={spiConfig.mode}
                        onChange={(event) =>
                          updateDecoderConfigs((current) => ({
                            ...current,
                            spi: { ...current.spi, mode: Number(event.target.value) },
                          }))
                        }
                        className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-2 text-xs text-ink"
                      >
                        {[0, 1, 2, 3].map((mode) => (
                          <option key={mode} value={mode}>
                            {t("logicAnalyzer.decoder.mode")} {mode}
                          </option>
                        ))}
                      </select>
                    </label>
                    <label className="text-[11px] text-ink-dim">
                      {t("logicAnalyzer.decoder.bitOrder")}
                      <select
                        value={spiConfig.bitOrder}
                        onChange={(event) =>
                          updateDecoderConfigs((current) => ({
                            ...current,
                            spi: {
                              ...current.spi,
                              bitOrder: event.target.value as LogicDecoderProtocolConfigs["spi"]["bitOrder"],
                            },
                          }))
                        }
                        className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-2 text-xs text-ink"
                      >
                        <option value="msbfirst">{t("logicAnalyzer.decoder.bitOrder.msbfirst")}</option>
                        <option value="lsbfirst">{t("logicAnalyzer.decoder.bitOrder.lsbfirst")}</option>
                      </select>
                    </label>
                    <label className="text-[11px] text-ink-dim">
                      {t("logicAnalyzer.decoder.wordSize")}
                      <input
                        type="number"
                        min="1"
                        max="16"
                        step="1"
                        value={spiConfig.bitsPerWord}
                        onChange={(event) =>
                          updateDecoderConfigs((current) => ({
                            ...current,
                            spi: { ...current.spi, bitsPerWord: Number(event.target.value) },
                          }))
                        }
                        className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-2 text-xs text-ink"
                      />
                    </label>
                    <div className="flex items-center justify-between border-b border-line/40 py-2 text-[11px] text-ink-dim md:self-end">
                      <span>{t("logicAnalyzer.decoder.csActiveHigh")}</span>
                      <Toggle
                        checked={spiConfig.csActiveHigh}
                        onChange={(next) =>
                          updateDecoderConfigs((current) => ({
                            ...current,
                            spi: { ...current.spi, csActiveHigh: next },
                          }))
                        }
                      />
                    </div>
                  </>
                )}
              </div>

              <div className="mt-3 flex flex-wrap gap-2">
                <Button
                  type="button"
                  onClick={runDecode}
                  disabled={controlsDisabled || decoderState.status === "loading" || decodePreparation.request == null}
                >
                  {decoderState.status === "loading" ? (
                    <Loader2 size={14} className="animate-spin" />
                  ) : (
                    <Binary size={14} />
                  )}
                  {decoderState.status === "loading"
                    ? t("logicAnalyzer.decoder.decoding")
                    : replaceTokens(t("logicAnalyzer.decoder.decode"), {
                        protocol: t(`logicAnalyzer.decoder.${decoderProtocol}`),
                      })}
                </Button>
                <Badge tone="neutral">
                  {capturePins.map((pin) => pinLabel(pin)).join(" · ") || t("logicAnalyzer.decoder.noChannels")}
                </Badge>
              </div>

              <div aria-live="polite" className="mt-3 space-y-2">
                {!decodeAvailable && !liveDecodeMessage && <p className="text-xs text-ink-dim">
                  {t(streaming ? "logicAnalyzer.decoder.liveHint" : streamSnapshot
                    ? "logicAnalyzer.decoder.streamStoppedHint" : "logicAnalyzer.decoder.captureRequired")}
                </p>}
                {streaming && localizedLiveDecodeError && (
                  <div role="alert" data-testid="logic-analyzer-live-decode-error"
                    className="rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
                    {localizedLiveDecodeError}
                  </div>
                )}
                {decodeIsStale && (
                  <div className="rounded-lg border border-warn/30 bg-warn/10 px-3 py-2 text-xs text-warn">
                    {t("logicAnalyzer.decoder.staleWarning")}
                  </div>
                )}
                {!decodeIsStale && localizedDecodePreparationError && (
                  <div className="rounded-lg border border-warn/30 bg-warn/10 px-3 py-2 text-xs text-warn">
                    {localizedDecodePreparationError}
                  </div>
                )}
                {localizedDecodeError && (
                  <div className="rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
                    {localizedDecodeError}
                  </div>
                )}
                {decodeResult && (
                  <div className="flex flex-wrap items-center gap-2 text-[11px] text-ink-dim">
                    <Badge tone={decodeResult.annotations.length > 0 ? "ok" : "neutral"}>
                      {replaceTokens(t("logicAnalyzer.decoder.annotationsCount"), {
                        count: decodeResult.annotations.length,
                      })}
                    </Badge>
                    <Badge tone={diagnosticCounts.error > 0 ? "danger" : diagnosticCounts.warning > 0 ? "warn" : "neutral"}>
                      {replaceTokens(t("logicAnalyzer.decoder.diagnosticsCount"), {
                        count: decodeResult.diagnostics.length,
                      })}
                    </Badge>
                    {diagnosticCounts.error > 0 && (
                      <Badge tone="danger">
                        {replaceTokens(t("logicAnalyzer.decoder.errorsCount"), {
                          count: diagnosticCounts.error,
                        })}
                      </Badge>
                    )}
                    {diagnosticCounts.warning > 0 && (
                      <Badge tone="warn">
                        {replaceTokens(t("logicAnalyzer.decoder.warningsCount"), {
                          count: diagnosticCounts.warning,
                        })}
                      </Badge>
                    )}
                  </div>
                )}
              </div>
              </div>
              </fieldset>
            </section>

          {decodeResult && (
            <div className="mt-3 grid gap-4 border-t border-line/50 pt-3 lg:grid-cols-[minmax(0,1fr)_320px]">
              <section className="min-w-0 overflow-hidden rounded-xl border border-line/60 bg-panel">
                {decodeResult.annotations.length === 0 ? (
                  <div className="p-3 text-xs text-ink-dim">{t("logicAnalyzer.decoder.noAnnotations")}</div>
                ) : (
                  <div className="overflow-x-auto">
                    <table className="w-full min-w-[520px] border-collapse text-left text-[11px]">
                      <caption className="border-b border-line/50 bg-panel2/25 px-3 py-2 text-left font-medium text-ink">
                        {t("logicAnalyzer.decoder.annotations")}
                      </caption>
                      <tbody className="divide-y divide-line/40">
                        {decodeResult.annotations.map((annotation, index) => (
                          <tr key={`${annotation.row}-${annotation.startSample}-${index}`} className="align-top hover:bg-panel2/25">
                            <td className="whitespace-nowrap px-3 py-2 font-mono text-ink-dim">{formatAnnotationRange(annotation, sampleRateForAnnotations)}</td>
                            <td className="px-3 py-2"><Badge tone="neutral">{annotation.row}</Badge></td>
                            <td className="px-3 py-2 font-medium text-ink">{annotation.shortText}</td>
                            <td className="px-3 py-2 text-ink-dim">{annotation.longText}</td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  </div>
                )}
              </section>

              <section className="min-w-0 rounded-xl border border-line/60 bg-panel p-3">
                <div className="mb-2 flex items-center gap-2 text-[11px] font-medium text-ink">
                  <TriangleAlert size={13} className="text-warn" />
                  {t("logicAnalyzer.decoder.diagnostics")}
                </div>
                {decodeResult.diagnostics.length === 0 ? (
                  <div className="text-xs text-ink-dim">{t("logicAnalyzer.decoder.noDiagnostics")}</div>
                ) : (
                  <ul className="divide-y divide-line/40 text-xs text-ink-dim">
                    {decodeResult.diagnostics.map((diagnostic, index) => (
                      <li key={`${diagnostic.code}-${diagnostic.startSample}-${index}`} className="py-2 first:pt-0 last:pb-0">
                        <div className="flex flex-wrap items-center gap-2 text-[11px]">
                          <Badge tone={diagnosticTone(diagnostic.severity)}>{diagnostic.severity}</Badge>
                          <span className="font-mono text-ink">{diagnostic.code}</span>
                        </div>
                        <div className="mt-1 text-[11px] text-ink-dim">{diagnostic.message}</div>
                      </li>
                    ))}
                  </ul>
                )}
              </section>
            </div>
          )}
        </div>
      </div>
    </Card>
  );
}
