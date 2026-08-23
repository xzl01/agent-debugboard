import { TELEMETRY_STREAM_BATCH_SIZE } from "@/lib/liveSubscribe";
import { beginPowerCaptureArchive } from "@/lib/powerCaptureStore";
import {
  appendLegacyCaptureSamples,
  completeLegacyCapture,
  createLegacyCaptureBuilder,
  type LegacyCaptureBuilder,
} from "@/lib/powerCaptureWire";
import {
  streamingCaptureLease,
  streamingCaptureRecord,
  type StreamingCaptureSession,
} from "@/lib/streamingCaptureModel";
import {
  queueStreamingLeaseRenewal,
  queueStreamingSamples,
} from "@/lib/streamingCapturePersistence";
import type {
  BoardCaptureProgress,
  BoardCaptureState,
  CaptureConfig,
  CaptureSample,
  PowerCapture,
} from "@/lib/types";

interface CaptureRefBox<T> {
  current: T;
}

export interface CaptureArmPromiseSettlement {
  resolve: () => void;
  reject: (reason: Error) => void;
}

export interface BoardCaptureMessageDeps {
  readonly pendingCaptureRef: CaptureRefBox<CaptureConfig | null>;
  readonly captureBuilderRef: CaptureRefBox<LegacyCaptureBuilder | null>;
  readonly streamingCaptureRef: CaptureRefBox<StreamingCaptureSession | null>;
  readonly captureArmPromiseRef: CaptureRefBox<CaptureArmPromiseSettlement | null>;
  readonly setCaptureState: (state: BoardCaptureState) => void;
  readonly setCaptureProgress: (progress: BoardCaptureProgress | null) => void;
  readonly onCapture: (capture: PowerCapture) => void;
  readonly setError: (message: string | null) => void;
  readonly finalizeStreamingCapture: (
    incomplete?: boolean,
    interruptionReason?: string,
  ) => Promise<void>;
  readonly resetCapture: (reason?: Error) => void;
}

export type BoardCaptureMessageHandler = (
  message: Record<string, unknown>,
  samples: readonly CaptureSample[],
  now: number,
) => boolean;

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

export function createBoardCaptureMessageHandler(
  deps: BoardCaptureMessageDeps,
): BoardCaptureMessageHandler {
  const {
    pendingCaptureRef,
    captureBuilderRef,
    streamingCaptureRef,
    captureArmPromiseRef,
    setCaptureState,
    setCaptureProgress,
    onCapture,
    setError,
    finalizeStreamingCapture,
    resetCapture,
  } = deps;

  return (msg, samples, now) => {
    if (msg.type === "telemetry" || msg.type === "telemetry-batch") {
      const streaming = streamingCaptureRef.current;
      if (
        streaming &&
        !streaming.finishing &&
        (streaming.stopHandshake?.acceptsTelemetry ?? true)
      ) {
        streaming.droppedSamples += Math.max(0, Number(msg.dropped_samples ?? 0));
        const stopForStorageError = (storageError: Error) => {
          setError(storageError.message);
          void finalizeStreamingCapture(true, storageError.message);
        };
        queueStreamingLeaseRenewal(streaming, stopForStorageError);
        if (!streaming.triggered) {
          streaming.preBuffer.push(...samples);
          const keep = Math.max(
            streaming.config.preSamples + TELEMETRY_STREAM_BATCH_SIZE * 2,
            TELEMETRY_STREAM_BATCH_SIZE * 2,
          );
          if (streaming.preBuffer.length > keep) {
            streaming.preBuffer.splice(0, streaming.preBuffer.length - keep);
          }
        } else {
          queueStreamingSamples(streaming, samples, stopForStorageError);
          if (now - streaming.lastProgressAt >= 250) {
            streaming.lastProgressAt = now;
            const expected = streaming.config.stopAfterMs
              ? streaming.triggerOffset + 1 + Math.round(
                streaming.config.stopAfterMs * streaming.config.rateHz / 1000,
              )
              : 0;
            setCaptureProgress({
              received: streaming.totalSamples,
              total: expected,
              persisted: streaming.persistedSamples,
              queuedChunks: streaming.queuedChunks,
              dropped: streaming.droppedSamples,
            });
          }
        }
      }
      return true;
    }
    if (msg.type === "result" && msg.command === "capture_arm") {
      pendingCaptureRef.current = null;
      setCaptureState("armed");
      captureArmPromiseRef.current?.resolve();
      captureArmPromiseRef.current = null;
      return true;
    }
    if (msg.type === "capture_triggered") {
      const streaming = streamingCaptureRef.current;
      if (streaming) {
        streaming.captureId = Number(msg.capture_id ?? Date.now());
        streaming.capturedAt = Date.now();
        streaming.triggerDeviceTimeUs = Number(msg.device_t_mono_us ?? 0);
        streaming.triggerSampleSequence = Number(msg.sample_sequence ?? 0);
        streaming.droppedSamples = Math.max(
          streaming.droppedSamples,
          Math.max(0, Number(msg.dropped_samples ?? 0)),
        );
        streaming.triggered = true;
        streaming.archiveStarted = true;
        const before = streaming.preBuffer
          .filter((sample) => streaming.triggerSampleSequence > 0
            ? sample.sampleSequence < streaming.triggerSampleSequence
            : sample.deviceTimeUs < streaming.triggerDeviceTimeUs)
          .slice(-streaming.config.preSamples);
        const atOrAfter = streaming.preBuffer.filter(
          (sample) => streaming.triggerSampleSequence > 0
            ? sample.sampleSequence >= streaming.triggerSampleSequence
            : sample.deviceTimeUs >= streaming.triggerDeviceTimeUs,
        );
        streaming.preBuffer = [];
        const initial = streamingCaptureRecord(streaming);
        streaming.lastLeaseRenewedAt = Date.now();
        streaming.writeChain = beginPowerCaptureArchive(initial, streamingCaptureLease(streaming));
        queueStreamingSamples(streaming, [...before, ...atOrAfter], (storageError) => {
          setError(storageError.message);
          void finalizeStreamingCapture(true, storageError.message);
        });
        setCaptureState("recording");
        setCaptureProgress({
          received: streaming.totalSamples,
          total: streaming.config.stopAfterMs
            ? streaming.triggerOffset + 1 + Math.round(
              streaming.config.stopAfterMs * streaming.config.rateHz / 1000,
            )
            : 0,
          persisted: streaming.persistedSamples,
          queuedChunks: streaming.queuedChunks,
          dropped: streaming.droppedSamples,
        });
        if (streaming.config.stopAfterMs && streaming.config.stopAfterMs > 0) {
          streaming.stopTimer = setTimeout(
            () => void finalizeStreamingCapture(),
            streaming.config.stopAfterMs,
          );
        }
      } else {
        setCaptureState("recording");
      }
      return true;
    }
    if (msg.type === "result" && msg.command === "capture_stop") {
      const streaming = streamingCaptureRef.current;
      if (streaming?.stopHandshake?.acknowledge(String(msg.id ?? ""))) {
        setCaptureState("receiving");
      }
      return true;
    }
    if (msg.type === "capture_begin" && !streamingCaptureRef.current) {
      captureBuilderRef.current = createLegacyCaptureBuilder(msg);
      setCaptureState("receiving");
      setCaptureProgress({ received: 0, total: Number(msg.sample_count ?? 0) });
      return true;
    }
    if (msg.type === "capture_sample" && captureBuilderRef.current && !streamingCaptureRef.current) {
      const builder = captureBuilderRef.current;
      appendLegacyCaptureSamples(builder, [msg]);
      if (builder.samples.length % 20 === 0 || builder.samples.length === builder.expected) {
        setCaptureProgress({ received: builder.samples.length, total: builder.expected });
      }
      return true;
    }
    if (
      msg.type === "capture_samples" && captureBuilderRef.current &&
      !streamingCaptureRef.current && Array.isArray(msg.samples)
    ) {
      const builder = captureBuilderRef.current;
      appendLegacyCaptureSamples(builder, msg.samples);
      setCaptureProgress({ received: builder.samples.length, total: builder.expected });
      return true;
    }
    if (msg.type === "capture_complete" && captureBuilderRef.current && !streamingCaptureRef.current) {
      const builder = captureBuilderRef.current;
      const completed = completeLegacyCapture(builder, Date.now());
      onCapture(completed);
      captureBuilderRef.current = null;
      setCaptureProgress(null);
      setCaptureState("idle");
      return true;
    }
    if (msg.type === "error" && msg.command === "capture_stop") {
      if (streamingCaptureRef.current) {
        const detail = isRecord(msg.error) ? msg.error : {};
        const stopError = new Error(String(
          detail.message ?? "Power capture could not be stopped",
        ));
        streamingCaptureRef.current.stopHandshake?.fail(stopError);
        setError(stopError.message);
      }
      return true;
    }
    if (msg.type === "error" && msg.command === "capture") {
      const error = isRecord(msg.error) ? msg.error : null;
      const message = typeof error?.message === "string"
        ? error.message
        : "Power capture failed";
      resetCapture(new Error(message));
      setError(message);
      return true;
    }
    return false;
  };
}
