import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import {
  liveSubscribeMessage,
  TELEMETRY_STREAM_BATCH_SIZE,
} from "@/lib/liveSubscribe";
import {
  MAX_POWER_CAPTURE_PRE_TRIGGER_SAMPLES,
  POWER_CAPTURE_PROTOCOL,
} from "@/lib/powerCapture";
import {
  clearPowerCaptureArchives,
  ensurePowerCaptureStorageCapacity,
  listRecentPowerCaptures,
  recoverStalePowerCaptureArchives,
} from "@/lib/powerCaptureStore";
import {
  powerCaptureArmMessage,
  type LegacyCaptureBuilder,
} from "@/lib/powerCaptureWire";
import {
  discardStreamingCaptureArchive,
  finalizeStreamingCaptureArchive,
} from "@/lib/streamingCaptureFinalize";
import {
  createArchiveId,
  createStreamingCaptureSession,
  MAX_WEB_STREAMING_RATE_HZ,
  type StreamingCaptureSession,
} from "@/lib/streamingCaptureModel";
import type {
  BoardCaptureProgress,
  BoardCaptureState,
  CaptureConfig,
  PowerCapture,
} from "@/lib/types";

import {
  createBoardCaptureMessageHandler,
  type BoardCaptureMessageHandler,
  type CaptureArmPromiseSettlement,
} from "./useBoardCaptureMessages";

export interface UseBoardCaptureOptions {
  readonly wsRef: { current: WebSocket | null };
  readonly powerCaptureProtocol: string | undefined;
  readonly setLive: (live: boolean) => void;
  readonly setError: (message: string | null) => void;
}

export interface UseBoardCaptureController {
  readonly captureState: BoardCaptureState;
  readonly captureProgress: BoardCaptureProgress | null;
  readonly captures: PowerCapture[];
  readonly pendingCaptureRef: { current: CaptureConfig | null };
  readonly streamingCaptureRef: { current: StreamingCaptureSession | null };
  readonly handleCaptureMessage: BoardCaptureMessageHandler;
  readonly handleSocketFailure: (message: string) => void;
  readonly handleSessionCleanup: () => void;
  readonly armCapture: (config: CaptureConfig) => Promise<void>;
  readonly triggerCapture: () => void;
  readonly stopCapture: () => void;
  readonly cancelCapture: () => void;
  readonly clearCaptures: () => void;
}

export function useBoardCapture(options: UseBoardCaptureOptions): UseBoardCaptureController {
  const { wsRef, powerCaptureProtocol, setLive, setError } = options;
  const [captureState, setCaptureState] = useState<BoardCaptureState>("idle");
  const [captureProgress, setCaptureProgress] = useState<BoardCaptureProgress | null>(null);
  const [captures, setCaptures] = useState<PowerCapture[]>([]);
  const pendingCaptureRef = useRef<CaptureConfig | null>(null);
  const captureBuilderRef = useRef<LegacyCaptureBuilder | null>(null);
  const streamingCaptureRef = useRef<StreamingCaptureSession | null>(null);
  const captureOwnerRef = useRef(createArchiveId());
  const captureArmPromiseRef = useRef<CaptureArmPromiseSettlement | null>(null);

  useEffect(() => {
    let cancelled = false;
    void recoverStalePowerCaptureArchives().then(() => listRecentPowerCaptures()).then((archived) => {
      if (!cancelled && archived.length > 0) setCaptures(archived);
    }).catch(() => {
      // IndexedDB is an optional browser capability. A recording attempt will
      // surface an actionable error if persistent storage is unavailable.
    });
    return () => {
      cancelled = true;
    };
  }, []);

  const appendCapture = useCallback((capture: PowerCapture) => {
    setCaptures((previous) => [...previous, capture].slice(-4));
  }, []);

  const finalizeStreamingCapture = useCallback((
    incomplete = false,
    interruptionReason?: string,
  ): Promise<void> => finalizeStreamingCaptureArchive({
    streamingCaptureRef,
    socketRef: wsRef,
    onState: setCaptureState,
    onProgress: setCaptureProgress,
    onCapture: appendCapture,
    onError: setError,
  }, incomplete, interruptionReason), [appendCapture, setError, wsRef]);

  const discardStreamingCapture = useCallback(() => {
    discardStreamingCaptureArchive({ streamingCaptureRef, socketRef: wsRef });
  }, [wsRef]);

  const resetCapture = useCallback((reason?: Error) => {
    if (captureArmPromiseRef.current) {
      captureArmPromiseRef.current.reject(reason ?? new Error("Power capture arming was cancelled"));
      captureArmPromiseRef.current = null;
    }
    pendingCaptureRef.current = null;
    captureBuilderRef.current = null;
    discardStreamingCapture();
    setCaptureProgress(null);
    setCaptureState("idle");
  }, [discardStreamingCapture]);

  const armCapture = useCallback(async (config: CaptureConfig) => {
    if (config.streaming) {
      if (powerCaptureProtocol !== POWER_CAPTURE_PROTOCOL) {
        const reported = powerCaptureProtocol ?? "not reported";
        const message = `Power capture requires firmware protocol ${POWER_CAPTURE_PROTOCOL}; the debugger reported ${reported}`;
        setError(message);
        throw new Error(message);
      }
      if (config.rateHz > MAX_WEB_STREAMING_RATE_HZ) {
        const message = `Continuous Web recording is limited to ${MAX_WEB_STREAMING_RATE_HZ} Hz to keep USB telemetry and the control API responsive`;
        setError(message);
        throw new Error(message);
      }
      if (config.preSamples > MAX_POWER_CAPTURE_PRE_TRIGGER_SAMPLES) {
        const message = `Pre-trigger history is limited to ${MAX_POWER_CAPTURE_PRE_TRIGGER_SAMPLES} samples to protect browser memory`;
        setError(message);
        throw new Error(message);
      }
      try {
        await ensurePowerCaptureStorageCapacity({
          rateHz: config.rateHz,
          preSamples: config.preSamples,
          stopAfterMs: config.stopAfterMs,
        });
      } catch (reason) {
        const message = reason instanceof Error ? reason.message : String(reason);
        setError(message);
        throw reason;
      }
    }
    return new Promise<void>((resolve, reject) => {
      captureArmPromiseRef.current?.reject(new Error("Power capture arming was superseded"));
      captureArmPromiseRef.current = { resolve, reject };
      pendingCaptureRef.current = config;
      captureBuilderRef.current = null;
      streamingCaptureRef.current = config.streaming
        ? createStreamingCaptureSession(config, captureOwnerRef.current)
        : null;
      setCaptureProgress(null);
      setCaptureState("connecting");
      setError(null);
      if (wsRef.current?.readyState === WebSocket.OPEN) {
        if (config.streaming) {
          wsRef.current.send(JSON.stringify(liveSubscribeMessage(
            config.rateHz,
            TELEMETRY_STREAM_BATCH_SIZE,
          )));
        }
        wsRef.current.send(JSON.stringify(powerCaptureArmMessage(config)));
      } else {
        setLive(true);
      }
    });
  }, [powerCaptureProtocol, setError, setLive, wsRef]);

  const triggerCapture = useCallback(() => {
    wsRef.current?.send(JSON.stringify({
      type: "command", command: "capture_trigger", id: "web-trigger",
    }));
  }, [wsRef]);

  const stopCapture = useCallback(() => {
    if (streamingCaptureRef.current?.triggered) {
      void finalizeStreamingCapture();
      return;
    }
    wsRef.current?.send(JSON.stringify({
      type: "command", command: "capture_stop", id: "web-stop",
    }));
  }, [finalizeStreamingCapture, wsRef]);

  const cancelCapture = useCallback(() => {
    if (streamingCaptureRef.current) {
      wsRef.current?.send(JSON.stringify({
        type: "command", command: "capture_cancel", id: "web-cancel",
      }));
    }
    resetCapture(new Error("Power capture was cancelled"));
  }, [resetCapture, wsRef]);

  const clearCaptures = useCallback(() => {
    const activeArchiveId = streamingCaptureRef.current?.archiveId;
    void clearPowerCaptureArchives({ activeArchiveId }).then(() => listRecentPowerCaptures()).then(
      setCaptures,
    ).catch((reason) => {
      setError(reason instanceof Error ? reason.message : String(reason));
    });
  }, [setError]);

  const handleSocketFailure = useCallback((message: string) => {
    if (streamingCaptureRef.current?.triggered) {
      void finalizeStreamingCapture(true, message);
    } else {
      resetCapture(new Error(message));
    }
  }, [finalizeStreamingCapture, resetCapture]);

  const handleSessionCleanup = useCallback(() => {
    if (streamingCaptureRef.current?.triggered) {
      void finalizeStreamingCapture(true, "Live session ended before the recording stopped");
    } else if (!streamingCaptureRef.current?.finishing) {
      resetCapture();
    }
  }, [finalizeStreamingCapture, resetCapture]);

  const handleCaptureMessage = useMemo(
    () => createBoardCaptureMessageHandler({
      pendingCaptureRef,
      captureBuilderRef,
      streamingCaptureRef,
      captureArmPromiseRef,
      setCaptureState,
      setCaptureProgress,
      onCapture: appendCapture,
      setError,
      finalizeStreamingCapture,
      resetCapture,
    }),
    [appendCapture, finalizeStreamingCapture, resetCapture, setError],
  );

  return {
    captureState,
    captureProgress,
    captures,
    pendingCaptureRef,
    streamingCaptureRef,
    handleCaptureMessage,
    handleSocketFailure,
    handleSessionCleanup,
    armCapture,
    triggerCapture,
    stopCapture,
    cancelCapture,
    clearCaptures,
  };
}
