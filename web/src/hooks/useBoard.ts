import { useCallback, useEffect, useRef, useState } from "react";
import * as api from "@/lib/api";
import type {
  AdcReading,
  Availability,
  BoardSnapshot,
  BoardMonitoring,
  CaptureConfig,
  CaptureSample,
  PowerCapture,
  MemoryPressureSnapshot,
  PowerOutput,
  SafeGpio,
  SwitchState,
  WatchdogStatus,
} from "@/lib/types";

const EMPTY: BoardSnapshot = {
  powerOutputs: [],
  switches: { sd: "", usb: "", vin: "" },
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

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

function mergeAvailability<T extends Availability>(
  prev: T,
  raw: unknown,
  defaults?: Partial<T>
): T {
  if (!isRecord(raw)) {
    return prev;
  }

  return {
    ...defaults,
    ...prev,
    ...raw,
  } as T;
}

function parseMemoryPressure(
  raw: unknown,
  prev?: MemoryPressureSnapshot
): MemoryPressureSnapshot | undefined {
  if (!isRecord(raw)) {
    return prev;
  }

  return mergeAvailability(prev ?? { available: false }, raw);
}

function parseMonitoringMemory(
  raw: unknown,
  prev: BoardMonitoring["memory"]
): BoardMonitoring["memory"] {
  if (!isRecord(raw)) {
    return prev;
  }

  const next = mergeAvailability(prev ?? { available: false }, raw);
  const physical = isRecord(raw.physical)
    ? {
        ...prev?.physical,
        ...raw.physical,
      }
    : prev?.physical;
  const stacks = isRecord(raw.stacks)
    ? {
        ...prev?.stacks,
        ...raw.stacks,
      }
    : prev?.stacks;
  const current_pressure = parseMemoryPressure(raw.current_pressure, prev?.current_pressure);
  const peak_pressure = parseMemoryPressure(raw.peak_pressure, prev?.peak_pressure);

  return {
    ...next,
    physical,
    stacks,
    current_pressure,
    peak_pressure,
  };
}

function parseMonitoring(raw: unknown, prev: BoardMonitoring = EMPTY.monitoring): BoardMonitoring {
  if (!isRecord(raw)) return prev;
  return {
    temperature: mergeAvailability(prev.temperature, raw.temperature, { available: false }),
    heap: mergeAvailability(prev.heap, raw.heap, { available: false }),
    memory: parseMonitoringMemory(raw.memory, prev.memory),
    runtime: mergeAvailability(prev.runtime, raw.runtime, { available: false }),
    cpu: mergeAvailability(prev.cpu, raw.cpu, { available: false }),
  };
}

function parseWatchdog(raw: unknown, prev: WatchdogStatus = EMPTY.watchdog): WatchdogStatus {
  if (!isRecord(raw)) return prev;

  return {
    supported: typeof raw.supported === "boolean" ? raw.supported : prev.supported,
    automatic: typeof raw.automatic === "boolean" ? raw.automatic : prev.automatic,
    healthy: typeof raw.healthy === "boolean" ? raw.healthy : prev.healthy,
    armed: typeof raw.armed === "boolean" ? raw.armed : prev.armed,
    timeout_ms: typeof raw.timeout_ms === "number" ? raw.timeout_ms : prev.timeout_ms,
    bootloader_on_timeout:
      typeof raw.bootloader_on_timeout === "boolean"
        ? raw.bootloader_on_timeout
        : prev.bootloader_on_timeout,
    failing_service:
      typeof raw.failing_service === "string" ? raw.failing_service : prev.failing_service,
  };
}

function mapStatus(status: unknown, adc: AdcReading[]): BoardSnapshot {
  const record = isRecord(status) ? status : {};
  const rawOutputs = Array.isArray(record.power_outputs)
    ? record.power_outputs.filter(isRecord)
    : [];
  const powerOutputs: PowerOutput[] = rawOutputs.map((o) => ({
    name: typeof o.name === "string" ? o.name : "",
    signal: typeof o.signal === "string" ? o.signal : undefined,
    gp: typeof o.gp === "number" ? o.gp : undefined,
    controllable: typeof o.controllable === "boolean" ? o.controllable : false,
    state: typeof o.state === "string" ? o.state : "",
    value: typeof o.value === "number" ? o.value : null,
  }));

  const rawGpios = Array.isArray(record.gpios) ? record.gpios.filter(isRecord) : [];
  const gpios: SafeGpio[] = rawGpios.map((g) => ({
    name: typeof g.name === "string" ? g.name : "",
    pin: typeof g.pin === "number" ? g.pin : 0,
    note: typeof g.note === "string" ? g.note : "",
    value: typeof g.value === "number" ? g.value : 0,
    direction: typeof g.direction === "string" ? g.direction : "input",
  }));

  const rawSwitches = isRecord(record.switches) ? record.switches : undefined;
  const rawSd = rawSwitches && isRecord(rawSwitches.sd) ? rawSwitches.sd : undefined;
  const rawUsb = rawSwitches && isRecord(rawSwitches.usb) ? rawSwitches.usb : undefined;
  const rawVin = rawSwitches && isRecord(rawSwitches.vin) ? rawSwitches.vin : undefined;
  const switches: SwitchState = {
    sd: typeof rawSd?.route === "string" ? rawSd.route : "",
    usb: typeof rawUsb?.route === "string" ? rawUsb.route : "",
    vin: typeof rawVin?.route === "string" ? rawVin.route : "",
  };

  return {
    mcu: typeof record.mcu === "string" ? record.mcu : undefined,
    usb: typeof record.usb === "string" ? record.usb : undefined,
    powerOutputs,
    switches,
    gpios,
    watchdog: parseWatchdog(record.watchdog, EMPTY.watchdog),
    monitoring: parseMonitoring(record.board_monitoring, EMPTY.monitoring),
    adc,
  };
}

// Merge a WebSocket "snapshot" message into the previous snapshot, preserving
// metadata (signal/gp/controllable) we only learned from the HTTP status poll.
function mergeWsSnapshot(prev: BoardSnapshot, msg: any): BoardSnapshot {
  const meta = new Map(prev.powerOutputs.map((o) => [o.name, o]));
  const record = isRecord(msg) ? msg : {};
  const powerOutputs: PowerOutput[] = Array.isArray(record.power_outputs)
    ? record.power_outputs.filter(isRecord).map((o) => ({
        name: typeof o.name === "string" ? o.name : "",
        signal: meta.get(typeof o.name === "string" ? o.name : "")?.signal,
        gp: meta.get(typeof o.name === "string" ? o.name : "")?.gp,
        controllable:
          meta.get(typeof o.name === "string" ? o.name : "")?.controllable ?? true,
        state: typeof o.state === "string" ? o.state : "",
        value: typeof o.value === "number" ? o.value : null,
      }))
    : prev.powerOutputs;

  const gpios: SafeGpio[] = Array.isArray(record.gpios)
    ? record.gpios.filter(isRecord).map((g) => ({
        name: typeof g.name === "string" ? g.name : "",
        pin: typeof g.pin === "number" ? g.pin : 0,
        note: typeof g.note === "string" ? g.note : "",
        value: typeof g.value === "number" ? g.value : 0,
        direction: typeof g.direction === "string" ? g.direction : "input",
      }))
    : prev.gpios;

  const rawSwitches = isRecord(record.switches) ? record.switches : undefined;
  const rawSd = rawSwitches && isRecord(rawSwitches.sd) ? rawSwitches.sd : undefined;
  const rawUsb = rawSwitches && isRecord(rawSwitches.usb) ? rawSwitches.usb : undefined;
  const rawVin = rawSwitches && isRecord(rawSwitches.vin) ? rawSwitches.vin : undefined;

  return {
    ...prev,
    powerOutputs,
    switches: rawSwitches
      ? {
          sd: typeof rawSd?.route === "string" ? rawSd.route : prev.switches.sd,
          usb: typeof rawUsb?.route === "string" ? rawUsb.route : prev.switches.usb,
          vin: typeof rawVin?.route === "string" ? rawVin.route : prev.switches.vin,
        }
      : prev.switches,
    gpios,
    watchdog: parseWatchdog(record.watchdog, prev.watchdog),
    monitoring: parseMonitoring(record.board_monitoring, prev.monitoring),
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
  refresh: () => Promise<void>;
  setPower: (name: string, on: boolean) => Promise<void>;
  readPower: (name: string) => Promise<{ state: string; currentUa: number }>;
  setSwitch: (name: "sd" | "usb" | "vin", route: string) => Promise<void>;
  setGpio: (identifier: string, direction: "input" | "output", value?: number) => Promise<void>;
  enterBootloader: () => Promise<void>;
  captureState: "idle" | "connecting" | "armed" | "receiving";
  captureProgress: { received: number; total: number } | null;
  captures: PowerCapture[];
  armCapture: (config: CaptureConfig) => Promise<void>;
  triggerCapture: () => void;
  cancelCapture: () => void;
  clearCaptures: () => void;
}

type CaptureBuilder = Omit<PowerCapture, "samples" | "capturedAt"> & {
  samples: CaptureSample[];
  expected: number;
};

function captureArmMessage(config: CaptureConfig) {
  return {
    type: "command",
    command: "capture_arm",
    id: "web-capture",
    trigger: config.trigger,
    output: config.trigger === "gpio" ? "" : config.source,
    gpio: config.trigger === "gpio" ? config.source : "",
    edge: config.edge,
    threshold_ua: config.thresholdUa,
    rate_hz: config.rateHz,
    pre_samples: config.preSamples,
    post_samples: config.postSamples,
  };
}

export function useBoard(): UseBoard {
  const [snapshot, setSnapshot] = useState<BoardSnapshot>(EMPTY);
  const [hasData, setHasData] = useState(false);
  const [connected, setConnected] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const [auto, setAuto] = useState(true);
  const [live, setLive] = useState(false);
  const [captureState, setCaptureState] = useState<UseBoard["captureState"]>("idle");
  const [captureProgress, setCaptureProgress] = useState<UseBoard["captureProgress"]>(null);
  const [captures, setCaptures] = useState<PowerCapture[]>([]);
  const wsRef = useRef<WebSocket | null>(null);
  const pendingCaptureRef = useRef<CaptureConfig | null>(null);
  const captureBuilderRef = useRef<CaptureBuilder | null>(null);
  const captureArmPromiseRef = useRef<{
    resolve: () => void;
    reject: (reason: Error) => void;
  } | null>(null);

  const resetCapture = useCallback((reason?: Error) => {
    if (captureArmPromiseRef.current) {
      captureArmPromiseRef.current.reject(reason ?? new Error("Power capture arming was cancelled"));
      captureArmPromiseRef.current = null;
    }
    pendingCaptureRef.current = null;
    captureBuilderRef.current = null;
    setCaptureProgress(null);
    setCaptureState("idle");
  }, []);

  const refresh = useCallback(async () => {
    try {
      const status = await api.getStatus();
      const adcRes = await api.getAdc();
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
          const status = await api.getStatus();
          const adcRes = await api.getAdc();
          if (cancelled) return;
          setSnapshot(mapStatus(status, adcRes?.readings ?? []));
          setHasData(true);
          setConnected(true);
          setError(null);
          setLoading(false);
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
        wsRef.current = ws;
        ws.onopen = () => {
          ws?.send(JSON.stringify({ type: "subscribe", topic: "live", rate_hz: 10, id: "web" }));
          if (pendingCaptureRef.current) {
            ws?.send(JSON.stringify(captureArmMessage(pendingCaptureRef.current)));
          }
        };
        ws.onmessage = (ev) => {
          try {
            const msg = JSON.parse(ev.data);
            if (msg.type === "snapshot") {
              setSnapshot((prev) => mergeWsSnapshot(prev, msg));
            } else if (msg.type === "telemetry" && Array.isArray(msg.readings)) {
              setSnapshot((prev) => ({ ...prev, adc: msg.readings }));
            } else if (msg.type === "result" && msg.command === "capture_arm") {
              pendingCaptureRef.current = null;
              setCaptureState("armed");
              captureArmPromiseRef.current?.resolve();
              captureArmPromiseRef.current = null;
            } else if (msg.type === "capture_begin") {
              captureBuilderRef.current = {
                id: msg.capture_id,
                trigger: msg.trigger,
                source: msg.source,
                edge: msg.edge,
                thresholdUa: msg.threshold_ua,
                rateHz: msg.rate_hz,
                preSamples: msg.pre_samples,
                postSamples: msg.post_samples,
                triggerOffset: msg.trigger_offset,
                expected: msg.sample_count,
                samples: [],
              };
              setCaptureState("receiving");
              setCaptureProgress({ received: 0, total: msg.sample_count });
            } else if (msg.type === "capture_sample" && captureBuilderRef.current) {
              const builder = captureBuilderRef.current;
              builder.samples.push({
                offset: msg.offset,
                triggered: !!msg.triggered,
                sampleSequence: msg.sample_sequence,
                deviceTimeUs: msg.device_t_mono_us,
                readings: (msg.readings ?? []).map((reading: any) => ({
                  name: reading.name,
                  signal: "",
                  power_enabled: !!reading.power_enabled,
                  raw: null,
                  mv: 0,
                  sensor_channel: "current",
                  unit: "A",
                  current_ua: reading.current_ua ?? 0,
                })),
              });
              if (builder.samples.length % 20 === 0 || builder.samples.length === builder.expected) {
                setCaptureProgress({ received: builder.samples.length, total: builder.expected });
              }
            } else if (msg.type === "capture_complete" && captureBuilderRef.current) {
              const builder = captureBuilderRef.current;
              const completed: PowerCapture = {
                id: builder.id,
                trigger: builder.trigger,
                source: builder.source,
                edge: builder.edge,
                thresholdUa: builder.thresholdUa,
                rateHz: builder.rateHz,
                preSamples: builder.preSamples,
                postSamples: builder.postSamples,
                triggerOffset: builder.triggerOffset,
                samples: builder.samples,
                capturedAt: Date.now(),
              };
              setCaptures((previous) => [...previous, completed].slice(-4));
              captureBuilderRef.current = null;
              setCaptureProgress(null);
              setCaptureState("idle");
            } else if (msg.type === "error" && msg.command === "capture") {
              const message = msg.error?.message ?? "Power capture failed";
              resetCapture(new Error(message));
              setError(message);
            }
          } catch {
            /* ignore malformed frames */
          }
        };
        ws.onerror = () => {
          if (!cancelled) {
            resetCapture(new Error("Live WebSocket error"));
            setError("Live WebSocket error");
          }
        };
        ws.onclose = () => {
          if (wsRef.current === ws) wsRef.current = null;
          void releaseSession();
          if (!cancelled) {
            resetCapture(new Error("Live WebSocket disconnected"));
            setLive(false);
            setError("Live WebSocket disconnected");
          }
        };
      })();

      return () => {
        cancelled = true;
        resetCapture();
        if (wsRef.current === ws) wsRef.current = null;
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
  }, [auto, live, refresh, resetCapture]);

  const setPower = useCallback(
    async (name: string, on: boolean) => {
      const response = await api.setPower(name, on);
      const expectedState = on ? "on" : "off";
      if (response?.power_output?.name !== name || response?.power_output?.state !== expectedState) {
        throw new Error(`Power output ${name} did not confirm state ${expectedState}`);
      }
      if (!live) await refresh();
    },
    [live, refresh]
  );

  const readPower = useCallback(async (name: string) => {
    // Keep these reads sequential. The firmware HTTP server has a deliberately
    // small client pool and the live WebSocket already owns one slot; opening
    // two more requests at once can cause a transient connection refusal
    // exactly while a power-cycle task is verifying the shutdown edge.
    const status = await api.getStatus();
    const adcRes = await api.getAdc();
    const output = (status?.power_outputs ?? []).find((item: any) => item.name === name);
    const reading = (adcRes?.readings ?? []).find((item: any) => item.name === name);
    if (!output) throw new Error(`Power output ${name} was not reported by the device`);
    return {
      state: String(output.state ?? ""),
      currentUa: Math.max(0, Number(reading?.current_ua ?? 0)),
    };
  }, []);

  const setSwitch = useCallback(
    async (name: "sd" | "usb" | "vin", route: string) => {
      await api.setSwitch(name, route);
      if (!live) await refresh();
    },
    [live, refresh]
  );

  const setGpio = useCallback(
    async (identifier: string, direction: "input" | "output", value?: number) => {
      await api.setGpio(identifier, direction, value);
      if (!live) await refresh();
    },
    [live, refresh]
  );

  const enterBootloader = useCallback(async () => {
    await api.enterBootloader();
  }, []);

  const armCapture = useCallback((config: CaptureConfig) => new Promise<void>((resolve, reject) => {
    captureArmPromiseRef.current?.reject(new Error("Power capture arming was superseded"));
    captureArmPromiseRef.current = { resolve, reject };
    pendingCaptureRef.current = config;
    captureBuilderRef.current = null;
    setCaptureProgress(null);
    setCaptureState("connecting");
    setError(null);
    if (wsRef.current?.readyState === WebSocket.OPEN) {
      wsRef.current.send(JSON.stringify(captureArmMessage(config)));
    } else {
      setLive(true);
    }
  }), []);

  const triggerCapture = useCallback(() => {
    wsRef.current?.send(JSON.stringify({
      type: "command", command: "capture_trigger", id: "web-trigger",
    }));
  }, []);

  const cancelCapture = useCallback(() => {
    wsRef.current?.send(JSON.stringify({
      type: "command", command: "capture_cancel", id: "web-cancel",
    }));
    resetCapture(new Error("Power capture was cancelled"));
  }, [resetCapture]);

  const clearCaptures = useCallback(() => setCaptures([]), []);

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
    readPower,
    setSwitch,
    enterBootloader,
    setGpio,
    captureState,
    captureProgress,
    captures,
    armCapture,
    triggerCapture,
    cancelCapture,
    clearCaptures,
  };
}
