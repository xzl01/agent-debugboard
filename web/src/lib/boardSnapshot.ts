import { parseSwitches } from "./switches.ts";
import { mergePersistentConfigSummary } from "./persistentConfig.ts";
import type {
  AdcReading,
  Availability,
  BoardSnapshot,
  BoardMonitoring,
  MemoryPressureSnapshot,
  PowerOutput,
  SafeGpio,
  WatchdogStatus,
} from "./types.ts";

export const EMPTY_BOARD_SNAPSHOT: BoardSnapshot = {
  powerOutputs: [],
  switches: {},
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

  return Object.assign({}, defaults, prev, raw);
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

function parseMonitoring(
  raw: unknown,
  prev: BoardMonitoring = EMPTY_BOARD_SNAPSHOT.monitoring
): BoardMonitoring {
  if (!isRecord(raw)) return prev;
  return {
    temperature: mergeAvailability(prev.temperature, raw.temperature, { available: false }),
    heap: mergeAvailability(prev.heap, raw.heap, { available: false }),
    memory: parseMonitoringMemory(raw.memory, prev.memory),
    runtime: mergeAvailability(prev.runtime, raw.runtime, { available: false }),
    cpu: mergeAvailability(prev.cpu, raw.cpu, { available: false }),
  };
}

function parseWatchdog(
  raw: unknown,
  prev: WatchdogStatus = EMPTY_BOARD_SNAPSHOT.watchdog
): WatchdogStatus {
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

export function mapBoardStatus(status: unknown, adc: readonly AdcReading[]): BoardSnapshot {
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
    layoutGroup: typeof g.layoutGroup === "string" ? g.layoutGroup : undefined,
    layoutLabel: typeof g.layoutLabel === "string" ? g.layoutLabel : undefined,
    layoutRow: typeof g.layoutRow === "number" ? g.layoutRow : undefined,
    layoutColumn: typeof g.layoutColumn === "number" ? g.layoutColumn : undefined,
  }));

  const switches = parseSwitches(record.switches);

  const config = mergePersistentConfigSummary(undefined, record.config);
  return {
    mcu: typeof record.mcu === "string" ? record.mcu : undefined,
    usb: typeof record.usb === "string" ? record.usb : undefined,
    powerCaptureProtocol:
      typeof record.power_capture_protocol === "string"
        ? record.power_capture_protocol
        : undefined,
    powerOutputs,
    switches,
    gpios,
    watchdog: parseWatchdog(record.watchdog, EMPTY_BOARD_SNAPSHOT.watchdog),
    monitoring: parseMonitoring(record.board_monitoring, EMPTY_BOARD_SNAPSHOT.monitoring),
    adc,
    ...(config ? { config } : {}),
  };
}

// Merge a WebSocket "snapshot" message into the previous snapshot, preserving
// metadata (signal/gp/controllable) we only learned from the HTTP status poll.
export function mergeBoardWsSnapshot(prev: BoardSnapshot, msg: unknown): BoardSnapshot {
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
        layoutGroup: typeof g.layoutGroup === "string" ? g.layoutGroup : undefined,
        layoutLabel: typeof g.layoutLabel === "string" ? g.layoutLabel : undefined,
        layoutRow: typeof g.layoutRow === "number" ? g.layoutRow : undefined,
        layoutColumn: typeof g.layoutColumn === "number" ? g.layoutColumn : undefined,
      }))
    : prev.gpios;

  const config = mergePersistentConfigSummary(prev.config, record.config);
  return {
    ...prev,
    powerCaptureProtocol:
      typeof record.power_capture_protocol === "string"
        ? record.power_capture_protocol
        : prev.powerCaptureProtocol,
    powerOutputs,
    switches: parseSwitches(record.switches, prev.switches),
    gpios,
    watchdog: parseWatchdog(record.watchdog, prev.watchdog),
    monitoring: parseMonitoring(record.board_monitoring, prev.monitoring),
    ...(config ? { config } : {}),
  };
}
