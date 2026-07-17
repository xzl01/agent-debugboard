/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (c) Radxa Computer (Shenzhen) Co., Ltd.
 * Copyright (c) Jiali Chen <chenjiali@radxa.com>
 */

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
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
import { GpioPinoutSvg } from "./GpioPinoutSvg";
import { useLogicStream } from "@/hooks/useLogicStream";
import type {
  LogicAnalyzerCapture,
  LogicAnalyzerConfig,
  LogicAnalyzerState,
  LogicAnalyzerTriggerType,
  SafeGpio,
} from "@/lib/types";
import {
  AVAILABLE_PINS,
  DEFAULT_CONFIG,
  SAMPLE_RATES,
  buildLogicAnalyzerArmRequest,
  calculateActualSampleRate,
  calculateMaxSamples,
  extractLogicAnalyzerErrorMessage,
  exportToCsv,
  exportToSr,
  formatDuration,
  formatSampleCount,
  formatSamplePeriod,
  formatSampleRate,
  getLogicAnalyzerActualSampleRate,
  getLogicAnalyzerBackend,
  getLogicAnalyzerRequestedSampleRate,
  getLogicAnalyzerSamplePeriodPs,
  normalizeLogicAnalyzerCapture,
  normalizeLogicAnalyzerConfig,
} from "@/lib/logicAnalyzer";
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
  "#4f7cff",
  "#f59e0b",
  "#22c55e",
  "#ef4444",
  "#8b5cf6",
  "#ec4899",
  "#06b6d4",
  "#84cc16",
  "#f97316",
  "#6366f1",
  "#14b8a6",
  "#e11d48",
  "#0ea5e9",
  "#a855f7",
  "#10b981",
  "#f43f5e",
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
const LOGIC_ANALYZER_STATES: LogicAnalyzerState[] = [
  "idle",
  "armed",
  "capturing",
  "done",
  "error",
];
const PROTOCOL_OPTIONS: Array<{ id: LogicDecoderProtocolName; label: string }> = [
  { id: "uart", label: "UART" },
  { id: "i2c", label: "I2C" },
  { id: "spi", label: "SPI" },
];

interface WaveformPoint {
  x: number;
  y: number;
}

interface DecoderRunState {
  status: "idle" | "loading" | "done";
  result: LogicDecoderResult | null;
  error: string | null;
  lastRequestSignature: string | null;
}

const INITIAL_DECODER_STATE: DecoderRunState = {
  status: "idle",
  result: null,
  error: null,
  lastRequestSignature: null,
};

function isLogicAnalyzerState(value: unknown): value is LogicAnalyzerState {
  return typeof value === "string" && LOGIC_ANALYZER_STATES.includes(value as LogicAnalyzerState);
}

function pinLabel(pin: number | null): string {
  return pin == null ? "—" : `GP${pin}`;
}

function replaceTokens(template: string, values: Record<string, string | number>): string {
  return Object.entries(values).reduce(
    (text, [key, value]) => text.replaceAll(`{${key}}`, String(value)),
    template
  );
}

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

export function LogicAnalyzerCard({ boardGpios }: { boardGpios?: SafeGpio[] }) {
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
  const pollingRef = useRef<ReturnType<typeof setInterval> | null>(null);
  const armInFlightRef = useRef(false);
  const currentDecodeSignatureRef = useRef<string | null>(null);
  const streamSamplesRef = useRef<number[]>([]);
  const streamSequenceRef = useRef(0);
  const streamTotalRef = useRef(0);
  const streamDirtyRef = useRef(false);
  const streamDecodeBusyRef = useRef(false);
  const [streamSampleCount, setStreamSampleCount] = useState(0);
  const [streamWaveformVersion, setStreamWaveformVersion] = useState(0);
  const [streamFollow, setStreamFollow] = useState(true);
  const [streamSpan, setStreamSpan] = useState<number>(4096);
  const [streamAnchor, setStreamAnchor] = useState(0);
  const [liveAnnotations, setLiveAnnotations] = useState<LogicDecoderAnnotation[]>([]);

  const {
    streaming,
    streamRate,
    streamError,
    startStream,
    stopStream,
    onChunk,
  } = useLogicStream();

  useEffect(() => {
    onChunk((chunk) => {
      const arr = streamSamplesRef.current;
      for (let i = 0; i < chunk.count; i++) {
        arr.push(chunk.values[i]);
      }
      if (arr.length > STREAM_BUFFER_CAP) {
        arr.splice(0, arr.length - STREAM_BUFFER_CAP);
      }
      streamSequenceRef.current = chunk.seq + 1;
      streamTotalRef.current += chunk.count;
      streamDirtyRef.current = true;
    });
  }, [onChunk]);

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
    setConfig((current) => normalizeLogicAnalyzerConfig(updater(current)));
  }, []);

  const actualRate = useMemo(
    () => calculateActualSampleRate(config.sampleRateHz),
    [config.sampleRateHz]
  );

  const streamRateExceeded = config.sampleRateHz > 25000000;

  const maxSamples = calculateMaxSamples();
  const totalSamples = config.preSamples + config.postSamples;
  const controlsDisabled = state !== "idle" || isArming;
  const captureRequestedRate = capture ? getLogicAnalyzerRequestedSampleRate(capture.config) : null;
  const captureActualRate = capture ? getLogicAnalyzerActualSampleRate(capture.config) : null;
  const captureSamplePeriod = capture
    ? formatSamplePeriod(getLogicAnalyzerSamplePeriodPs(capture.config))
    : null;
  const captureBackend = capture ? getLogicAnalyzerBackend(capture.config) : null;
  const capturePins = useMemo(
    () => (capture ? getLogicDecoderCapturePins(capture.config) : config.selectedPins),
    [capture, config.selectedPins]
  );
  const capturePinsKey = capturePins.join(",");

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

  const readResponseErrorMessage = useCallback(async (res: Response) => {
    try {
      const payload: unknown = await res.json();
      return extractLogicAnalyzerErrorMessage(payload);
    } catch {
      return null;
    }
  }, []);

  const fetchCapture = useCallback(async () => {
    try {
      const res = await fetch("/api/v1/logic-analyzer/capture");
      if (!res.ok) return null;
      const data: LogicAnalyzerCapture = normalizeLogicAnalyzerCapture(await res.json());
      setCapture(data);
      return data;
    } catch {
      return null;
    }
  }, []);

  const fetchStatus = useCallback(async () => {
    try {
      const res = await fetch("/api/v1/logic-analyzer");
      if (!res.ok) return null;
      const data: unknown = await res.json();
      if (!data || typeof data !== "object") return null;

      const nextState = (data as Record<string, unknown>).state;
      if (isLogicAnalyzerState(nextState)) {
        setState(nextState);
        if (nextState === "done") {
          await fetchCapture();
        }
        return nextState;
      }
    } catch {
      return null;
    }
    return null;
  }, [fetchCapture]);

  useEffect(() => {
    if (state === "armed" || state === "capturing") {
      pollingRef.current = setInterval(fetchStatus, 200);
    } else if (pollingRef.current) {
      clearInterval(pollingRef.current);
      pollingRef.current = null;
    }

    return () => {
      if (pollingRef.current) {
        clearInterval(pollingRef.current);
      }
    };
  }, [state, fetchStatus]);

  useEffect(() => {
    void (async () => {
      try {
        const res = await fetch("/api/v1/logic-analyzer");
        if (!res.ok) return;
        const data: unknown = await res.json();
        if (!data || typeof data !== "object") return;

        const nextState = (data as Record<string, unknown>).state;
        if (!isLogicAnalyzerState(nextState)) return;

        if (nextState === "armed" || nextState === "capturing") {
          await fetch("/api/v1/logic-analyzer", { method: "DELETE" });
          setState("idle");
          setCapture(null);
          return;
        }

        setState(nextState);
        if (nextState === "done") {
          await fetchCapture();
        }
      } catch {
        /* ignore initial state sync failure */
      }
    })();
  }, [fetchCapture]);

  const handleArm = useCallback(async () => {
    if (armInFlightRef.current) return;

    setError(null);

    const nextConfig = normalizeLogicAnalyzerConfig(config);
    setConfig(nextConfig);

    if (nextConfig.selectedPins.length === 0) {
      setError("Select at least one pin");
      return;
    }
    const armRequest = buildLogicAnalyzerArmRequest(nextConfig);

    armInFlightRef.current = true;
    setIsArming(true);

    try {
      const res = await fetch("/api/v1/logic-analyzer", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(armRequest),
      });
      if (!res.ok) {
        const message = await readResponseErrorMessage(res);
        if (res.status === 409) {
          await fetchStatus();
          if (message) {
            setError(message);
          }
          return;
        }
        setError(message || "Failed to arm");
        return;
      }
      setState("armed");
    } catch (err) {
      setError(err instanceof Error ? err.message : "Network error");
    } finally {
      armInFlightRef.current = false;
      setIsArming(false);
    }
  }, [config, fetchStatus, readResponseErrorMessage]);

  const handleCancel = useCallback(async () => {
    setError(null);

    try {
      const res = await fetch("/api/v1/logic-analyzer", { method: "DELETE" });
      if (!res.ok) {
        const message = await readResponseErrorMessage(res);
        setError(message || "Failed to cancel");
        return;
      }
      setState("idle");
    } catch (err) {
      setError(err instanceof Error ? err.message : "Network error");
    }
  }, [readResponseErrorMessage]);

  const handleClear = useCallback(() => {
    setCapture(null);
    setState("idle");
    setDecoderState(INITIAL_DECODER_STATE);
  }, []);

  const waveformData = useMemo(() => {
    if (!capture || capture.sampleCount === 0) return [];

    const plotWidth = Math.max(1, WIDTH - PLOT_LEFT - PLOT_RIGHT);
    const xScale = capture.sampleCount > 1 ? plotWidth / (capture.sampleCount - 1) : 0;

    return Array.from({ length: capturePins.length }, (_, channelIndex) => {
      const channelPoints: WaveformPoint[] = [];

      for (let sampleIndex = 0; sampleIndex < capture.sampleCount; sampleIndex += 1) {
        const sample = capture.samples[sampleIndex];
        if (!sample) continue;

        const bit = (sample.values >> channelIndex) & 1;
        channelPoints.push({
          x: PLOT_LEFT + sampleIndex * xScale,
          y: bit ? CHANNEL_GAP : CHANNEL_HEIGHT - CHANNEL_GAP,
        });
      }

      return channelPoints;
    });
  }, [capture, capturePins]);

  const streamWindow = useMemo(() => {
    const samples = streamSamplesRef.current;
    const len = samples.length;
    const plotWidth = Math.max(1, WIDTH - PLOT_LEFT - PLOT_RIGHT);
    const span = Math.min(streamSpan, Math.max(1, len));
    const end = streamFollow ? len : Math.min(len, Math.max(span, len - streamAnchor));
    const start = end - span;
    const stride = Math.max(1, Math.floor(span / plotWidth));
    const pointCount = Math.min(plotWidth, Math.max(1, Math.ceil(span / stride)));
    return { samples, len, span, start, end, stride, pointCount, plotWidth };
    // streamSamplesRef content is versioned through streamWaveformVersion
  }, [streamWaveformVersion, streamFollow, streamSpan, streamAnchor]);

  const streamWaveformData = useMemo(() => {
    if (!streaming) return [];
    const pins = config.selectedPins;
    if (pins.length === 0 || streamWindow.len === 0) return [];
    const { samples, start, span, stride, pointCount, plotWidth } = streamWindow;
    const xScale = pointCount > 1 ? plotWidth / (pointCount - 1) : 0;

    return pins.map((_, channelIndex) => {
      const channelPoints: WaveformPoint[] = [];
      for (let i = 0; i < pointCount; i += 1) {
        const sampleIndex = start + Math.min(i * stride, span - 1);
        const bit = (samples[sampleIndex] >> channelIndex) & 1;
        channelPoints.push({
          x: PLOT_LEFT + i * xScale,
          y: bit ? CHANNEL_GAP : CHANNEL_HEIGHT - CHANNEL_GAP,
        });
      }
      return channelPoints;
    });
  }, [streaming, config.selectedPins, streamWindow]);

  const liveAnnotationLayout = useMemo(
    () => layoutLogicDecoderAnnotations(liveAnnotations),
    [liveAnnotations]
  );
  const liveAnnotationAreaHeight = liveAnnotationLayout.laneCount > 0
    ? liveAnnotationLayout.laneCount * (ANNOTATION_HEIGHT + ANNOTATION_GAP) + ANNOTATION_MARGIN
    : 0;
  const streamChannelTop = TOP_PADDING + liveAnnotationAreaHeight;

  const streamSvgHeight = useMemo(() => {
    const pinCount = config.selectedPins.length || 1;
    return streamChannelTop + pinCount * CHANNEL_HEIGHT + Math.max(0, pinCount - 1) * CHANNEL_GAP + BOTTOM_PADDING;
  }, [config.selectedPins.length, streamChannelTop]);

  const streamWindowRef = useRef(streamWindow);
  // FIXME: render-time ref assignment works for the latest-window pattern but
  // is impure; move into useEffect when refactoring the decode scheduler.
  streamWindowRef.current = streamWindow;
  // FIXME: streamRateExceeded below hardcodes 25 MHz; share the limit with the
  // firmware stream cap (see doc/logic-analyzer.md).
  const streamMaxAnchor = Math.max(0, streamWindow.len - streamWindow.span);
  const streamDecodeBase = streamWindow.end - Math.min(streamWindow.span, STREAM_DECODE_MAX_SAMPLES);

  useEffect(() => {
    if (!streaming) {
      setLiveAnnotations([]);
      return;
    }
    let cancelled = false;
    const decodeWindow = async () => {
      if (streamDecodeBusyRef.current) return;
      const { samples, start, end } = streamWindowRef.current;
      const decodeLen = Math.min(end - start, STREAM_DECODE_MAX_SAMPLES);
      if (decodeLen <= 0 || config.selectedPins.length === 0) return;
      streamDecodeBusyRef.current = true;
      try {
        const pseudoCapture: LogicAnalyzerCapture = {
          state: "done",
          config: {
            pinCount: config.selectedPins.length,
            pinBase: config.selectedPins[0] ?? 0,
            selectedPins: [...config.selectedPins],
            actualSampleRateHz: streamRate ?? actualRate,
          },
          sampleCount: decodeLen,
          triggerIndex: 0,
          samples: samples.slice(end - decodeLen, end).map((values) => ({ timestampUs: 0, values })),
        };
        const request = buildLogicDecoderRequest(pseudoCapture, decoderProtocol, decoderConfigs);
        const result = await decodeLogicCapture(request);
        if (!cancelled) setLiveAnnotations(result.annotations);
      } catch {
        if (!cancelled) setLiveAnnotations([]);
      } finally {
        streamDecodeBusyRef.current = false;
      }
    };
    const timer = window.setInterval(() => { void decodeWindow(); }, STREAM_DECODE_MS);
    void decodeWindow();
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, [streaming, decoderProtocol, decoderConfigs, config.selectedPins, streamRate, actualRate]);

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
    currentDecodeSignatureRef.current = decodePreparation.requestSignature;
  }, [decodePreparation.requestSignature]);

  const decodeIsStale =
    decoderState.lastRequestSignature != null &&
    decoderState.lastRequestSignature !== decodePreparation.requestSignature;
  const decodeResult = decodeIsStale ? null : decoderState.result;
  const decodeError = decodeIsStale ? null : decoderState.error;
  const annotationLayout = useMemo(
    () => layoutLogicDecoderAnnotations(decodeResult?.annotations ?? []),
    [decodeResult]
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
    if (!capture || capture.sampleCount <= 0) return null;
    const plotWidth = WIDTH - PLOT_LEFT - PLOT_RIGHT;
    return PLOT_LEFT + (capture.triggerIndex / Math.max(1, capture.sampleCount)) * plotWidth;
  }, [capture]);

  const svgHeight = useMemo(() => {
    const pinCount = capturePins.length || 1;
    return Math.max(
      140,
      channelTop +
        pinCount * CHANNEL_HEIGHT +
        Math.max(0, pinCount - 1) * CHANNEL_GAP +
        BOTTOM_PADDING
    );
  }, [capturePins.length, channelTop]);

  const sampleRateForAnnotations = captureActualRate ?? actualRate;
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
    setDecoderState({
      status: "loading",
      result: null,
      error: null,
      lastRequestSignature: requestSignature,
    });

    try {
      const result = await decodeLogicCapture(decodePreparation.request);
      if (currentDecodeSignatureRef.current !== requestSignature) {
        return;
      }
      setDecoderState({
        status: "done",
        result,
        error: null,
        lastRequestSignature: requestSignature,
      });
    } catch (decodeRunError) {
      if (currentDecodeSignatureRef.current !== requestSignature) {
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
      title={t("logicAnalyzer.title")}
      subtitle={t("logicAnalyzer.subtitle")}
      icon={Activity}
      right={
        <Badge tone={state === "idle" ? "neutral" : state === "armed" ? "warn" : "brand"}>
          {t(`logicAnalyzer.state.${state}`)}
        </Badge>
      }
    >
      {error && (
        <div className="mb-3 rounded-lg border border-danger/30 bg-danger/10 px-3 py-2 text-xs text-danger">
          {error}
        </div>
      )}

      <div className="flex flex-col gap-3 lg:flex-row lg:items-start">
        <div className="text-[11px] text-ink-dim lg:max-w-[220px]">
          <div className="flex items-baseline justify-between gap-2">
            {t("logicAnalyzer.selectPins")}
            <span className="text-[9px] text-ink-dim/70">{t("logicAnalyzer.pinHint")}</span>
          </div>
          {boardGpios && boardGpios.length > 0 ? (
            <div className="mt-1">
              <GpioPinoutSvg
                gpios={boardGpios}
                selectedPins={config.selectedPins}
                triggerPin={
                  config.triggerType !== "none" && config.selectedPins.length > 0
                    ? config.selectedPins[config.triggerPin] ?? null
                    : null
                }
                triggerActive={config.triggerType !== "none"}
                onTogglePin={(pin) => {
                  if (controlsDisabled) return;
                  updateConfig((current) => {
                    const has = current.selectedPins.includes(pin);
                    if (has) {
                      const nextSelected = current.selectedPins.filter((p) => p !== pin);
                      const triggerPin = nextSelected.length === 0
                        ? 0
                        : Math.min(current.triggerPin, nextSelected.length - 1);
                      return { ...current, selectedPins: nextSelected, triggerPin };
                    }
                    return { ...current, selectedPins: [...current.selectedPins, pin] };
                  });
                }}
                onSetTriggerPin={(pin) => {
                  if (controlsDisabled) return;
                  if (pin == null) {
                    updateConfig((current) => ({ ...current, triggerType: "none" }));
                    return;
                  }
                  updateConfig((current) => {
                    const selectedPins = current.selectedPins.includes(pin)
                      ? current.selectedPins
                      : [...current.selectedPins, pin];
                    return {
                      ...current,
                      selectedPins,
                      triggerPin: selectedPins.indexOf(pin),
                      triggerType: current.triggerType === "none" ? "either" : current.triggerType,
                    };
                  });
                }}
              />
            </div>
          ) : (
            <div className="mt-1 flex flex-wrap gap-1.5">
              {AVAILABLE_PINS.map((pin) => (
                <label
                  key={pin.pin}
                  className={`inline-flex cursor-pointer items-center gap-1 rounded-md border px-2 py-0.5 text-[11px] transition-colors ${
                    config.selectedPins.includes(pin.pin)
                      ? "border-brand bg-brand/10 text-brand"
                      : "border-line bg-panel text-ink-dim hover:border-brand/40"
                  } ${controlsDisabled ? "cursor-not-allowed opacity-50" : ""}`}
                >
                  <input
                    type="checkbox"
                    checked={config.selectedPins.includes(pin.pin)}
                    onChange={(event) => {
                      if (event.target.checked) {
                        updateConfig((current) => ({ ...current, selectedPins: [...current.selectedPins, pin.pin] }));
                        return;
                      }
                      updateConfig((current) => ({ ...current, selectedPins: current.selectedPins.filter((selectedPin) => selectedPin !== pin.pin) }));
                    }}
                    disabled={controlsDisabled}
                    className="sr-only"
                  />
                  <span className="font-mono">{pin.name}</span>
                </label>
              ))}
            </div>
          )}
        </div>

        <div className="lg:flex-1">
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
                type="text"
                inputMode="numeric"
                pattern="[0-9]*"
                placeholder="0"
                value={config.preSamples || ""}
                onChange={(event) => {
                  const value = event.target.value.replace(/[^0-9]/g, "");
                  updateConfig((current) => ({ ...current, preSamples: value ? Number(value) : 0 }));
                }}
                disabled={controlsDisabled || config.triggerType === "none"}
                className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-1.5 text-xs text-ink"
              />
              {config.triggerType === "none" && (
                <span className="mt-0.5 block text-[9px] text-ink-dim/60">
                  {t("logicAnalyzer.preTriggerOnlyEdge")}
                </span>
              )}
            </label>

            <label className="text-[11px] text-ink-dim">
              {t("logicAnalyzer.postSamples")}
              <input
                type="number"
                min="1"
                max={maxSamples - config.preSamples}
                value={config.postSamples}
                onChange={(event) =>
                  updateConfig((current) => ({ ...current, postSamples: Number(event.target.value) }))
                }
                disabled={controlsDisabled}
                className="mt-1 w-full rounded-lg border border-line bg-panel px-2 py-1.5 text-xs text-ink"
              />
            </label>

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

          {streaming && streamWaveformData.length > 0 && (
            // FIXME: the step-path lane rendering below duplicates the capture
            // waveform svg almost verbatim; extract a shared StepWaveform
            // component when the live view gains more features.
            <div className="mt-3">
              <div className="mb-2 flex flex-wrap items-center gap-3 text-[10px] text-ink-dim">
                <span className="flex items-center gap-1 text-xs">
                  <Activity size={12} />
                  {t("logicAnalyzer.streamWaveform")}
                </span>
                <label className="inline-flex items-center gap-1">
                  <input
                    type="checkbox"
                    checked={streamFollow}
                    onChange={(event) => {
                      setStreamFollow(event.target.checked);
                      if (event.target.checked) setStreamAnchor(0);
                    }}
                  />
                  {t("logicAnalyzer.streamFollow")}
                </label>
                <select
                  value={streamSpan}
                  onChange={(event) => {
                    setStreamSpan(Number(event.target.value));
                    setStreamAnchor(0);
                  }}
                  className="rounded border border-line bg-panel px-1 py-0.5 text-[10px] text-ink"
                  aria-label={t("logicAnalyzer.streamWindow")}
                >
                  {STREAM_SPAN_OPTIONS.map((spanOption) => (
                    <option key={spanOption} value={spanOption}>
                      {formatSampleCount(spanOption)}
                    </option>
                  ))}
                </select>
                {!streamFollow && streamMaxAnchor > 0 && (
                  <input
                    type="range"
                    min={0}
                    max={streamMaxAnchor}
                    value={streamMaxAnchor - streamAnchor}
                    onChange={(event) => setStreamAnchor(streamMaxAnchor - Number(event.target.value))}
                    className="min-w-24 flex-1"
                    aria-label={t("logicAnalyzer.streamWindow")}
                  />
                )}
              </div>
              <svg
                viewBox={`0 0 ${WIDTH} ${streamSvgHeight}`}
                className="w-full rounded-lg border border-line/60 bg-panel"
                style={{ height: `${streamSvgHeight}px` }}
                role="img"
                aria-label={t("logicAnalyzer.streamWaveform")}
              >
                {liveAnnotationLayout.annotations.map((annotation, index) => {
                  const xScale = streamWindow.pointCount > 1
                    ? streamWindow.plotWidth / (streamWindow.pointCount - 1)
                    : 0;
                  const xFor = (samplePos: number) =>
                    PLOT_LEFT + (((streamDecodeBase + samplePos) - streamWindow.start) / streamWindow.stride) * xScale;
                  const x1 = xFor(annotation.startSample);
                  const x2 = xFor(annotation.endSample);
                  const width = Math.max(4, x2 - x1);
                  const y = TOP_PADDING + annotation.lane * (ANNOTATION_HEIGHT + ANNOTATION_GAP);
                  const palette = annotationPalette(annotation.class);
                  const labelFits = width >= annotation.shortText.length * 5.5 + 10;

                  return (
                    <g key={`live-${annotation.row}-${annotation.startSample}-${index}`}>
                      <rect
                        x={x1}
                        y={y}
                        width={width}
                        height={ANNOTATION_HEIGHT}
                        rx={4}
                        ry={4}
                        fill={palette.fill}
                        stroke={palette.stroke}
                      />
                      <title>{annotation.longText}</title>
                      {labelFits && (
                        <text
                          x={x1 + 4}
                          y={y + 12}
                          style={{ fill: palette.text }}
                          className="text-[9px] font-medium"
                        >
                          {annotation.shortText}
                        </text>
                      )}
                    </g>
                  );
                })}
                {config.selectedPins.map((pin, channelIndex) => {
                  const yOffset = streamChannelTop + channelIndex * (CHANNEL_HEIGHT + CHANNEL_GAP);
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
                {streamWaveformData.map((channelPoints, channelIndex) => {
                  if (channelPoints.length === 0) return null;
                  const yOffset = streamChannelTop + channelIndex * (CHANNEL_HEIGHT + CHANNEL_GAP);
                  const pathPoints = channelPoints.map((point) => ({
                    x: point.x,
                    y: yOffset + point.y,
                  }));

                  let d = `M ${pathPoints[0]?.x ?? 0} ${pathPoints[0]?.y ?? 0}`;
                  for (let pointIndex = 1; pointIndex < pathPoints.length; pointIndex += 1) {
                    const previous = pathPoints[pointIndex - 1];
                    const current = pathPoints[pointIndex];
                    if (!previous || !current) continue;

                    if (current.y !== previous.y) {
                      d += ` L ${current.x} ${previous.y}`;
                      d += ` L ${current.x} ${current.y}`;
                    } else {
                      d += ` L ${current.x} ${current.y}`;
                    }
                  }

                  return (
                    <path
                      key={config.selectedPins[channelIndex] ?? channelIndex}
                      d={d}
                      fill="none"
                      stroke={WAVEFORM_COLORS[channelIndex % WAVEFORM_COLORS.length]}
                      strokeWidth={1.5}
                      vectorEffect="non-scaling-stroke"
                    />
                  );
                })}
              </svg>
            </div>
          )}
          {!streaming && capture && capture.sampleCount > 0 && (
            <div className="mt-3">
              <svg
                viewBox={`0 0 ${WIDTH} ${svgHeight}`}
                className="w-full rounded-lg border border-line/60 bg-panel"
                style={{ height: `${svgHeight}px` }}
                role="img"
                aria-label={t("logicAnalyzer.decoder.waveformAria")}
              >
                {annotationLayout.annotations.map((annotation, index) => {
                  if (!capture) return null;

                  const plotWidth = WIDTH - PLOT_LEFT - PLOT_RIGHT;
                  const x1 = PLOT_LEFT + (annotation.startSample / capture.sampleCount) * plotWidth;
                  const x2 = PLOT_LEFT + (annotation.endSample / capture.sampleCount) * plotWidth;
                  const width = Math.max(10, x2 - x1);
                  const y = TOP_PADDING + annotation.lane * (ANNOTATION_HEIGHT + ANNOTATION_GAP);
                  const palette = annotationPalette(annotation.class);
                  const labelFits = width >= annotation.shortText.length * 5.5 + 10;

                  return (
                    <g key={`${annotation.row}-${annotation.startSample}-${annotation.endSample}-${index}`}>
                      <rect
                        x={x1}
                        y={y}
                        width={width}
                        height={ANNOTATION_HEIGHT}
                        rx={4}
                        ry={4}
                        fill={palette.fill}
                        stroke={palette.stroke}
                      />
                      <title>{annotation.longText}</title>
                      {labelFits && (
                        <text
                          x={x1 + 4}
                          y={y + 12}
                          style={{ fill: palette.text }}
                          className="text-[9px] font-medium"
                        >
                          {annotation.shortText}
                        </text>
                      )}
                    </g>
                  );
                })}

                {capturePins.map((pin, channelIndex) => {
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

                {triggerLineX !== null && (
                  <line
                    x1={triggerLineX}
                    x2={triggerLineX}
                    y1={10}
                    y2={svgHeight - 10}
                    stroke="rgb(var(--c-danger))"
                    strokeDasharray="4 3"
                    strokeWidth={1}
                  />
                )}

                {waveformData.map((channelPoints, channelIndex) => {
                  if (channelPoints.length === 0) return null;

                  const yOffset = channelTop + channelIndex * (CHANNEL_HEIGHT + CHANNEL_GAP);
                  const pathPoints = channelPoints.map((point) => ({
                    x: point.x,
                    y: yOffset + point.y,
                  }));

                  let d = `M ${pathPoints[0]?.x ?? 0} ${pathPoints[0]?.y ?? 0}`;
                  for (let pointIndex = 1; pointIndex < pathPoints.length; pointIndex += 1) {
                    const previous = pathPoints[pointIndex - 1];
                    const current = pathPoints[pointIndex];
                    if (!previous || !current) continue;

                    if (current.y !== previous.y) {
                      d += ` L ${current.x} ${previous.y}`;
                      d += ` L ${current.x} ${current.y}`;
                    } else {
                      d += ` L ${current.x} ${current.y}`;
                    }
                  }

                  return (
                    <path
                      key={capturePins[channelIndex] ?? channelIndex}
                      d={d}
                      fill="none"
                      stroke={WAVEFORM_COLORS[channelIndex % WAVEFORM_COLORS.length]}
                      strokeWidth={1.5}
                      vectorEffect="non-scaling-stroke"
                    />
                  );
                })}
              </svg>

              <div className="mt-1 flex justify-between text-[9px] text-ink-dim">
                <span>0</span>
                <span>{t("logicAnalyzer.triggerPoint")}</span>
                <span>{formatSampleCount(capture.sampleCount)}</span>
              </div>
            </div>
          )}
        </div>
      </div>

      <div className="mt-2 flex flex-wrap gap-4 text-[10px] text-ink-dim">
        <span>
          {t("logicAnalyzer.actualRate")}: {formatSampleRate(actualRate)}
        </span>
        <span>
          {t("logicAnalyzer.totalSamples")}: {formatSampleCount(totalSamples)}
        </span>
        <span>
          {t("logicAnalyzer.duration")}: {formatDuration(totalSamples, actualRate)}
        </span>
        <span>
          {t("logicAnalyzer.maxSamples")}: {formatSampleCount(maxSamples)}
          <span className="ml-1 text-ink-dim/70">
            ({formatDuration(maxSamples, actualRate)})
          </span>
        </span>
      </div>
      <div className="mt-1 flex flex-wrap gap-3 text-[9px] text-ink-dim/70">
        <span>{t("logicAnalyzer.captureDurationByRate")}:</span>
        {SAMPLE_RATES.map((rate) => (
          <span key={rate.value}>
            {rate.label}={formatDuration(maxSamples, rate.value)}
          </span>
        ))}
      </div>

      <div className="mt-3 flex flex-wrap gap-2">
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
              disabled={
                controlsDisabled ||
                totalSamples > maxSamples ||
                totalSamples === 0 ||
                config.selectedPins.length === 0
              }
            >
              {isArming ? <Loader2 size={15} className="animate-spin" /> : <Radio size={15} />}
              {isArming ? t("logicAnalyzer.arming") : t("logicAnalyzer.arm")}
            </Button>
            <Button
              type="button"
              variant="ghost"
              title={streamRateExceeded ? t("logicAnalyzer.streamRateLimit") : undefined}
              onClick={() => {
                streamSamplesRef.current = [];
                streamSequenceRef.current = 0;
                streamTotalRef.current = 0;
                setStreamSampleCount(0);
                void startStream({
                  sampleRateHz: config.sampleRateHz,
                  selectedPins: config.selectedPins,
                  pinCount: config.selectedPins.length,
                  pinBase: config.selectedPins[0] ?? 0,
                });
              }}
              disabled={config.selectedPins.length === 0 || streamRateExceeded}
            >
              <Zap size={15} />
              {t("logicAnalyzer.startStream")}
            </Button>
            {streamRateExceeded && (
              <span className="self-center text-[10px] text-ink-dim">
                {t("logicAnalyzer.streamRateLimit")}
              </span>
            )}
          </>
        ) : (
          <Button type="button" onClick={handleCancel}>
            <Square size={15} />
            {t("logicAnalyzer.cancel")}
          </Button>
        )}

        {capture && state === "done" && (
          <>
            <Button type="button" variant="ghost" onClick={() => exportToCsv(capture)}>
              <Download size={13} />
              CSV
            </Button>
            <Button type="button" variant="ghost" onClick={() => exportToSr(capture)}>
              <Download size={13} />
              PulseView (.sr)
            </Button>
            <Button type="button" variant="ghost" onClick={handleClear}>
              <Trash2 size={13} />
            </Button>
          </>
        )}
      </div>

      {streaming && (
        <div className="mb-3 flex flex-wrap items-center gap-3 rounded-lg border border-brand/30 bg-brand/5 px-3 py-2 text-xs text-ink">
          <Badge tone="brand">{t("logicAnalyzer.streaming")}</Badge>
          <span>{formatSampleRate(streamRate ?? actualRate)}</span>
          <span>{formatSampleCount(streamSampleCount)} {t("logicAnalyzer.samples")}</span>
          {streamError && (
            <span className="text-danger">{streamError}</span>
          )}
        </div>
      )}

      {capture && capture.sampleCount > 0 && (
        <div className="mt-4">
          <div className="mb-2 flex items-center gap-2 text-xs text-ink-dim">
            <Zap size={12} />
            <span>
              {t("logicAnalyzer.captured")}
              {formatSampleCount(capture.sampleCount)} {t("logicAnalyzer.samples")} @{" "}
              {captureActualRate != null
                ? formatSampleRate(captureActualRate)
                : formatSampleRate(actualRate)}
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
                {t("logicAnalyzer.actualRate")}: {formatSampleRate(captureActualRate)}
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

          {state === "done" && (
            <section className="mb-4 border-t border-line/60 pt-4">
              <div className="flex flex-wrap items-center justify-between gap-3">
                <div className="flex items-center gap-2 text-xs font-medium text-ink">
                  <Binary size={14} className="text-brand" />
                  <span>{t("logicAnalyzer.decoder.title")}</span>
                </div>
                <Badge tone={decodeStatusTone}>{t(decodeStatusKey)}</Badge>
              </div>

              <div
                role="tablist"
                aria-label={t("logicAnalyzer.decoder.protocol")}
                className="mt-3 inline-flex min-w-full rounded-lg border border-line/70 p-1 sm:min-w-0"
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
                        selected ? "bg-brand text-white" : "text-ink-dim hover:text-ink"
                      }`}
                    >
                      {protocolOption.label}
                    </button>
                  );
                })}
              </div>

              <div className="mt-3 grid gap-3 border-t border-line/50 pt-3 md:grid-cols-2 xl:grid-cols-3">
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
                  disabled={decoderState.status === "loading" || decodePreparation.request == null}
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
            </section>
          )}

          {decodeResult && (
            <div className="mt-3 grid gap-4 border-t border-line/50 pt-3 lg:grid-cols-[minmax(0,1fr)_320px]">
              <section className="min-w-0">
                <div className="mb-2 text-[11px] font-medium text-ink">{t("logicAnalyzer.decoder.annotations")}</div>
                {decodeResult.annotations.length === 0 ? (
                  <div className="text-xs text-ink-dim">{t("logicAnalyzer.decoder.noAnnotations")}</div>
                ) : (
                  <ul className="divide-y divide-line/40 text-xs text-ink-dim">
                    {decodeResult.annotations.map((annotation, index) => (
                      <li key={`${annotation.row}-${annotation.startSample}-${index}`} className="py-2 first:pt-0 last:pb-0">
                        <div className="flex flex-wrap items-center gap-2 text-[11px]">
                          <Badge tone="neutral">{annotation.row}</Badge>
                          <span className="font-medium text-ink">{annotation.shortText}</span>
                          <span className="font-mono text-ink-dim">
                            {formatAnnotationRange(annotation, sampleRateForAnnotations)}
                          </span>
                        </div>
                        <div className="mt-1 text-[11px] text-ink-dim">{annotation.longText}</div>
                      </li>
                    ))}
                  </ul>
                )}
              </section>

              <section className="min-w-0 lg:border-l lg:border-line/40 lg:pl-4">
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
      )}
    </Card>
  );
}
