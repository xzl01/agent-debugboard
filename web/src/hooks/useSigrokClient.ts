import { useCallback, useEffect, useRef, useState } from "react";
import { SigrokClient, type SigrokClientEvent, type SigrokClientState, type SigrokConfigReq, type SigrokAck, type SigrokCapsResp } from "@/lib/sigrokClient";
import { liveWebSocketUrl } from "@/lib/api";

export interface UseSigrokClientOptions {
  url?: string;
  autoConnect?: boolean;
}

export interface UseSigrokClientReturn {
  state: SigrokClientState;
  connect: () => Promise<void>;
  disconnect: () => void;
  configure: (config: SigrokConfigReq) => Promise<SigrokAck>;
  start: () => Promise<SigrokAck>;
  stop: () => Promise<SigrokAck>;
  getCaps: () => Promise<SigrokCapsResp>;
  error: string | null;
  clearError: () => void;
}

export function useSigrokClient(options: UseSigrokClientOptions = {}): UseSigrokClientReturn {
  const { url = "/api/v1/ws/0", autoConnect = false } = options;
  const clientRef = useRef<SigrokClient | null>(null);
  const [state, setState] = useState<SigrokClientState>("disconnected");
  const [error, setError] = useState<string | null>(null);

  const clearError = useCallback(() => setError(null), []);

  const getClient = useCallback(() => {
    if (!clientRef.current) {
      clientRef.current = new SigrokClient();
    }
    return clientRef.current;
  }, []);

  const connect = useCallback(async () => {
      const client = getClient();
      try {
        setError(null);
        await client.connect(liveWebSocketUrl(url));
      } catch (err) {
        setError(err instanceof Error ? err.message : "Connection failed");
        throw err;
    }
  }, [getClient, url]);

  const disconnect = useCallback(() => {
    const client = getClient();
    client.disconnect();
  }, [getClient]);

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

  useEffect(() => {
    const client = getClient();

    const handleEvent = (event: SigrokClientEvent) => {
      if (event.type === "state") {
        setState(event.state);
      } else if (event.type === "error") {
        setError(event.message);
      }
    };

    client.addEventListener(handleEvent);

    if (autoConnect) {
      connect().catch((connectError) => {
        const message = connectError instanceof Error ? connectError.message : String(connectError);
        console.warn(`[useSigrokClient] auto-connect failed: ${message}`);
      });
    }

    return () => {
      client.removeEventListener(handleEvent);
      client.disconnect();
    };
  }, [getClient, autoConnect, connect]);

  return {
    state,
    connect,
    disconnect,
    configure,
    start,
    stop,
    getCaps,
    error,
    clearError,
  };
}
