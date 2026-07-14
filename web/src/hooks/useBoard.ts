import { useCallback, useEffect, useRef, useState } from "react";
import * as api from "@/lib/api";
import type {
  AdcReading,
  BoardSnapshot,
  BoardMonitoring,
  PowerOutput,
  SafeGpio,
  SwitchState,
  WatchdogStatus,
} from "@/lib/types";

const EMPTY: BoardSnapshot = {
  powerOutputs: [],
  switches: { sd: "", usb: "" },
  gpios: [],
  watchdog: {
    supported: false,
    automatic: false,
    healthy: false,
    armed: false,
    timeout_ms: 0,
    bootloader_on_timeout: false,
    failing_service: "",
  },
  monitoring: {
    temperature: { available: false },
    heap: { available: false },
    runtime: { available: false },
    cpu: { available: false },
  },
  adc: [],
};

function parseMonitoring(raw: any): BoardMonitoring {
  if (!raw) return EMPTY.monitoring;
  return {
    temperature: raw.temperature ?? { available: false },
    heap: raw.heap ?? { available: false },
    runtime: raw.runtime ?? { available: false },
    cpu: raw.cpu ?? { available: false },
  };
}

function parseWatchdog(raw: any): WatchdogStatus {
  if (!raw) return EMPTY.watchdog;
  return {
    supported: !!raw.supported,
    automatic: !!raw.automatic,
    healthy: !!raw.healthy,
    armed: !!raw.armed,
    timeout_ms: raw.timeout_ms ?? 0,
    bootloader_on_timeout: !!raw.bootloader_on_timeout,
    failing_service: raw.failing_service ?? "",
  };
}

function mapStatus(status: any, adc: AdcReading[]): BoardSnapshot {
  const rawOutputs: any[] = status?.power_outputs ?? [];
  const powerOutputs: PowerOutput[] = rawOutputs.map((o) => ({
    name: o.name,
    signal: o.signal,
    gp: o.gp,
    controllable: o.controllable,
    state: o.state,
    value: o.value,
  }));

  const rawGpios: any[] = status?.gpios ?? [];
  const gpios: SafeGpio[] = rawGpios.map((g) => ({
    name: g.name,
    pin: g.pin,
    note: g.note,
    value: g.value,
    direction: g.direction,
  }));

  const switches: SwitchState = {
    sd: status?.switches?.sd?.route ?? "",
    usb: status?.switches?.usb?.route ?? "",
  };

  return {
    mcu: status?.mcu,
    usb: status?.usb,
    powerOutputs,
    switches,
    gpios,
    watchdog: parseWatchdog(status?.watchdog),
    monitoring: parseMonitoring(status?.board_monitoring),
    adc,
  };
}

// Merge a WebSocket "snapshot" message into the previous snapshot, preserving
// metadata (signal/gp/controllable) we only learned from the HTTP status poll.
function mergeWsSnapshot(prev: BoardSnapshot, msg: any): BoardSnapshot {
  const meta = new Map(prev.powerOutputs.map((o) => [o.name, o]));
  const powerOutputs: PowerOutput[] = (msg.power_outputs ?? []).map((o: any) => ({
    name: o.name,
    signal: meta.get(o.name)?.signal,
    gp: meta.get(o.name)?.gp,
    controllable: meta.get(o.name)?.controllable ?? true,
    state: o.state,
    value: o.value,
  }));

  const gpios: SafeGpio[] = (msg.gpios ?? []).map((g: any) => ({
    name: g.name,
    pin: g.pin,
    note: g.note,
    value: g.value,
    direction: g.direction,
  }));

  return {
    ...prev,
    powerOutputs,
    switches: msg.switches
      ? { sd: msg.switches.sd?.route ?? prev.switches.sd, usb: msg.switches.usb?.route ?? prev.switches.usb }
      : prev.switches,
    gpios,
    watchdog: parseWatchdog(msg.watchdog),
    monitoring: parseMonitoring(msg.board_monitoring),
  };
}

export interface UseBoard {
  snapshot: BoardSnapshot;
  hasData: boolean;
  connected: boolean;
  error: string | null;
  loading: boolean;
  auto: boolean;
  setAuto: (v: boolean) => void;
  live: boolean;
  setLive: (v: boolean) => void;
  refresh: () => void;
  setPower: (name: string, on: boolean) => Promise<void>;
  setSwitch: (name: "sd" | "usb", route: string) => Promise<void>;
  setGpio: (identifier: string, direction: "input" | "output", value?: number) => Promise<void>;
  enterBootloader: () => Promise<void>;
}

export function useBoard(): UseBoard {
  const [snapshot, setSnapshot] = useState<BoardSnapshot>(EMPTY);
  const [hasData, setHasData] = useState(false);
  const [connected, setConnected] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const [auto, setAuto] = useState(true);
  const [live, setLive] = useState(false);

  const refresh = useCallback(async () => {
    try {
      const [status, adcRes] = await Promise.all([api.getStatus(), api.getAdc()]);
      const readings: AdcReading[] = adcRes?.readings ?? [];
      setSnapshot(mapStatus(status, readings));
      setHasData(true);
      setConnected(true);
      setError(null);
    } catch (e) {
      setConnected(false);
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setLoading(false);
    }
  }, []);

  // Polling mode (default) or WebSocket live mode.
  useEffect(() => {
    if (live) {
      let ws: WebSocket | null = null;
      let cancelled = false;
      let sessionId: number | null = null;
      let sessionReleased = false;

      const releaseSession = async () => {
        if (sessionId == null || sessionReleased) return;
        sessionReleased = true;
        try {
          await api.deleteLiveSession(sessionId);
        } catch {
          // The firmware also releases the session when the socket closes, so
          // a follow-up DELETE can legitimately return unknown_session_id.
        }
      };

      (async () => {
        // Seed metadata from an HTTP poll before switching to live stream.
        try {
          const [status, adcRes] = await Promise.all([api.getStatus(), api.getAdc()]);
          if (cancelled) return;
          setSnapshot(mapStatus(status, adcRes?.readings ?? []));
          setHasData(true);
          setConnected(true);
          setError(null);
        } catch (e) {
          setLive(false);
          setConnected(false);
          setError(e instanceof Error ? e.message : String(e));
          return;
        }

        let session: api.LiveSession;
        try {
          session = await api.createLiveSession();
          sessionId = session.session_id;
          if (cancelled) {
            await releaseSession();
            return;
          }
        } catch (e) {
          if (!cancelled) {
            setLive(false);
            setError(e instanceof Error ? e.message : String(e));
          }
          return;
        }

        // Use the session path returned by firmware, but keep the browser
        // connection same-origin so Vite's WebSocket proxy and deployed pages
        // both work without hard-coding the board address.
        ws = new WebSocket(api.liveWebSocketUrl(session.ws_url));
        ws.onopen = () => {
          ws?.send(JSON.stringify({ type: "subscribe", topic: "live", rate_hz: 10, id: "web" }));
        };
        ws.onmessage = (ev) => {
          try {
            const msg = JSON.parse(ev.data);
            if (msg.type === "snapshot") {
              setSnapshot((prev) => mergeWsSnapshot(prev, msg));
            } else if (msg.type === "telemetry" && Array.isArray(msg.readings)) {
              setSnapshot((prev) => ({ ...prev, adc: msg.readings }));
            }
          } catch {
            /* ignore malformed frames */
          }
        };
        ws.onerror = () => {
          if (!cancelled) setError("Live WebSocket error");
        };
        ws.onclose = () => {
          void releaseSession();
          if (!cancelled) {
            setLive(false);
            setError("Live WebSocket disconnected");
          }
        };
      })();

      return () => {
        cancelled = true;
        if (ws && ws.readyState < WebSocket.CLOSING) {
          ws.close();
        } else {
          void releaseSession();
        }
      };
    }

    setLoading(true);
    refresh();
    if (!auto) return;
    const id = setInterval(refresh, 2000);
    return () => clearInterval(id);
  }, [auto, live, refresh]);

  const setPower = useCallback(
    async (name: string, on: boolean) => {
      await api.setPower(name, on);
      if (!live) refresh();
    },
    [live, refresh]
  );

  const setSwitch = useCallback(
    async (name: "sd" | "usb", route: string) => {
      await api.setSwitch(name, route);
      if (!live) refresh();
    },
    [live, refresh]
  );

  const setGpio = useCallback(
    async (identifier: string, direction: "input" | "output", value?: number) => {
      await api.setGpio(identifier, direction, value);
      if (!live) refresh();
    },
    [live, refresh]
  );

  const enterBootloader = useCallback(async () => {
    await api.enterBootloader();
  }, []);

  return {
    snapshot,
    hasData,
    connected,
    error,
    loading,
    auto,
    setAuto,
    live,
    setLive,
    refresh,
    setPower,
    setSwitch,
    enterBootloader,
    setGpio,
  };
}
