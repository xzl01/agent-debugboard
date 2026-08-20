import { useCallback, useEffect, useRef, useState } from "react";
import {
  formatSigrokErrorMessage,
  isSigrokDataFrame,
  isSigrokErrorFrame,
  SigrokClient,
  SigrokEventCode,
  SigrokFrameType,
  type SigrokAck,
  type SigrokCapsResp,
  type SigrokClientEvent,
  type SigrokClientState,
  type SigrokConfigReq,
  type SigrokDataMeta,
  type SigrokEvent,
  type SigrokFrame,
  type SigrokServerCapabilities,
} from "@/lib/sigrokClient";
import {
  createLiveSession,
  deleteLiveSession,
  liveWebSocketUrl,
  type LiveSession,
} from "@/lib/api";

export interface UseSigrokScopeOptions {
  url?: string;
}

export interface UseSigrokScopeReturn {
  state: SigrokClientState;
  ensureConnected: () => Promise<void>;
  close: () => void;
  configure: (config: SigrokConfigReq) => Promise<SigrokAck>;
  start: () => Promise<SigrokAck>;
  stop: () => Promise<SigrokAck>;
  getCaps: () => Promise<SigrokCapsResp>;
  getServerCapabilities: () => SigrokServerCapabilities;
  readData: (timeoutMs?: number) => Promise<{ meta: SigrokDataMeta; samples: Uint8Array } | null>;
  readCaptureFrame: (timeoutMs?: number) => Promise<SigrokCaptureFrame | null>;
  waitForTriggered: (timeoutMs?: number) => Promise<boolean>;
  error: string | null;
  clearError: () => void;
}

type SigrokCaptureFrame = Extract<
  SigrokFrame,
  | { type: typeof SigrokFrameType.DATA }
  | { type: typeof SigrokFrameType.EVENT }
  | { type: typeof SigrokFrameType.ERROR }
>;

function isCaptureFrame(frame: SigrokFrame): frame is SigrokCaptureFrame {
  return (
    frame.type === SigrokFrameType.DATA ||
    frame.type === SigrokFrameType.EVENT ||
    frame.type === SigrokFrameType.ERROR
  );
}

export function useSigrokScope(options: UseSigrokScopeOptions = {}): UseSigrokScopeReturn {
  const { url } = options;
  const clientRef = useRef<SigrokClient | null>(null);
  const [state, setState] = useState<SigrokClientState>("disconnected");
  const [error, setError] = useState<string | null>(null);
  const frameQueueRef = useRef<SigrokCaptureFrame[]>([]);
  const triggerQueueRef = useRef<SigrokEvent[]>([]);
  const frameResolveRef = useRef<((frame: SigrokCaptureFrame | null) => void) | null>(null);
  const eventResolveRef = useRef<((triggered: boolean) => void) | null>(null);
  const sessionRef = useRef<LiveSession | null>(null);
  const sessionCreationRef = useRef<{
    generation: number;
    promise: Promise<LiveSession>;
  } | null>(null);
  const connectionRef = useRef<Promise<void> | null>(null);
  const connectionGenerationRef = useRef(0);

  const warnCleanup = useCallback((action: string, cleanupError: unknown) => {
    const message = cleanupError instanceof Error ? cleanupError.message : String(cleanupError);
    console.warn(`[useSigrokScope] ${action} failed: ${message}`);
  }, []);

  const deleteSessionBestEffort = useCallback(async (sessionId: number, action: string) => {
    try {
      await deleteLiveSession(sessionId);
    } catch (cleanupError) {
      // Firmware releases a live-session slot when its WebSocket closes. A
      // subsequent DELETE is therefore an idempotent cleanup boundary, not a
      // user-visible failure.
      const code = typeof cleanupError === "object" && cleanupError != null && "code" in cleanupError
        ? String(cleanupError.code)
        : "";
      if (code !== "unknown_session_id") {
        warnCleanup(action, cleanupError);
      }
    }
  }, [warnCleanup]);

  const clearError = useCallback(() => setError(null), []);

  const getClient = useCallback(() => {
    if (!clientRef.current) {
      clientRef.current = new SigrokClient();
    }
    return clientRef.current;
  }, []);

  const getServerCapabilities = useCallback((): SigrokServerCapabilities => {
    return getClient().getServerCapabilities();
  }, [getClient]);

  const releaseSession = useCallback(async () => {
    if (url) {
      return;
    }

    const session = sessionRef.current;
    if (session == null) {
      return;
    }

    sessionRef.current = null;
    await deleteSessionBestEffort(
      session.session_id,
      `deleting live session ${session.session_id}`,
    );
  }, [deleteSessionBestEffort, url]);

  const createSession = useCallback(async (generation: number): Promise<string> => {
    if (url) {
      return url;
    }
    if (sessionRef.current) {
      return liveWebSocketUrl(sessionRef.current.ws_url);
    }

    let creation = sessionCreationRef.current;
    if (creation == null || creation.generation !== generation) {
      creation = { generation, promise: createLiveSession() };
      sessionCreationRef.current = creation;
    }

    let session: LiveSession;
    try {
      session = await creation.promise;
    } finally {
      if (sessionCreationRef.current === creation) {
        sessionCreationRef.current = null;
      }
    }
    if (connectionGenerationRef.current !== generation) {
      await deleteSessionBestEffort(
        session.session_id,
        `deleting cancelled live session ${session.session_id}`,
      );
      throw new Error("Connection cancelled");
    }
    sessionRef.current = session;
    return liveWebSocketUrl(session.ws_url);
  }, [deleteSessionBestEffort, url]);

  const ensureConnected = useCallback(async () => {
    const client = getClient();
    const clientState = client.getState();
    if (
      clientState === "ready" ||
      clientState === "configured" ||
      clientState === "armed" ||
      clientState === "running"
    ) {
      return;
    }
    if (connectionRef.current != null) {
      return connectionRef.current;
    }

    const generation = connectionGenerationRef.current;
    const connection = (async () => {
      try {
        setError(null);
        const wsUrl = await createSession(generation);
        if (connectionGenerationRef.current !== generation) {
          throw new Error("Connection cancelled");
        }
        await client.connect(wsUrl);
        if (connectionGenerationRef.current !== generation) {
          client.disconnect();
          throw new Error("Connection cancelled");
        }
      } catch (err) {
        if (connectionGenerationRef.current === generation) {
          await releaseSession();
          setError(err instanceof Error ? err.message : "Connection failed");
        }
        throw err;
      }
    })();
    connectionRef.current = connection;
    try {
      await connection;
    } finally {
      if (connectionRef.current === connection) {
        connectionRef.current = null;
      }
    }
  }, [createSession, getClient, releaseSession]);

  const resetQueues = useCallback(() => {
    frameQueueRef.current = [];
    triggerQueueRef.current = [];
  }, []);

  const close = useCallback(() => {
    connectionGenerationRef.current += 1;
    connectionRef.current = null;
    const client = getClient();
    client.disconnect();
    resetQueues();
    if (frameResolveRef.current) {
      frameResolveRef.current(null);
      frameResolveRef.current = null;
    }
    if (eventResolveRef.current) {
      eventResolveRef.current(false);
      eventResolveRef.current = null;
    }
    void releaseSession();
  }, [getClient, releaseSession, resetQueues]);

  const configure = useCallback(async (config: SigrokConfigReq): Promise<SigrokAck> => {
    const client = getClient();
    try {
      setError(null);
      return await client.configure(config);
    } catch (err) {
      setError(err instanceof Error ? err.message : "Configuration failed");
      throw err;
    }
  }, [getClient]);

  const start = useCallback(async (): Promise<SigrokAck> => {
    const client = getClient();
    try {
      setError(null);
      return await client.start();
    } catch (err) {
      setError(err instanceof Error ? err.message : "Start failed");
      throw err;
    }
  }, [getClient]);

  const stop = useCallback(async (): Promise<SigrokAck> => {
    const client = getClient();
    try {
      setError(null);
      return await client.stop();
    } catch (err) {
      setError(err instanceof Error ? err.message : "Stop failed");
      throw err;
    }
  }, [getClient]);

  const getCaps = useCallback(async (): Promise<SigrokCapsResp> => {
    const client = getClient();
    try {
      setError(null);
      return await client.getCaps();
    } catch (err) {
      setError(err instanceof Error ? err.message : "Get caps failed");
      throw err;
    }
  }, [getClient]);

  const readCaptureFrame = useCallback(async (timeoutMs: number = 5000): Promise<SigrokCaptureFrame | null> => {
    const queued = frameQueueRef.current.shift();
    if (queued) {
      return queued;
    }

    return new Promise((resolve) => {
      const timeout = setTimeout(() => {
        frameResolveRef.current = null;
        resolve(null);
      }, timeoutMs);

      frameResolveRef.current = (frame) => {
        clearTimeout(timeout);
        frameResolveRef.current = null;
        resolve(frame);
      };
    });
  }, []);

  const readData = useCallback(async (timeoutMs: number = 5000): Promise<{ meta: SigrokDataMeta; samples: Uint8Array } | null> => {
    const deadline = Date.now() + timeoutMs;

    while (true) {
      const remainingMs = Math.max(0, deadline - Date.now());
      if (remainingMs === 0) {
        return null;
      }

      const frame = await readCaptureFrame(remainingMs);
      if (frame == null) {
        return null;
      }
      if (isSigrokDataFrame(frame)) {
        return { meta: frame.meta, samples: frame.samples };
      }
      if (isSigrokErrorFrame(frame)) {
        throw new Error(formatSigrokErrorMessage(frame.payload));
      }
    }
  }, [readCaptureFrame]);

  const waitForTriggered = useCallback(async (timeoutMs: number = 10000): Promise<boolean> => {
    if (triggerQueueRef.current.some((event) => event.typeDetail === SigrokEventCode.TRIGGERED)) {
      triggerQueueRef.current = [];
      return true;
    }

    return new Promise((resolve) => {
      const timeout = setTimeout(() => {
        eventResolveRef.current = null;
        resolve(false);
      }, timeoutMs);

      eventResolveRef.current = (triggered) => {
        clearTimeout(timeout);
        eventResolveRef.current = null;
        resolve(triggered);
      };
    });
  }, []);

  useEffect(() => {
    const client = getClient();

    const handleEvent = (event: SigrokClientEvent) => {
      if (event.type === "state") {
        setState(event.state);
        if (event.state === "disconnected") {
          void releaseSession();
        }
      } else if (event.type === "error") {
        setError(event.message);
      } else if (event.type === "frame") {
        if (isCaptureFrame(event.frame)) {
          if (frameResolveRef.current) {
            frameResolveRef.current(event.frame);
          } else {
            frameQueueRef.current.push(event.frame);
          }
        }

        if (event.frame.type === SigrokFrameType.EVENT) {
          const ev = event.frame.payload;
          if (eventResolveRef.current) {
            if (ev.typeDetail === SigrokEventCode.TRIGGERED) {
              eventResolveRef.current(true);
            }
          } else {
            triggerQueueRef.current.push(ev);
          }
        }
      }
    };

    client.addEventListener(handleEvent);

    return () => {
      connectionGenerationRef.current += 1;
      connectionRef.current = null;
      client.removeEventListener(handleEvent);
      client.disconnect();
      resetQueues();
      if (frameResolveRef.current) {
        frameResolveRef.current(null);
        frameResolveRef.current = null;
      }
      if (eventResolveRef.current) {
        eventResolveRef.current(false);
        eventResolveRef.current = null;
      }
      void releaseSession();
    };
  }, [getClient, releaseSession, resetQueues]);

  return {
    state,
    ensureConnected,
    close,
    configure,
    start,
    stop,
    getCaps,
    getServerCapabilities,
    readData,
    readCaptureFrame,
    waitForTriggered,
    error,
    clearError,
  };
}
