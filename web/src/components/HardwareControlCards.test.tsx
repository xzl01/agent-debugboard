import { act } from "react";
import { createRoot, type Root } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import type { AdcReading, PowerOutput, SafeGpio, SwitchState } from "@/lib/types";
import { GpioCard } from "./GpioCard";
import { PowerCard } from "./PowerCard";
import { SwitchCard } from "./SwitchCard";

vi.mock("@/lib/i18n", () => ({
  useI18n: () => ({ t: (key: string) => key }),
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

function changeSelect(select: HTMLSelectElement, value: string) {
  act(() => {
    const setter = Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, "value")?.set;
    setter?.call(select, value);
    select.dispatchEvent(new Event("change", { bubbles: true }));
  });
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
      usb: { route: "target", routes: ["pc", "target"], requires_confirm: true },
    };
    const onSet = vi.fn();
    const confirm = vi.spyOn(window, "confirm").mockReturnValue(false);
    const view = render(<SwitchCard switches={switches} onSet={onSet} />);

    act(() => button(view.host, "pc").click());
    expect(confirm).toHaveBeenCalledWith("switch.confirm");
    expect(onSet).not.toHaveBeenCalled();

    confirm.mockReturnValue(true);
    act(() => button(view.host, "pc").click());
    expect(onSet).toHaveBeenCalledWith("usb", "pc");

    view.rerender(<SwitchCard switches={switches} onSet={onSet} disabled />);
    expect([...view.host.querySelectorAll<HTMLButtonElement>("button")].every((item) => item.disabled)).toBe(true);
    view.close();
  });

  it("keeps GPIO current state factual until the edited direction is applied", () => {
    const gpio: SafeGpio = {
      name: "GP12",
      pin: 12,
      note: "safe input",
      direction: "input",
      value: 1,
    };
    const onSet = vi.fn();
    const view = render(<GpioCard gpios={[gpio]} onSet={onSet} />);

    expect(view.host.textContent).toContain("gpio.current: gpio.input · gpio.high");
    const direction = view.host.querySelector<HTMLSelectElement>("select");
    if (!direction) throw new TypeError("Direction select not found");
    changeSelect(direction, "output");

    expect(view.host.textContent).toContain("gpio.current: gpio.input · gpio.high");
    const value = [...view.host.querySelectorAll<HTMLSelectElement>("select")][1];
    if (!value) throw new TypeError("Value select not found");
    changeSelect(value, "0");
    act(() => button(view.host, "gpio.set").click());
    expect(onSet).toHaveBeenCalledWith("GP12", "output", 0);
    expect(view.host.textContent).toContain("gpio.current: gpio.input · gpio.high");
    view.close();
  });

  it("shows input GPIO as read-only in quick controls and locks output toggles offline", () => {
    const gpios: SafeGpio[] = [
      { name: "GP10", pin: 10, note: "output", direction: "output", value: 1 },
      { name: "GP12", pin: 12, note: "input", direction: "input", value: 0 },
    ];
    const onSet = vi.fn();
    const view = render(<GpioCard gpios={gpios} onSet={onSet} compact disabled stale />);

    expect(view.host.textContent).toContain("snapshot.readOnly");
    expect(view.host.textContent).toContain("snapshot.last: gpio.low");
    const toggles = view.host.querySelectorAll<HTMLButtonElement>('[role="switch"]');
    expect(toggles).toHaveLength(1);
    expect(toggles[0]?.disabled).toBe(true);
    expect(view.host.querySelectorAll("select")).toHaveLength(0);
    expect(onSet).not.toHaveBeenCalled();
    view.close();
  });

  it("filters the full GPIO list by pin number, name, or interface note", () => {
    const gpios: SafeGpio[] = [
      { name: "CON_USER", pin: 9, note: "user button", direction: "input", value: 0 },
      { name: "GP12", pin: 12, note: "camera reset", direction: "output", value: 1 },
    ];
    const view = render(<GpioCard gpios={gpios} onSet={vi.fn()} />);
    const filter = view.host.querySelector<HTMLInputElement>('[data-testid="gpio-filter"]');
    if (!filter) throw new TypeError("GPIO filter not found");

    act(() => {
      const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")?.set;
      setter?.call(filter, "camera");
      filter.dispatchEvent(new Event("input", { bubbles: true }));
      filter.dispatchEvent(new Event("change", { bubbles: true }));
    });

    expect(view.host.textContent).toContain("GP12");
    expect(view.host.textContent).not.toContain("CON_USER");
    view.close();
  });
});
