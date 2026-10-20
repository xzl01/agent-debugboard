import { useCallback, useEffect, useRef, useState } from "react";
import * as api from "@/lib/api";

export const REBOOT_REQUEST_TIMEOUT_MS = 10_000;
export const REBOOT_RECONNECT_TIMEOUT_MS = 30_000;
const MIN_UPTIME_ROLLBACK_MS = 5000;
const FRESH_BOOT_TOLERANCE_MS = 5000;

type RestartAttempt = {
  controller: AbortController;
  accepted: boolean;
  disconnected: boolean;
  reconnected: boolean;
  online: boolean;
  uptimeAtRequestMs: number | null;
  requestedAtMs: number;
  uptimeReset: boolean;
  requestTimer?: ReturnType<typeof setTimeout>;
  reconnectTimer?: ReturnType<typeof setTimeout>;
};

/** Owns the expected-disconnect window, independently of maintenance UI mounts. */
export function useBoardReboot(connected: boolean, uptimeMs: number | null) {
  const [restarting, setRestarting] = useState(false);
  const [restartTimedOut, setRestartTimedOut] = useState(false);
  const attemptRef = useRef<RestartAttempt | null>(null);
  const currentUptimeMs = typeof uptimeMs === "number" && Number.isSafeInteger(uptimeMs) && uptimeMs >= 0 ? uptimeMs : null;

  const finish = useCallback((attempt: RestartAttempt, timedOut = false) => {
    if (attemptRef.current !== attempt) return;
    clearTimeout(attempt.requestTimer);
    clearTimeout(attempt.reconnectTimer);
    attemptRef.current = null;
    setRestarting(false);
    setRestartTimedOut(timedOut);
  }, []);

  useEffect(() => () => {
    const attempt = attemptRef.current;
    attemptRef.current = null;
    if (attempt) {
      clearTimeout(attempt.requestTimer);
      clearTimeout(attempt.reconnectTimer);
      attempt.controller.abort();
    }
  }, []);

  const isExpectedRestartDisconnect = useCallback(() => attemptRef.current !== null, []);
  const noteConnection = useCallback((online: boolean) => {
    const attempt = attemptRef.current;
    if (!attempt) return;
    attempt.online = online;
    if (!online) {
      attempt.disconnected = true;
      attempt.reconnected = false;
    } else if (attempt.disconnected) {
      attempt.reconnected = true;
      if (attempt.accepted) finish(attempt);
    }
  }, [finish]);

  useEffect(() => {
    const attempt = attemptRef.current;
    if (!attempt || !connected) return;
    const baseline = attempt.uptimeAtRequestMs;
    // A missed USB outage is normal with 2 s polling. Require a substantial
    // rollback into a plausible fresh-boot window, not minor rounding, stale
    // status jitter, or the first known uptime after an unknown baseline.
    attempt.uptimeReset = baseline !== null && currentUptimeMs !== null &&
      baseline - currentUptimeMs >= MIN_UPTIME_ROLLBACK_MS &&
      currentUptimeMs <= baseline / 2 &&
      currentUptimeMs <= performance.now() - attempt.requestedAtMs + FRESH_BOOT_TOLERANCE_MS;
    if (attempt.accepted && attempt.uptimeReset) finish(attempt);
  }, [connected, currentUptimeMs, finish]);

  const rebootBoard = useCallback(async () => {
    if (attemptRef.current) throw new Error("A normal restart is already in progress");
    if (!connected) throw new Error("Connect the debugger before restarting it");
    const attempt: RestartAttempt = {
      controller: new AbortController(), accepted: false, disconnected: false, reconnected: false,
      online: connected, uptimeAtRequestMs: currentUptimeMs, requestedAtMs: performance.now(), uptimeReset: false,
    };
    attemptRef.current = attempt;
    setRestartTimedOut(false);
    setRestarting(true);
    try {
      const timeout = new Promise<never>((_, reject) => {
        attempt.requestTimer = setTimeout(() => {
          reject(Object.assign(new Error("Normal restart request timed out"), { code: "reboot_request_timeout" }));
          attempt.controller.abort();
        }, REBOOT_REQUEST_TIMEOUT_MS);
      });
      await Promise.race([api.rebootBoard(attempt.controller.signal), timeout]);
      if (attemptRef.current !== attempt) return;
      clearTimeout(attempt.requestTimer);
      attempt.accepted = true;
      if (attempt.reconnected || (attempt.online && attempt.uptimeReset)) finish(attempt);
      else attempt.reconnectTimer = setTimeout(() => finish(attempt, true), REBOOT_RECONNECT_TIMEOUT_MS);
      // Do not refresh here. Existing HTTP/WS connection handling observes the
      // a real reconnect or uptime reset; a 202 alone is not proof of boot.
    } catch (error) {
      finish(attempt);
      throw error;
    }
  }, [connected, currentUptimeMs, finish]);

  return { rebootBoard, restarting, restartTimedOut, isExpectedRestartDisconnect, noteConnection };
}
