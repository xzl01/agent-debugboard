import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import type { AdcReading, PowerOutput, SafeGpio, SwitchState } from "@/lib/types";
import { GpioCard } from "./GpioCard";
import { layoutForTestGpios } from "@/lib/gpioPhysicalLayout.testFixtures";
import { x12SwitchesFixture } from "@/lib/switches.testFixtures";
import { setSwitch } from "@/lib/api";
import { PowerCard } from "./PowerCard";
import { SwitchCard } from "./SwitchCard";

const I18N_TEMPLATES: Record<string, string> = {
  "gpio.pinAria": "{name} GP{pin}, {direction}, {level}",
};

vi.mock("@/lib/i18n", () => ({
  useI18n: () => ({
    t: (key: string, params?: Record<string, string | number>) => {
      const template = I18N_TEMPLATES[key] ?? key;
      if (!params) return template;
      return template.replace(/\{(\w+)\}/g, (match, name: string) =>
        name in params ? String(params[name]) : match,
      );
    },
  }),
}));

vi.mock("./PowerSparkline", () => ({
  MeasurementSparkline: () => <div data-testid="measurement-sparkline" />,
}));

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });

function render(node: React.ReactNode) {
  const host = document.createElement("div");
  document.body.append(host);
  const root: Root = createRoot(host);
  act(() => root.render(node));
  return {
    host,
    rerender(next: React.ReactNode) {
      act(() => root.render(next));
    },
    close() {
      act(() => root.unmount());
      host.remove();
    },
  };
}

function button(host: HTMLElement, text: string): HTMLButtonElement {
  const match = [...host.querySelectorAll<HTMLButtonElement>("button")]
    .find((candidate) => candidate.textContent?.includes(text));
  if (!match) throw new TypeError(`Button not found: ${text}`);
  return match;
}

function press(element: Element, key: string) {
  act(() => {
    element.dispatchEvent(new KeyboardEvent("keydown", { key, bubbles: true, cancelable: true }));
  });
}

async function flush() {
  await act(async () => {
    await Promise.resolve();
  });
}

function pinButton(host: HTMLElement, pin: number): SVGGElement {
  const element = host.querySelector<SVGGElement>(`[role="button"][aria-label*="GP${pin},"]`);
  if (!element) throw new TypeError(`GPIO pin GP${pin} not found`);
  return element;
}

const outputs: PowerOutput[] = [
  {
    name: "5v_out",
    signal: "GP05_5V_EN",
    gp: 5,
    controllable: true,
    state: "on",
    value: 1,
  },
];

const readings: AdcReading[] = [
  {
    name: "5v_out",
    signal: "ADC0",
    kind: "current",
    unit: "uA",
    value: 184_000,
    power_enabled: true,
    voltage_valid: true,
    voltage_uv: 5_000_000,
  },
  {
    name: "adc3",
    signal: "GP29",
    kind: "voltage",
    unit: "uV",
    value: 373_000,
  },
];

describe("hardware control cards", () => {
  afterEach(() => {
    document.body.replaceChildren();
    localStorage.clear();
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
  });

  it("keeps quick power control compact while exposing live values and the full analysis route", () => {
    localStorage.setItem("linkr-power-trends-expanded", "true");
    const onSet = vi.fn();
    const onOpenDetails = vi.fn();
    const view = render(
      <PowerCard
        outputs={outputs}
        readings={readings}
        onSet={onSet}
        onOpenDetails={onOpenDetails}
        disabled
        compact
      />,
    );

    expect(view.host.textContent).toContain("0.184 A");
    expect(view.host.textContent).toContain("0.92 W");
    expect(view.host.textContent).toContain("0.373 V");
    expect(view.host.querySelector('[role="tablist"]')).toBeNull();
    expect(view.host.querySelector('[data-testid="measurement-sparkline"]')).toBeNull();
    const railToggle = view.host.querySelector<HTMLButtonElement>('[role="switch"]');
    expect(railToggle?.disabled).toBe(true);
    act(() => button(view.host, "quick.power.details").click());
    expect(onOpenDetails).toHaveBeenCalledOnce();
    expect(onSet).not.toHaveBeenCalled();
    view.close();
  });

  it("requires confirmation for risky routes and keeps all route writes locked when disabled", () => {
    const switches: SwitchState = {
      aux: { route: "lane-a", routes: ["lane-a", "lane-b"], requires_confirm: true },
    };
    const onSet = vi.fn();
    const confirm = vi.spyOn(window, "confirm").mockReturnValue(false);
    const view = render(<SwitchCard switches={switches} onSet={onSet} />);

    act(() => button(view.host, "lane-b").click());
    expect(confirm).toHaveBeenCalledWith("switch.confirm");
    expect(onSet).not.toHaveBeenCalled();

    confirm.mockReturnValue(true);
    act(() => button(view.host, "lane-b").click());
    expect(onSet).toHaveBeenCalledWith("aux", "lane-b");

    view.rerender(<SwitchCard switches={switches} onSet={onSet} disabled />);
    expect([...view.host.querySelectorAll<HTMLButtonElement>("button")].every((item) => item.disabled)).toBe(true);
    view.close();
  });

  it("renders only the x1.2 advertised sd/tf_wp switches and sends no USB switch request", async () => {
    const fetchMock = vi.fn<typeof fetch>(async () => new Response("{}"));
    vi.stubGlobal("fetch", fetchMock);
    const switches = x12SwitchesFixture();
    const view = render(<SwitchCard switches={switches} onSet={(name, route) => { void setSwitch(name, route); }} />);
    expect(view.host.textContent).not.toContain("USB data");
    const actions = [...view.host.querySelectorAll<HTMLButtonElement>("button")];
    expect(actions).toHaveLength(4);
    for (const action of actions) { act(() => action.click()); await flush(); }
    expect(fetchMock.mock.calls).toHaveLength(4);
    expect(fetchMock.mock.calls.map(([path]) => path)).toEqual([
      "/api/v1/switch/sd", "/api/v1/switch/sd", "/api/v1/switch/tf_wp", "/api/v1/switch/tf_wp",
    ]);
    expect(fetchMock.mock.calls.some(([path]) => String(path).endsWith("/switch/usb"))).toBe(false);
    view.close();
  });

  it("keeps the pin accessible direction and level firmware-factual across a keyboard action", async () => {
    // Given: a layout-valid input GPIO reported HIGH by the firmware snapshot
    const gpio: SafeGpio = {
      name: "GP12",
      pin: 12,
      note: "safe input",
      direction: "input",
      value: 1,
      layoutGroup: "J16",
      layoutLabel: "GP12",
      layoutRow: 2,
      layoutColumn: 0,
    };
    const onSet = vi.fn().mockResolvedValue(undefined);
    const view = render(<GpioCard gpios={[gpio]} physicalLayout={layoutForTestGpios([gpio])} onSet={onSet} />);
    expect(pinButton(view.host, 12).getAttribute("aria-label")).toContain("gpio.input");
    expect(pinButton(view.host, 12).getAttribute("aria-label")).toContain("gpio.high");

    // When: a keyboard gesture drives the pin output HIGH
    press(pinButton(view.host, 12), "1");
    await flush();

    // Then: onSet carries the requested write, but the accessible state still
    // reflects the last firmware snapshot instead of optimistic local state
    expect(onSet).toHaveBeenCalledWith("GP12", "output", 1);
    expect(pinButton(view.host, 12).getAttribute("aria-label")).toContain("gpio.input");
    expect(pinButton(view.host, 12).getAttribute("aria-label")).toContain("gpio.high");

    // When: the firmware reports the applied state in a new snapshot
    view.rerender(<GpioCard gpios={[{ ...gpio, direction: "output" }]} physicalLayout={layoutForTestGpios([gpio])} onSet={onSet} />);

    // Then: the accessible direction tracks the new snapshot
    expect(pinButton(view.host, 12).getAttribute("aria-label")).toContain("gpio.output");
    view.close();
  });

  it("keeps the full physical pinout read-only offline", () => {
    const gpios: SafeGpio[] = [
      { name: "GP10", pin: 10, note: "output", direction: "output", value: 1 },
      { name: "GP12", pin: 12, note: "input", direction: "input", value: 0 },
    ];
    const onSet = vi.fn();
    const view = render(<GpioCard gpios={gpios} physicalLayout={layoutForTestGpios(gpios)} onSet={onSet} disabled stale />);

    expect(view.host.textContent).toContain("snapshot.readOnly");
    const pins = view.host.querySelectorAll('[role="button"][aria-disabled="true"]');
    expect(pins).toHaveLength(2);
    press(pinButton(view.host, 10), "1");
    press(pinButton(view.host, 12), "i");
    expect(view.host.querySelector('[role="switch"]')).toBeNull();
    expect(view.host.querySelectorAll("select")).toHaveLength(0);
    expect(onSet).not.toHaveBeenCalled();
    view.close();
  });

  it("keeps every layout-valid pin in the fixed pinout and exposes no filter surface", () => {
    // Given: a full connector pinout with several layout-valid pins
    const gpios: SafeGpio[] = [
      { name: "GP10", pin: 10, note: "uart tx", direction: "output", value: 1, layoutGroup: "J16", layoutLabel: "GP10", layoutRow: 0, layoutColumn: 0 },
      { name: "GP11", pin: 11, note: "uart rx", direction: "input", value: 0, layoutGroup: "J16", layoutLabel: "GP11", layoutRow: 1, layoutColumn: 0 },
      { name: "GP12", pin: 12, note: "camera reset", direction: "input", value: 1, layoutGroup: "J16", layoutLabel: "GP12", layoutRow: 2, layoutColumn: 1 },
    ];
    const view = render(<GpioCard gpios={gpios} physicalLayout={layoutForTestGpios(gpios)} onSet={vi.fn()} />);

    // Then: every supplied pin is present and the card has no filter input that
    // could alter membership
    const pins = () => view.host.querySelectorAll('[role="button"][aria-label*="GP"]');
    expect(pins()).toHaveLength(3);
    expect(view.host.querySelector("input")).toBeNull();

    // When: a state notification rerenders the card with new levels and directions
    view.rerender(
      <GpioCard
        gpios={gpios.map((g) => ({ ...g, direction: g.direction === "input" ? "output" : "input", value: g.value > 0 ? 0 : 1 }))}
        physicalLayout={layoutForTestGpios(gpios)}
        onSet={vi.fn()}
      />,
    );

    // Then: pin membership is unchanged - notifications never add, remove, or
    // reorder pins in the fixed pinout
    expect(pins()).toHaveLength(3);
    for (const pin of [10, 11, 12]) {
      expect(pinButton(view.host, pin)).toBeDefined();
    }
    view.close();
  });
});
