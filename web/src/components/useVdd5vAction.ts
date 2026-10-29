import { useCallback, useRef, useState } from "react";
import { POWER_STATE_STALE_MS } from "@/lib/power";
import { useI18n } from "@/lib/i18n";

interface Vdd5vActionOptions {
  available: boolean;
  state: "on" | "off" | null;
  updatedAt: number | null;
  isLocked: () => boolean;
  acquire: () => boolean;
  release: () => void;
  setPower: (on: boolean) => Promise<void>;
}

/** App-owned action: every vdd_5v entry shares confirmation and one pending gate. */
export function useVdd5vAction(options: Vdd5vActionOptions) {
  const { t } = useI18n();
  const latest = useRef(options);
  latest.current = options;
  const pendingRef = useRef(false);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const set = useCallback(async (on: boolean) => {
    if (pendingRef.current) return;
    pendingRef.current = true;
    setPending(true);
    setError(null);
    let release: (() => void) | null = null;
    const read = () => {
      const current = latest.current;
      const age = current.updatedAt == null ? Infinity : Date.now() - current.updatedAt;
      if (!current.available || current.state == null || age < 0 || age >= POWER_STATE_STALE_MS) {
        throw new Error(t("status.vdd5v.unavailable"));
      }
      return current;
    };
    try {
      const before = read();
      const expectedState = on ? "off" : "on";
      if (before.state !== expectedState) throw new Error(t("status.vdd5v.unavailable"));
      if (before.isLocked()) throw new Error(t("task.error.busy"));
      if (!window.confirm(t(on ? "status.vdd5v.confirmOn" : "status.vdd5v.confirmOff"))) return;
      // Native confirm can block expiry timers. Read wall time, current state,
      // current transport callback and the real task lock again before dispatch.
      const current = read();
      if (current.state !== expectedState) throw new Error(t("status.vdd5v.unavailable"));
      if (!current.acquire()) throw new Error(t("task.error.busy"));
      release = current.release;
      await current.setPower(on);
    } catch (cause) {
      setError(`${t("status.vdd5v.failed")}${cause instanceof Error ? cause.message : String(cause)}`);
      throw cause;
    } finally {
      release?.();
      pendingRef.current = false;
      setPending(false);
    }
  }, [t]);

  return { set, pending, error };
}
