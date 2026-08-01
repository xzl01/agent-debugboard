import { describe, expect, it } from "vitest";
import { persistentConfigCurrentStateKey } from "./persistentConfigCurrentStateKey";

type PowerFixture = {
  readonly name: string;
  readonly state: string;
  readonly signal: string;
  readonly controllable: boolean;
  readonly value: number;
};
type SwitchFixture = {
  readonly route: string;
  readonly routes: readonly string[];
  readonly requires_confirm: boolean;
};
type GpioFixture = {
  readonly name: string;
  readonly direction: string;
  readonly value: number;
  readonly pin: number;
  readonly note: string;
};
type LiveState = {
  readonly powerOutputs: readonly PowerFixture[];
  readonly switches: Readonly<Record<string, SwitchFixture>>;
  readonly gpios: readonly GpioFixture[];
};
type RelevantFieldCase = {
  readonly field: string;
  readonly change: (baseline: LiveState) => LiveState;
};

function liveState(): LiveState {
  return {
    powerOutputs: [
      { name: "power-b", state: "off", signal: "B", controllable: true, value: 0 },
      { name: "power-a", state: "off", signal: "A", controllable: false, value: 1 },
    ],
    switches: {
      "switch-b": { route: "target", routes: ["target"], requires_confirm: true },
      "switch-a": { route: "target", routes: ["reader"], requires_confirm: false },
    },
    gpios: [
      { name: "GPIO-B", direction: "input", value: 0, pin: 8, note: "B" },
      { name: "GPIO-A", direction: "input", value: 0, pin: 7, note: "A" },
    ],
  };
}

const relevantFieldCases: readonly RelevantFieldCase[] = [
  {
    field: "power name",
    change: (baseline) => ({
      ...baseline,
      powerOutputs: baseline.powerOutputs.map((power) =>
        power.name === "power-b" ? { ...power, name: "power-c" } : power
      ),
    }),
  },
  {
    field: "power state",
    change: (baseline) => ({
      ...baseline,
      powerOutputs: baseline.powerOutputs.map((power) =>
        power.name === "power-b" ? { ...power, state: "on" } : power
      ),
    }),
  },
  {
    field: "switch name",
    change: (baseline) => ({
      ...baseline,
      switches: Object.fromEntries(Object.entries(baseline.switches).map(([name, value]) => [
        name === "switch-b" ? "switch-c" : name,
        value,
      ])),
    }),
  },
  {
    field: "switch route",
    change: (baseline) => ({
      ...baseline,
      switches: Object.fromEntries(Object.entries(baseline.switches).map(([name, value]) => [
        name,
        name === "switch-b" ? { ...value, route: "reader" } : value,
      ])),
    }),
  },
  {
    field: "GPIO name",
    change: (baseline) => ({
      ...baseline,
      gpios: baseline.gpios.map((gpio) =>
        gpio.name === "GPIO-B" ? { ...gpio, name: "GPIO-C" } : gpio
      ),
    }),
  },
  {
    field: "GPIO direction",
    change: (baseline) => ({
      ...baseline,
      gpios: baseline.gpios.map((gpio) =>
        gpio.name === "GPIO-B" ? { ...gpio, direction: "output" } : gpio
      ),
    }),
  },
  {
    field: "GPIO value",
    change: (baseline) => ({
      ...baseline,
      gpios: baseline.gpios.map((gpio) =>
        gpio.name === "GPIO-B" ? { ...gpio, value: 1 } : gpio
      ),
    }),
  },
];

describe("persistentConfigCurrentStateKey", () => {
  it("is independent of firmware collection and object iteration order", () => {
    const original = liveState();
    const reordered = {
      powerOutputs: [...original.powerOutputs].reverse(),
      switches: {
        "switch-a": original.switches["switch-a"],
        "switch-b": original.switches["switch-b"],
      },
      gpios: [...original.gpios].reverse(),
    };

    expect(persistentConfigCurrentStateKey(reordered)).toBe(
      persistentConfigCurrentStateKey(original)
    );
  });

  it.each(relevantFieldCases)("changes when only the relevant $field changes", ({ change }) => {
    const baseline = liveState();
    const changed = change(baseline);

    expect(changed.powerOutputs).toHaveLength(baseline.powerOutputs.length);
    expect(Object.keys(changed.switches)).toHaveLength(Object.keys(baseline.switches).length);
    expect(changed.gpios).toHaveLength(baseline.gpios.length);
    expect(persistentConfigCurrentStateKey(changed)).not.toBe(
      persistentConfigCurrentStateKey(baseline)
    );
  });

  it("ignores telemetry and firmware metadata outside the current-value contract", () => {
    const baseline = liveState();
    const metadataChanged = {
      powerOutputs: baseline.powerOutputs.map((power) => ({
        ...power,
        signal: `${power.signal}-changed`,
        controllable: !power.controllable,
        value: power.value + 10,
      })),
      switches: {
        "switch-a": { ...baseline.switches["switch-a"], routes: ["other"], requires_confirm: true },
        "switch-b": { ...baseline.switches["switch-b"], routes: ["other"], requires_confirm: false },
      },
      gpios: baseline.gpios.map((gpio) => ({
        ...gpio,
        pin: gpio.pin + 100,
        note: `${gpio.note}-changed`,
        layoutGroup: "other",
      })),
      adc: [{ name: "power-a", current_ua: 999 }],
      monitoring: { runtime: { uptime_seconds: 999 } },
      config: { savedCount: 999 },
    };

    expect(persistentConfigCurrentStateKey(metadataChanged)).toBe(
      persistentConfigCurrentStateKey(baseline)
    );
  });

  it("has stable keys for empty and unavailable live-state inputs", () => {
    const empty = { powerOutputs: [], switches: {}, gpios: [] };

    expect(persistentConfigCurrentStateKey(empty)).toBe(
      persistentConfigCurrentStateKey({ powerOutputs: [], switches: {}, gpios: [] })
    );
    expect(persistentConfigCurrentStateKey(null)).toBe(
      persistentConfigCurrentStateKey(undefined)
    );
  });
});
