import { useEffect, useRef, useState, useSyncExternalStore } from "react";
import { J4SerialSession } from "./j4SerialSession";

export interface J4BoardAvailability {
  readonly available: boolean;
  // Telemetry may arrive out of order; it is never connection-epoch evidence.
  readonly uptimeMs?: number | null;
  readonly restarting?: boolean;
}

export function useJ4Serial(board?: J4BoardAvailability) {
  const [session] = useState(() => new J4SerialSession());
  const auto = useRef({ ready: false, attempted: false, epoch: 0, cleanup: Promise.resolve() });
  const available = board?.available === true && !board.restarting;
  useEffect(() => () => { void session.disconnect(); }, [session]);
  useEffect(() => {
    const policy = auto.current;
    if (!available) {
      if (policy.ready) {
        policy.epoch += 1;
        policy.cleanup = policy.cleanup.then(() => session.disconnect());
      }
      policy.ready = false;
      policy.attempted = false;
      return;
    }
    if (!policy.ready) {
      policy.epoch += 1;
      policy.attempted = false;
    }
    policy.ready = true;
    if (policy.attempted) return;
    const epoch = policy.epoch;
    let cancelled = false;
    // Let StrictMode's setup/cleanup probe finish before consuming the attempt.
    // No timer/retry loop and no dependency on the serial connection phase:
    // manual Disconnect, unavailable and socket failure stay sticky this epoch.
    queueMicrotask(() => {
      void policy.cleanup.then(async () => {
        if (cancelled || !policy.ready || policy.epoch !== epoch || policy.attempted) return;
        policy.attempted = true;
        const phase = session.getState().phase;
        if (phase !== "connected" && phase !== "connecting") await session.connect();
      }).catch(() => { /* The session publishes the failure; do not retry. */ });
    });
    return () => { cancelled = true; };
  }, [available, session]);
  return session;
}

export function useJ4SerialSnapshot(session: J4SerialSession) {
  return useSyncExternalStore(session.subscribeState, session.getState, session.getState);
}
