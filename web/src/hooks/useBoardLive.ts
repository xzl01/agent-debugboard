import { useEffect } from "react";
import type { Dispatch, SetStateAction } from "react";
import * as api from "@/lib/api";
import { parseHttpAdcReadings } from "@/lib/adc";
import {
  mapBoardStatus,
  mergeBoardWsSnapshot,
} from "@/lib/boardSnapshot";
import {
  decodeTelemetryReadings,
  decodeTelemetrySamples,
} from "@/lib/boardTelemetry";
import {
  defaultLiveSubscribeMessage,
  liveSubscribeMessage,
  TELEMETRY_STREAM_BATCH_SIZE,
} from "@/lib/liveSubscribe";
import { powerCaptureArmMessage } from "@/lib/powerCaptureWire";
import { createTelemetryFrameScheduler } from "@/lib/telemetryFrameScheduler";
import type {
  AdcReading,
  BoardSnapshot,
  CaptureConfig,
} from "@/lib/types";

import type { BoardCaptureMessageHandler } from "./useBoardCaptureMessages";

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

function parseLiveMessage(data: unknown): unknown {
  if (typeof data !== "string") return null;
  try {
    return JSON.parse(data);
  } catch {
    return null;
  }
}

export interface UseBoardLiveOptions {
  readonly live: boolean;
  readonly pageVisible: boolean;
  readonly auto: boolean;
  readonly refresh: () => Promise<void>;
  readonly wsRef: { current: WebSocket | null };
  readonly pendingCaptureRef: { current: CaptureConfig | null };
  readonly setSnapshot: Dispatch<SetStateAction<BoardSnapshot>>;
  readonly setHasData: (hasData: boolean) => void;
  readonly setConnected: (connected: boolean) => void;
  readonly setError: (message: string | null) => void;
  readonly setLoading: (loading: boolean) => void;
  readonly setLive: (live: boolean) => void;
  readonly handleCaptureMessage: BoardCaptureMessageHandler;
  readonly handleSocketFailure: (message: string) => void;
  readonly handleSessionCleanup: () => void;
}

export function useBoardLive(options: UseBoardLiveOptions): void {
  const {
    live,
    pageVisible,
    auto,
    refresh,
    wsRef,
    pendingCaptureRef,
    setSnapshot,
    setHasData,
    setConnected,
    setError,
    setLoading,
    setLive,
    handleCaptureMessage,
    handleSocketFailure,
    handleSessionCleanup,
  } = options;

  // Polling mode (default) or WebSocket live mode.
  useEffect(() => {
    if (live && pageVisible) {
      let ws: WebSocket | null = null;
      let cancelled = false;
      let sessionId: number | null = null;
      let sessionReleased = false;

      // The live preview republishes the newest ADC readings once per
      // animation frame. Capture ingestion below stays immediate and never
      // routes through this scheduler, so recording remains lossless.
      const previewScheduler = createTelemetryFrameScheduler<readonly AdcReading[]>((readings) => {
        setSnapshot((prev) => ({ ...prev, adc: readings }));
      });
      const cancelPreviewWhenHidden = () => {
        if (document.hidden) previewScheduler.cancel();
      };
      if (typeof document !== "undefined") {
        document.addEventListener("visibilitychange", cancelPreviewWhenHidden);
      }

      const releaseSession = async () => {
        if (sessionId == null || sessionReleased) return;
        sessionReleased = true;
        try {
          await api.deleteLiveSession(sessionId);
        } catch {
          // The firmware also releases the session when the socket closes, so
          // a follow-up DELETE can legitimately return unknown_session_id.
        }
      };

      (async () => {
        // Seed metadata from an HTTP poll before switching to live stream.
        try {
          const status = await api.getStatus();
          const adcResponse: unknown = await api.getAdc();
          if (cancelled) return;
          setSnapshot(mapBoardStatus(status, parseHttpAdcReadings(adcResponse)));
          setHasData(true);
          setConnected(true);
          setError(null);
          setLoading(false);
        } catch (e) {
          setLive(false);
          setConnected(false);
          setError(e instanceof Error ? e.message : String(e));
          return;
        }

        let session: api.LiveSession;
        try {
          session = await api.createLiveSession();
          sessionId = session.session_id;
          if (cancelled) {
            await releaseSession();
            return;
          }
        } catch (e) {
          if (!cancelled) {
            setLive(false);
            setError(e instanceof Error ? e.message : String(e));
          }
          return;
        }

        // Use the session path returned by firmware, but keep the browser
        // connection same-origin so Vite's WebSocket proxy and deployed pages
        // both work without hard-coding the board address.
        ws = new WebSocket(api.liveWebSocketUrl(session.ws_url));
        wsRef.current = ws;
        ws.onopen = () => {
          const pending = pendingCaptureRef.current;
          ws?.send(JSON.stringify(
            pending?.streaming
              ? liveSubscribeMessage(pending.rateHz, TELEMETRY_STREAM_BATCH_SIZE)
              : defaultLiveSubscribeMessage(),
          ));
          if (pendingCaptureRef.current) {
            ws?.send(JSON.stringify(powerCaptureArmMessage(pendingCaptureRef.current)));
          }
        };
        ws.onmessage = (ev) => {
          if (cancelled) return;
          const parsed = parseLiveMessage(ev.data);
          if (!isRecord(parsed)) return;
          try {
            const msg = parsed;
            if (msg.type === "snapshot") {
              setSnapshot((prev) => mergeBoardWsSnapshot(prev, msg));
            } else if (msg.type === "telemetry" || msg.type === "telemetry-batch") {
              const samples = decodeTelemetrySamples(msg);
              const latestReadings = decodeTelemetryReadings(msg);
              const now = performance.now();
              // Hidden tabs keep an armed/active capture connected and
              // lossless; only the visual preview is suppressed there.
              if (latestReadings.length > 0 && !document.hidden) {
                previewScheduler.schedule(latestReadings);
              }
              handleCaptureMessage(msg, samples, now);
            } else {
              handleCaptureMessage(msg, [], 0);
            }
          } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            handleSocketFailure(`Live WebSocket frame handling failed: ${message}`);
          }
        };
        ws.onerror = () => {
          if (!cancelled) {
            handleSocketFailure("Live WebSocket error");
            setError("Live WebSocket error");
          }
        };
        ws.onclose = () => {
          previewScheduler.cancel();
          if (wsRef.current === ws) wsRef.current = null;
          void releaseSession();
          if (!cancelled) {
            handleSocketFailure("Live WebSocket disconnected");
            setLive(false);
            setError("Live WebSocket disconnected");
          }
        };
      })();

      return () => {
        cancelled = true;
        previewScheduler.cancel();
        if (typeof document !== "undefined") {
          document.removeEventListener("visibilitychange", cancelPreviewWhenHidden);
        }
        handleSessionCleanup();
        if (wsRef.current === ws) wsRef.current = null;
        if (ws && ws.readyState < WebSocket.CLOSING) {
          ws.close();
        } else {
          void releaseSession();
        }
      };
    }

    if (!pageVisible) {
      setLoading(false);
      return;
    }
    setLoading(true);
    refresh();
    if (!auto) return;
    const id = setInterval(refresh, 2000);
    return () => clearInterval(id);
  }, [
    auto,
    handleCaptureMessage,
    handleSessionCleanup,
    handleSocketFailure,
    live,
    pageVisible,
    pendingCaptureRef,
    refresh,
  ]);
}
