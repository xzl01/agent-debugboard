import type { AdcReading, CurrentAdcReading, PowerOutput } from "./types.ts";

/** Test-only firmware catalog: five outputs, four INA226 readings, four ADC pads. */
export function x12PowerFixture(): { outputs: PowerOutput[]; readings: AdcReading[] } {
  const instrumented = ["5v_out", "12v_out", "20v_out", "vdd_5v"];
  const outputs: PowerOutput[] = [...instrumented, "5v_usb_hub"].map((name) => ({
    name, signal: `TEST_${name.toUpperCase()}`, controllable: true, state: "on", value: 1,
  }));
  return {
    outputs,
    readings: [
      ...instrumented.map((name): CurrentAdcReading => ({ name, signal: `INA_TEST_${name.toUpperCase()}`, kind: "current", unit: "uA",
        value: 200_000, power_enabled: true, voltage_valid: true, voltage_uv: 5_100_000 })),
      ...[0, 1, 2, 3].map((index): AdcReading => ({ name: `adc${index}`, signal: `TEST_ADC${index}`, kind: "voltage", unit: "uV", value: 1_500_000 })),
    ],
  };
}
