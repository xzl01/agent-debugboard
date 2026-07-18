import { useCallback, useRef, useState } from "react";

import { liveWebSocketUrl } from "@/lib/api";
import { ScpiStreamReader, type ScpiEvent } from "@/lib/scpiScope";

const DEFAULT_QUERY_TIMEOUT_MS = 4000;
const FRAME_TIMEOUT_MS = 20000;

export function useScpiScope() {
  const wsRef = useRef<WebSocket | null>(null);
  const openRef = useRef<Promise<void> | null>(null);
  const readerRef = useRef(new ScpiStreamReader());
  const eventsRef = useRef<ScpiEvent[]>([]);
  const waitersRef = useRef<(() => void)[]>([]);
  const chainRef = useRef<Promise<unknown>>(Promise.resolve());
  const [connected, setConnected] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const notify = useCallback((events: ScpiEvent[]) => {
    if (events.length === 0) return;
    eventsRef.current.push(...events);
    const waiters = waitersRef.current.splice(0);
    for (const wake of waiters) wake();
  }, []);

  const close = useCallback(() => {
    const ws = wsRef.current;
    wsRef.current = null;
    openRef.current = null;
    setConnected(false);
    if (ws && ws.readyState === WebSocket.OPEN) {
      try {
        ws.send(":STOP\n");
      } catch {
        /* socket already unusable */
      }
    }
    ws?.close();
  }, []);

  const ensureConnected = useCallback(async () => {
    if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) return;
    if (openRef.current) return openRef.current;

    const open = new Promise<void>((resolve, reject) => {
      const ws = new WebSocket(liveWebSocketUrl("/api/v1/scpi"));
      ws.binaryType = "arraybuffer";
      ws.onopen = () => {
        wsRef.current = ws;
        setConnected(true);
        setError(null);
        resolve();
      };
      ws.onerror = () => {
        reject(new Error("scpi websocket connect failed"));
      };
      ws.onclose = () => {
        if (wsRef.current === ws) {
          wsRef.current = null;
          setConnected(false);
        }
        notify([{ type: "line", text: "" }]);
      };
      ws.onmessage = (msg) => {
        const data = msg.data;
        if (data instanceof ArrayBuffer) {
          notify(readerRef.current.feed(new Uint8Array(data)));
        } else if (typeof data === "string") {
          notify(readerRef.current.feed(new TextEncoder().encode(data)));
        }
      };
    });

    openRef.current = open;
    try {
      await open;
    } finally {
      openRef.current = null;
    }
  }, [notify]);

  const nextEvent = useCallback(
    (predicate: (ev: ScpiEvent) => boolean, timeoutMs: number): Promise<ScpiEvent> => {
      const queued = eventsRef.current.findIndex(predicate);
      if (queued >= 0) {
        return Promise.resolve(eventsRef.current.splice(queued, 1)[0]);
      }
      return new Promise<ScpiEvent>((resolve, reject) => {
        const timer = window.setTimeout(() => {
          const idx = waitersRef.current.indexOf(wake);
          if (idx >= 0) waitersRef.current.splice(idx, 1);
          reject(new Error("scpi response timeout"));
        }, timeoutMs);
        const wake = () => {
          window.clearTimeout(timer);
          const idx = eventsRef.current.findIndex(predicate);
          if (idx >= 0) {
            resolve(eventsRef.current.splice(idx, 1)[0]);
          } else {
            waitersRef.current.push(wake);
          }
        };
        waitersRef.current.push(wake);
      });
    },
    []
  );

  const exclusive = useCallback(<T,>(fn: () => Promise<T>): Promise<T> => {
    const run = chainRef.current.then(fn, fn);
    chainRef.current = run.catch(() => undefined);
    return run;
  }, []);

  const command = useCallback(
    (cmd: string) =>
      exclusive(async () => {
        await ensureConnected();
        wsRef.current?.send(cmd + "\n");
      }),
    [ensureConnected, exclusive]
  );

  const query = useCallback(
    (cmd: string, timeoutMs = DEFAULT_QUERY_TIMEOUT_MS) =>
      exclusive(async () => {
        await ensureConnected();
        wsRef.current?.send(cmd + "\n");
        const ev = await nextEvent((e) => e.type === "line", timeoutMs);
        return ev.type === "line" ? ev.text : "";
      }),
    [ensureConnected, exclusive, nextEvent]
  );

  const readDigitalFrame = useCallback(
    (timeoutMs = FRAME_TIMEOUT_MS) =>
      exclusive(async () => {
        await ensureConnected();
        wsRef.current?.send(":WAV:DATA? DIG\n");
        const ev = await nextEvent((e) => e.type === "block", timeoutMs);
        if (ev.type !== "block") throw new Error("scpi frame type mismatch");
        const payload = ev.payload;
        const out = new Uint16Array(payload.byteLength / 2);
        for (let i = 0; i < out.length; i++) {
          out[i] = payload[i * 2] | (payload[i * 2 + 1] << 8);
        }
        return out;
      }),
    [ensureConnected, exclusive, nextEvent]
  );

  const readDeepData = useCallback(
    (offset: number, count: number, timeoutMs = FRAME_TIMEOUT_MS) =>
      exclusive(async () => {
        await ensureConnected();
        wsRef.current?.send(`:LINKR:DEEP:DATA? ${offset} ${count}\n`);
        const ev = await nextEvent((e) => e.type === "block", timeoutMs);
        if (ev.type !== "block") throw new Error("scpi deep data type mismatch");
        const payload = ev.payload;
        const out = new Uint16Array(payload.byteLength / 2);
        for (let i = 0; i < out.length; i++) {
          out[i] = payload[i * 2] | (payload[i * 2 + 1] << 8);
        }
        return out;
      }),
    [ensureConnected, exclusive, nextEvent]
  );

  return {
    connected,
    error,
    setError,
    ensureConnected,
    close,
    command,
    query,
    readDigitalFrame,
    readDeepData,
  };
}
