import { useEffect, useState } from "react";
import { getOtaStatus, type OtaStatus } from "@/lib/ota";

const POLL_MS = 5000;

/**
 * Lightweight OTA status hook for StatusBar badge.
 * Polls less frequently than OtaCard's 1.5s interval.
 */
export function useOtaBadge() {
  const [ota, setOta] = useState<OtaStatus | null>(null);

  useEffect(() => {
    let cancelled = false;
    let timer: number | null = null;

    const poll = async () => {
      try {
        const status = await getOtaStatus();
        if (!cancelled) setOta(status);
      } catch {
        if (!cancelled) setOta(null);
      }
      if (!cancelled) {
        timer = window.setTimeout(poll, POLL_MS);
      }
    };

    poll();

    return () => {
      cancelled = true;
      if (timer != null) window.clearTimeout(timer);
    };
  }, []);

  return ota;
}
