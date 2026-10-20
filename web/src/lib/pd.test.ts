import assert from "node:assert/strict";
import test from "node:test";
import { EMPTY_PD_STATUS, parsePdStatus } from "./pd.ts";
import type { PdStatus } from "./types.ts";

test("parsePdStatus normalizes firmware PD state", () => {
  assert.deepEqual(parsePdStatus({
    available: true,
    mode: 1,
    voltage_mv: 9000,
    gear: 0,
    pps_mv: 9000,
    pd: true,
    max_current_ma: 2500,
    last_error: 0,
  }), {
    available: true,
    mode: 1,
    voltage_mv: 9000,
    gear: 0,
    pps_mv: 9000,
    pd: true,
    qc2: false,
    qc3: false,
    bc: false,
    epr: false,
    avs: false,
    max_current_ma: 2500,
    last_error: 0,
    protocol: "none",
    board_max_voltage_mv: 20000,
    capabilities_valid: false,
    fixed_profiles: [],
    pps_ranges: [],
    avs_ranges: [],
  });
});

const cached: PdStatus = {
  ...EMPTY_PD_STATUS, available: true, pd: true, capabilities_valid: true,
  fixed_profiles: [{ voltage_mv: 9000, max_current_ma: 3000 }],
  pps_ranges: [{ min_voltage_mv: 3300, max_voltage_mv: 11000, step_mv: 100, max_current_ma: 3000 }],
  avs_ranges: [{ min_voltage_mv: 15000, max_voltage_mv: 20000, step_mv: 100, max_power_w: 65 }],
};

test("explicit invalid capability state clears all cached tables, even if stale tables accompany it", () => {
  for (const update of [{ capabilities_valid: false }, { ...cached, capabilities_valid: false }]) {
    assert.deepEqual(parsePdStatus(update, cached), {
      ...cached, capabilities_valid: false, fixed_profiles: [], pps_ranges: [], avs_ranges: [],
    });
  }
});

test("missing PD or capability fields preserve cache without converting explicit false into absence", () => {
  assert.equal(parsePdStatus(undefined, cached), cached);
  assert.deepEqual(parsePdStatus({ voltage_mv: 5000 }, cached), { ...cached, voltage_mv: 5000 });
  assert.deepEqual(parsePdStatus({ capabilities_valid: false, voltage_mv: 5000 }, cached), {
    ...cached, voltage_mv: 5000, capabilities_valid: false, fixed_profiles: [], pps_ranges: [], avs_ranges: [],
  });
});
