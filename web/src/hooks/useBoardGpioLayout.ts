import { useCallback, useEffect, useRef, useState } from "react";
import { getGpioLayout } from "@/lib/api";
import type { GpioPhysicalLayout } from "@/lib/types";

/** One read per reachable-board epoch; failure is local to the GPIO panel. */
export function useBoardGpioLayout() {
  const [gpioPhysicalLayout, setLayout] = useState<GpioPhysicalLayout | null>(null);
  const [gpioLayoutError, setError] = useState<string | null>(null);
  const [gpioLayoutLoading, setLoading] = useState(false);
  const mounted = useRef(false);
  const epoch = useRef({ attempted: false, controller: null as AbortController | null });

  const invalidateGpioLayout = useCallback(() => {
    const current = epoch.current;
    if (!current.attempted) return;
    current.controller?.abort();
    epoch.current = { attempted: false, controller: null };
    setLayout(null);
    setError(null);
    setLoading(false);
  }, []);

  const noteGpioStatus = useCallback(() => {
    if (!mounted.current || epoch.current.attempted) return;
    const controller = new AbortController();
    const requestEpoch = { attempted: true, controller };
    epoch.current = requestEpoch;
    setLoading(true);
    setError(null);
    void getGpioLayout(controller.signal).then((layout) => {
      if (controller.signal.aborted || epoch.current !== requestEpoch) return;
      setLayout(layout);
    }).catch((cause: unknown) => {
      if (controller.signal.aborted || epoch.current !== requestEpoch) return;
      setError(cause instanceof Error ? cause.message : String(cause));
    }).finally(() => {
      if (!controller.signal.aborted && epoch.current === requestEpoch) setLoading(false);
    });
  }, []);

  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
      epoch.current.controller?.abort();
      epoch.current = { attempted: false, controller: null };
    };
  }, []);

  return { gpioPhysicalLayout, gpioLayoutError, gpioLayoutLoading, noteGpioStatus, invalidateGpioLayout };
}
