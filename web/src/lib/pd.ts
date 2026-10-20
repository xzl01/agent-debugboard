import type { PdStatus } from "./types.ts";

const EMPTY_PD_STATUS: PdStatus = {
  available: false,
  mode: 0,
  voltage_mv: 5000,
  gear: 0,
  pps_mv: 0,
  pd: false,
  qc2: false,
  qc3: false,
  bc: false,
  epr: false,
  avs: false,
  max_current_ma: 0,
  last_error: 0,
  protocol: "none",
  board_max_voltage_mv: 20000,
  capabilities_valid: false,
  fixed_profiles: [],
  pps_ranges: [],
  avs_ranges: [],
};

function record(value: unknown): Record<string, unknown> | null {
  return typeof value === "object" && value !== null && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
}

function number(raw: Record<string, unknown>, key: string, fallback: number): number {
  return typeof raw[key] === "number" && Number.isFinite(raw[key]) ? raw[key] : fallback;
}

// Merge the raw partial update here, before normalization loses field presence.
// Missing fields retain known state; an explicit false invalidates every table.
export function parsePdStatus(value: unknown, previous: PdStatus = EMPTY_PD_STATUS): PdStatus {
  const raw = record(value);
  if (!raw) return previous;
  const mode = number(raw, "mode", previous.mode);
  const status: PdStatus = {
    available: typeof raw.available === "boolean" ? raw.available : previous.available,
    mode: mode === 1 ? 1 : 0,
    voltage_mv: number(raw, "voltage_mv", previous.voltage_mv),
    gear: number(raw, "gear", previous.gear),
    pps_mv: number(raw, "pps_mv", previous.pps_mv),
    pd: typeof raw.pd === "boolean" ? raw.pd : previous.pd,
    qc2: typeof raw.qc2 === "boolean" ? raw.qc2 : previous.qc2,
    qc3: typeof raw.qc3 === "boolean" ? raw.qc3 : previous.qc3,
    bc: typeof raw.bc === "boolean" ? raw.bc : previous.bc,
    epr: typeof raw.epr === "boolean" ? raw.epr : previous.epr,
    avs: typeof raw.avs === "boolean" ? raw.avs : previous.avs,
    max_current_ma: number(raw, "max_current_ma", previous.max_current_ma),
    last_error: number(raw, "last_error", previous.last_error),
    protocol: typeof raw.protocol === "string" ? raw.protocol : previous.protocol,
    board_max_voltage_mv: number(raw, "board_max_voltage_mv", previous.board_max_voltage_mv),
    capabilities_valid: typeof raw.capabilities_valid === "boolean"
      ? raw.capabilities_valid : previous.capabilities_valid,
    fixed_profiles: Array.isArray(raw.fixed_profiles)
      ? raw.fixed_profiles.flatMap((entry) => {
          const item = record(entry);
          return item ? [{
            voltage_mv: number(item, "voltage_mv", 0),
            max_current_ma: number(item, "max_current_ma", 0),
          }] : [];
        }).filter((profile) => profile.voltage_mv > 0)
      : previous.fixed_profiles,
    pps_ranges: Array.isArray(raw.pps_ranges)
      ? raw.pps_ranges.flatMap((entry) => {
          const item = record(entry);
          return item ? [{
            min_voltage_mv: number(item, "min_voltage_mv", 0),
            max_voltage_mv: number(item, "max_voltage_mv", 0),
            step_mv: number(item, "step_mv", 100),
            max_current_ma: number(item, "max_current_ma", 0),
          }] : [];
        }).filter((range) => range.min_voltage_mv > 0 && range.max_voltage_mv >= range.min_voltage_mv)
      : previous.pps_ranges,
    avs_ranges: Array.isArray(raw.avs_ranges)
      ? raw.avs_ranges.flatMap((entry) => {
          const item = record(entry);
          return item ? [{
            min_voltage_mv: number(item, "min_voltage_mv", 0),
            max_voltage_mv: number(item, "max_voltage_mv", 0),
            step_mv: number(item, "step_mv", 100),
            max_power_w: number(item, "max_power_w", 0),
          }] : [];
        }).filter((range) => range.min_voltage_mv > 0 && range.max_voltage_mv >= range.min_voltage_mv)
      : previous.avs_ranges,
  };
  if (!status.available || !status.pd || status.last_error !== 0 || raw.capabilities_valid === false) {
    return { ...status, capabilities_valid: false, fixed_profiles: [], pps_ranges: [], avs_ranges: [] };
  }
  return status;
}

export { EMPTY_PD_STATUS };
