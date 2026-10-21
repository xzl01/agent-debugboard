import assert from "node:assert/strict";
import test from "node:test";
import type { CurrentAdcReading } from "./types.ts";
import { formatPowerMetric, measuredVoltage, readingMetric, powerRailRows, powerRailCatalog, powerRailLabel } from "./power.ts";
import { x12PowerFixture } from "./power.testFixtures.ts";

const reading: CurrentAdcReading = {
  name: "5v_out",
  signal: "INA226_5V",
  kind: "current",
  unit: "uA",
  value: 200_000,
  power_enabled: true,
  voltage_valid: true,
  voltage_uv: 4_875_000,
};

test("joins firmware output order with descriptor-only rails without inventing readings", () => {
  const { outputs, readings } = x12PowerFixture();
  const rows = powerRailRows(outputs, readings);
  assert.equal(rows.length, 5);
  assert.deepEqual(rows.map((row) => row.name), outputs.map((output) => output.name));
  assert.equal(rows.filter((row) => row.reading).length, 4);
  assert.equal(rows.at(-1)?.reading, undefined);
  const measuredOnly = { ...reading, name: "external_monitor" };
  assert.deepEqual(powerRailRows(outputs, [measuredOnly, ...readings]).map((row) => row.name), [...outputs.map((output) => output.name), "external_monitor"]);
  assert.deepEqual(powerRailRows([outputs[0]!, outputs[0]!], [reading, reading]).map((row) => row.name), [outputs[0]!.name]);
});

test("separates switchable outputs from instrumented capture/current channels", () => {
  const { outputs, readings } = x12PowerFixture();
  const catalog = powerRailCatalog(outputs, readings);
  assert.equal(catalog.switchable.length, 5);
  assert.equal(catalog.current.length, 4);
  assert.equal(catalog.measurements.length, 8);
  assert.ok(catalog.switchable.includes("5v_usb_hub"));
  assert.equal(catalog.current.includes("5v_usb_hub"), false);
  assert.equal(catalog.measurements.includes("5v_usb_hub"), false);
  assert.equal(powerRailLabel("5v_usb_hub"), "5V · usb hub");
  assert.equal(powerRailLabel("5V_OUT"), "5V");
  assert.equal(powerRailLabel("future_aux"), "future_aux");
});

test("voltage metrics use only valid measured bus voltage, never the nominal rail value", () => {
  assert.equal(measuredVoltage(reading), 4.875);
  assert.equal(readingMetric(reading, "voltage"), 4.875);
  for (const invalid of [
    { voltage_valid: false },
    { voltage_valid: undefined },
    { voltage_uv: undefined },
    { voltage_uv: NaN },
    { voltage_uv: Infinity },
  ]) {
    const sample = { ...reading, ...invalid };
    assert.equal(measuredVoltage(sample), null);
    assert.equal(readingMetric(sample, "voltage"), null);
    assert.equal(readingMetric(sample, "power"), null);
  }
});

test("rail enable state does not suppress actual current, measured voltage or calculated power", () => {
  const disabled = { ...reading, power_enabled: false };
  assert.equal(readingMetric(disabled, "voltage"), 4.875);
  assert.equal(readingMetric({ ...disabled, voltage_valid: false }, "voltage"), null);
  assert.equal(readingMetric({ ...disabled, voltage_uv: 0 }, "voltage"), 0);
  assert.equal(readingMetric(disabled, "current"), 0.2);
  assert.ok(Math.abs(readingMetric(disabled, "power")! - 0.975) < Number.EPSILON);
  assert.equal(readingMetric({ ...disabled, voltage_valid: false }, "power"), null);
  assert.equal(readingMetric({ ...disabled, voltage_uv: undefined }, "power"), null);
});

test("retains signed sensor current and power, including reverse current while the switch is off", () => {
  for (const power_enabled of [false, true]) {
    const sample = { ...reading, power_enabled, value: -200_000, voltage_uv: 4_800_000 };
    assert.equal(readingMetric(sample, "current"), -0.2);
    assert.equal(readingMetric(sample, "power"), -0.96);
    assert.equal(readingMetric({ ...sample, voltage_valid: false }, "power"), null);
  }
  assert.equal(formatPowerMetric(-0.2, "current"), "-0.200 A");
  assert.equal(formatPowerMetric(-0.001, "current"), "-1.0 mA");
  assert.equal(formatPowerMetric(-0.96, "power"), "-0.96 W");
  assert.equal(formatPowerMetric(-0.001, "power"), "-1.0 mW");
});

test("keeps current and power units while using measured voltage for available power samples", () => {
  assert.equal(readingMetric(reading, "current"), 0.2);
  assert.ok(Math.abs(readingMetric(reading, "power")! - 0.975) < Number.EPSILON);
  assert.equal(formatPowerMetric(4.875, "voltage"), "4.875 V");
  assert.equal(formatPowerMetric(0.2, "current"), "0.200 A");
  assert.equal(formatPowerMetric(0.96, "power"), "0.96 W");
});
