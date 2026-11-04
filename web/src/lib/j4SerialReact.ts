import { useEffect, useRef, useState, useSyncExternalStore } from "react";
import { J4SerialSession } from "./j4SerialSession";

export interface J4BoardAvailability {
  readonly available: boolean;
  // Telemetry may arrive out of order; it is never connection-epoch evidence.
  readonly uptimeMs?: number | null;
  readonly restarting?: boolean;
}

interface J4AutoConnection {
  ready: boolean;
  attempted: boolean;
  epoch: number;
  cleanup: Promise<void>;
  suspended: boolean;
  restoring: boolean;
}

function scheduleConnection(session: J4SerialSession, policy: J4AutoConnection, cancelled = () => false) {
  const epoch = policy.epoch;
  const connectionEpoch = session.getConnectionEpoch();
  // Let StrictMode's setup/cleanup probe finish before consuming the attempt.
  queueMicrotask(() => {
    void policy.cleanup.then(() => {
      if (cancelled() || !policy.ready || policy.suspended || policy.epoch !== epoch || policy.attempted) return;
      policy.attempted = true;
      const restoring = policy.restoring;
      policy.restoring = false;
      return session.connectAutomatically(connectionEpoch, restoring);
    }).catch(() => { /* The client reports errors; never retry in this epoch. */ });
  });
}

export function useJ4Serial(board?: J4BoardAvailability) {
  const [session] = useState(() => new J4SerialSession());
  const auto = useRef<J4AutoConnection>({ ready: false, attempted: false, epoch: 0,
    cleanup: Promise.resolve(), suspended: false, restoring: false });
  const available = board?.available === true && !board.restarting;
  useEffect(() => {
    const policy = auto.current;
    const suspend = () => {
      if (policy.suspended) return;
      policy.suspended = true;
      policy.restoring = true;
      policy.attempted = false;
      policy.epoch += 1;
      // Invalidate TX and callbacks synchronously, before any awaited cleanup.
      policy.cleanup = session.suspend();
    };
    const resume = () => {
      if (!policy.suspended) return;
      policy.suspended = false;
      session.resume();
      if (policy.ready) scheduleConnection(session, policy);
    };
    const pageshow = (event: PageTransitionEvent) => { if (event.persisted) resume(); };
    window.addEventListener("pagehide", suspend);
    window.addEventListener("pageshow", pageshow);
    document.addEventListener("freeze", suspend);
    document.addEventListener("resume", resume);
    // visibilitychange/hidden alone is not a transport lifetime boundary.
    return () => {
      window.removeEventListener("pagehide", suspend);
      window.removeEventListener("pageshow", pageshow);
      document.removeEventListener("freeze", suspend);
      document.removeEventListener("resume", resume);
      policy.epoch += 1;
      policy.cleanup = session.disconnect();
    };
  }, [session]);
  useEffect(() => {
    const policy = auto.current;
    if (!available) {
      if (policy.ready) {
        policy.epoch += 1;
        policy.cleanup = session.interruptTransport();
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
    if (policy.attempted || policy.suspended) return;
    let cancelled = false;
    // No phase dependency or timer: ordinary socket failures remain sticky.
    scheduleConnection(session, policy, () => cancelled);
    return () => { cancelled = true; };
  }, [available, session]);
  return session;
}

export function useJ4SerialSnapshot(session: J4SerialSession) {
  return useSyncExternalStore(session.subscribeState, session.getState, session.getState);
}
