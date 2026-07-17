import { useCallback, useEffect, useRef, useState } from "react";
import { apiEndpoint, liveWebSocketUrl } from "@/lib/api";

export interface LogicChunkFrame {
  seq: number;
  count: number;
  values: Uint16Array;
}

interface LogicStreamSession {
  sessionId: number;
  wsUrl: string;
  ws: WebSocket;
}

export function useLogicStream() {
  const [streaming, setStreaming] = useState(false);
  const [streamRate, setStreamRate] = useState<number | null>(null);
  const [streamError, setStreamError] = useState<string | null>(null);
  const sessionRef = useRef<LogicStreamSession | null>(null);
  const chunkCallbackRef = useRef<((chunk: LogicChunkFrame) => void) | null>(null);

  const stopStream = useCallback(async () => {
    const session = sessionRef.current;
    sessionRef.current = null;

    if (session) {
      try {
        session.ws.close();
      } catch {
        /* ignore */
      }
      try {
        await fetch(`${apiEndpoint()}/live-sessions/${session.sessionId}`, {
          method: "DELETE",
        });
      } catch {
        /* ignore */
      }
    }

    try {
      await fetch(`${apiEndpoint()}/logic-analyzer/stream`, { method: "DELETE" });
    } catch {
      /* ignore */
    }

    setStreaming(false);
    setStreamRate(null);
  }, []);

  const startStream = useCallback(
    async (params: {
      sampleRateHz: number;
      selectedPins: number[];
      pinCount: number;
      pinBase: number;
    }) => {
      setStreamError(null);

      const startRes = await fetch(`${apiEndpoint()}/logic-analyzer/stream`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          sample_rate_hz: params.sampleRateHz,
          selected_pins: params.selectedPins,
          pin_count: params.pinCount,
          pin_base: params.pinBase,
        }),
      });

      if (!startRes.ok) {
        const err = await startRes.json().catch(() => null);
        const message = err?.error?.message ?? `Stream start failed: HTTP ${startRes.status}`;
        setStreamError(message);
        return;
      }

      const startData = await startRes.json();
      setStreamRate(startData.actualSampleRateHz ?? params.sampleRateHz);

      const sessionRes = await fetch(`${apiEndpoint()}/live-sessions`, { method: "POST" });
      if (!sessionRes.ok) {
        setStreamError("Failed to create live session");
        await fetch(`${apiEndpoint()}/logic-analyzer/stream`, { method: "DELETE" });
        return;
      }

      const sessionData = await sessionRes.json();
      const sessionId = sessionData.session_id ?? sessionData.session?.session_id;
      const wsUrl = sessionData.ws_url ?? sessionData.session?.ws_url;

      if (!sessionId || !wsUrl) {
        setStreamError("Invalid session response");
        await fetch(`${apiEndpoint()}/logic-analyzer/stream`, { method: "DELETE" });
        return;
      }

      const ws = new WebSocket(liveWebSocketUrl(wsUrl));
      sessionRef.current = { sessionId, wsUrl, ws };

      ws.onopen = () => {
        ws.send(JSON.stringify({ type: "subscribe", topic: "live", rate_hz: 100, id: "logic-stream" }));
        setStreaming(true);
      };

      ws.onmessage = (ev) => {
        try {
          const msg = JSON.parse(ev.data);
          if (msg.type === "logic-chunk" && Array.isArray(msg.values)) {
            const values = new Uint16Array(msg.values);
            chunkCallbackRef.current?.({
              seq: msg.seq,
              count: msg.count,
              values,
            });
          }
        } catch {
          /* ignore malformed frames */
        }
      };

      ws.onerror = () => {
        setStreamError("WebSocket stream error");
        void stopStream();
      };

      ws.onclose = () => {
        if (sessionRef.current?.ws === ws) {
          setStreaming(false);
        }
      };
    },
    [stopStream]
  );

  useEffect(() => {
    return () => {
      void stopStream();
    };
  }, [stopStream]);

  const onChunk = useCallback((callback: (chunk: LogicChunkFrame) => void) => {
    chunkCallbackRef.current = callback;
  }, []);

  return { streaming, streamRate, streamError, startStream, stopStream, onChunk };
}