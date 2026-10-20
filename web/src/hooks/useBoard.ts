import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { Dispatch, SetStateAction } from "react";
import * as api from "@/lib/api";
import { parseHttpAdcReadings } from "@/lib/adc";
import {
  EMPTY_BOARD_SNAPSHOT,
  mapBoardStatus,
} from "@/lib/boardSnapshot";
import { persistentConfigCurrentStateKey } from "@/lib/persistentConfigCurrentStateKey";
import { parsePdStatus } from "@/lib/pd";
import type { BoardSnapshot } from "@/lib/types";

import { useBoardCapture } from "./useBoardCapture";
import type { BoardCaptureMessageHandler } from "./useBoardCaptureMessages";
import { useBoardLive } from "./useBoardLive";
import { useBoardMutations } from "./useBoardMutations";
import { useBoardReboot } from "./useBoardReboot";
import type { UseBoard as UseBoardBase } from "./useBoard.types";

export interface UseBoard extends UseBoardBase {
  lastVerifiedAt: number | null;
}

const PD_CAPABILITY_RETRY_MS = 5000;

export function useBoard(): UseBoard {
  const [snapshot, setSnapshot] = useState<BoardSnapshot>(EMPTY_BOARD_SNAPSHOT);
  const [hasData, setHasData] = useState(false);
  const [connected, setConnected] = useState(true);
  const [lastVerifiedAt, setLastVerifiedAt] = useState<number | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const [auto, setAuto] = useState(true);
  const [live, setLive] = useState(false);
  const [documentVisible, setDocumentVisible] = useState(
    () => typeof document === "undefined" || !document.hidden
  );
  const wsRef = useRef<WebSocket | null>(null);
  const lastVerifiedAtRef = useRef<number | null>(null);
  const pdDiscoveryRef = useRef({
    request: null as object | null,
    retryAfter: 0,
  });
  const runtime = snapshot.monitoring.runtime;
  const uptimeMs = !runtime.available ? null : typeof runtime.uptime_ms === "number" ? runtime.uptime_ms
    : typeof runtime.uptime_seconds === "number" ? runtime.uptime_seconds * 1000 : null;
  const { rebootBoard: requestReboot, restarting, restartTimedOut, isExpectedRestartDisconnect, noteConnection } = useBoardReboot(connected, uptimeMs);
  const setBoardConnected = useCallback((online: boolean) => {
    setConnected(online);
    noteConnection(online);
  }, [noteConnection]);
  const setConnectionError = useCallback((message: string | null) => {
    if (message !== null && isExpectedRestartDisconnect()) return;
    setError(message);
  }, [isExpectedRestartDisconnect]);
  const rebootBoard = useCallback(async () => {
    setError(null);
    await requestReboot();
  }, [requestReboot]);
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
  // An active capture may retain its socket in a hidden tab, but optional PD
  // discovery should still pause until the document itself is visible.
  const pageVisible = documentVisible || captureState !== "idle";

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
      setBoardConnected(false);
      if (!isExpectedRestartDisconnect()) handleSocketFailure(message);
    },
    [handleSocketFailure, isExpectedRestartDisconnect, setBoardConnected]
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
      setDocumentVisible(!document.hidden);
    };
    updateVisibility();
    document.addEventListener("visibilitychange", updateVisibility);
    return () => document.removeEventListener("visibilitychange", updateVisibility);
  }, []);

  const pd = snapshot.pd ?? EMPTY_BOARD_SNAPSHOT.pd!;
  useEffect(() => {
    const discovery = pdDiscoveryRef.current;
    if (!pd.available || !pd.pd || pd.last_error !== 0) {
      discovery.retryAfter = 0;
      return;
    }
    if (!connected || !documentVisible || pd.capabilities_valid) return;

    let cancelled = false;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const discover = async () => {
      if (cancelled) return;
      if (discovery.request || Date.now() < discovery.retryAfter) {
        timer = setTimeout(discover, Math.max(discovery.request ? PD_CAPABILITY_RETRY_MS : 0, discovery.retryAfter - Date.now()));
        return;
      }
      const request = {};
      discovery.request = request;
      let successful = false;
      try {
        const full = parsePdStatus(await api.getPd());
        if (cancelled) return;
        if (full.available && full.pd && full.last_error === 0 && full.capabilities_valid) {
          successful = true;
          setSnapshot((prev) => ({
            ...prev,
            pd: parsePdStatus({
              capabilities_valid: true,
              fixed_profiles: full.fixed_profiles,
              pps_ranges: full.pps_ranges,
              avs_ranges: full.avs_ranges,
            }, prev.pd),
          }));
        }
      } catch {
        // Capability discovery is optional: successful status/ADC polls still
        // prove the board is connected even if this slower request fails.
      } finally {
        if (discovery.request === request) {
          discovery.request = null;
          // Explicit invalidation can arrive on every short status read. Even
          // after a successful discovery it must not trigger a 2 s hot loop.
          discovery.retryAfter = Date.now() + PD_CAPABILITY_RETRY_MS;
        }
        if (!cancelled && !successful) timer = setTimeout(discover, PD_CAPABILITY_RETRY_MS);
      }
    };
    void discover();
    return () => {
      cancelled = true;
      clearTimeout(timer);
    };
  }, [connected, documentVisible, pd.available, pd.pd, pd.last_error, pd.capabilities_valid]);

  const refresh = useCallback(async () => {
    try {
      const status = await api.getStatus();
      const adcResponse: unknown = await api.getAdc();
      const readings = parseHttpAdcReadings(adcResponse);
      setSnapshot((prev) => mapBoardStatus(status, readings, prev.pd));
      setHasData(true);
      setBoardConnected(true);
      markVerified();
      setError(null);
    } catch (e) {
      setBoardConnected(false);
      setConnectionError(e instanceof Error ? e.message : String(e));
    } finally {
      setLoading(false);
    }
  }, [markVerified, setBoardConnected, setConnectionError]);

  useBoardLive({
    live,
    pageVisible,
    auto,
    refresh,
    wsRef,
    pendingCaptureRef,
    setSnapshot: setSnapshotVerified,
    setHasData,
    setConnected: setBoardConnected,
    setError: setConnectionError,
    setLoading,
    setLive,
    handleCaptureMessage: handleCaptureMessageVerified,
    handleSocketFailure: handleSocketFailureDisconnect,
    handleSessionCleanup,
  });

  const {
    setPower,
    setPd,
    readPower,
    setSwitch,
    setGpio,
    enterBootloader,
  } = useBoardMutations({ live, refresh, setSnapshot: setSnapshotVerified });

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
    setPd,
    readPower,
    setSwitch,
    enterBootloader,
    rebootBoard,
    restarting,
    restartTimedOut,
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
