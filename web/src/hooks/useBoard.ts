import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { Dispatch, SetStateAction } from "react";
import * as api from "@/lib/api";
import { parseHttpAdcReadings } from "@/lib/adc";
import {
  EMPTY_BOARD_SNAPSHOT,
  mapBoardStatus,
} from "@/lib/boardSnapshot";
import { persistentConfigCurrentStateKey } from "@/lib/persistentConfigCurrentStateKey";
import type { BoardSnapshot } from "@/lib/types";

import { useBoardCapture } from "./useBoardCapture";
import type { BoardCaptureMessageHandler } from "./useBoardCaptureMessages";
import { useBoardLive } from "./useBoardLive";
import { useBoardMutations } from "./useBoardMutations";
import type { UseBoard as UseBoardBase } from "./useBoard.types";

export interface UseBoard extends UseBoardBase {
  lastVerifiedAt: number | null;
}

export function useBoard(): UseBoard {
  const [snapshot, setSnapshot] = useState<BoardSnapshot>(EMPTY_BOARD_SNAPSHOT);
  const [hasData, setHasData] = useState(false);
  const [connected, setConnected] = useState(true);
  const [lastVerifiedAt, setLastVerifiedAt] = useState<number | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const [auto, setAuto] = useState(true);
  const [live, setLive] = useState(false);
  const [pageVisible, setPageVisible] = useState(
    () => typeof document === "undefined" || !document.hidden
  );
  const wsRef = useRef<WebSocket | null>(null);
  const lastVerifiedAtRef = useRef<number | null>(null);
  const currentStateKey = useMemo(
    () => persistentConfigCurrentStateKey({
      powerOutputs: snapshot.powerOutputs,
      switches: snapshot.switches,
      gpios: snapshot.gpios,
    }),
    [snapshot.gpios, snapshot.powerOutputs, snapshot.switches]
  );

  // Every successful HTTP poll or inbound WebSocket frame proves the board is
  // reachable; stamp it so the UI can show when the state was last verified.
  const markVerified = useCallback(() => {
    const verifiedAt = Date.now();
    lastVerifiedAtRef.current = verifiedAt;
    setLastVerifiedAt(verifiedAt);
  }, []);

  const {
    captureState,
    captureProgress,
    captures,
    pendingCaptureRef,
    handleCaptureMessage,
    handleSocketFailure,
    handleSessionCleanup,
    armCapture,
    triggerCapture,
    stopCapture,
    cancelCapture,
    clearCaptures,
  } = useBoardCapture({
    wsRef,
    powerCaptureProtocol: snapshot.powerCaptureProtocol,
    setLive,
    setError,
  });

  // Telemetry frames only flow through the capture handler, so stamp verified
  // state there as well; snapshot frames are covered by setSnapshot below.
  const handleCaptureMessageVerified = useCallback<BoardCaptureMessageHandler>(
    (msg, samples, now) => {
      markVerified();
      return handleCaptureMessage(msg, samples, now);
    },
    [handleCaptureMessage, markVerified]
  );

  const handleSocketFailureDisconnect = useCallback(
    (message: string) => {
      setConnected(false);
      handleSocketFailure(message);
    },
    [handleSocketFailure]
  );

  const setSnapshotVerified = useCallback<Dispatch<SetStateAction<BoardSnapshot>>>(
    (value) => {
      markVerified();
      setSnapshot(value);
    },
    [markVerified]
  );

  useEffect(() => {
    if (typeof document === "undefined") return;
    const updateVisibility = () => {
      // Do not interrupt an armed or active capture just because its tab was
      // backgrounded. Once the capture is idle, the hidden tab can release its
      // polling/WebSocket load and reconnect when it becomes visible again.
      setPageVisible(!document.hidden || captureState !== "idle");
    };
    updateVisibility();
    document.addEventListener("visibilitychange", updateVisibility);
    return () => document.removeEventListener("visibilitychange", updateVisibility);
  }, [captureState]);

  const refresh = useCallback(async () => {
    try {
      const status = await api.getStatus();
      const adcResponse: unknown = await api.getAdc();
      const readings = parseHttpAdcReadings(adcResponse);
      setSnapshot(mapBoardStatus(status, readings));
      setHasData(true);
      setConnected(true);
      markVerified();
      setError(null);
    } catch (e) {
      setConnected(false);
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setLoading(false);
    }
  }, [markVerified]);

  useBoardLive({
    live,
    pageVisible,
    auto,
    refresh,
    wsRef,
    pendingCaptureRef,
    setSnapshot: setSnapshotVerified,
    setHasData,
    setConnected,
    setError,
    setLoading,
    setLive,
    handleCaptureMessage: handleCaptureMessageVerified,
    handleSocketFailure: handleSocketFailureDisconnect,
    handleSessionCleanup,
  });

  const {
    setPower,
    readPower,
    setSwitch,
    setGpio,
    enterBootloader,
  } = useBoardMutations({ live, refresh });

  return {
    snapshot,
    persistentConfigCurrentStateKey: currentStateKey,
    hasData,
    connected,
    lastVerifiedAt,
    error,
    loading,
    auto,
    setAuto,
    live,
    setLive,
    refresh,
    setPower,
    readPower,
    setSwitch,
    enterBootloader,
    setGpio,
    captureState,
    captureProgress,
    captures,
    armCapture,
    triggerCapture,
    stopCapture,
    cancelCapture,
    clearCaptures,
  };
}
