import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import { LanguageProvider } from "@/lib/i18n";
import { x12PowerFixture } from "@/lib/power.testFixtures";
import type { AdcReading, PowerOutput } from "@/lib/types";
import { PowerCard } from "./PowerCard";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
vi.mock("./PowerSparkline", () => ({ MeasurementSparkline: ({ reading }: { reading: AdcReading }) => <div data-chart-reading={reading.name} /> }));
const views: (() => void)[] = [];
afterEach(() => { views.splice(0).forEach((close) => close()); vi.useRealTimers(); localStorage.clear(); });

function render(outputs: PowerOutput[], readings: readonly AdcReading[], lang = "en") {
  localStorage.setItem("lang", lang);
  const host = document.createElement("div"); document.body.append(host); const root = createRoot(host);
  const onSet = vi.fn();
  const update = (next: PowerOutput[], nextReadings = readings) => act(() => root.render(<LanguageProvider><PowerCard outputs={next} readings={nextReadings} onSet={onSet} /></LanguageProvider>));
  update(outputs); views.push(() => { act(() => root.unmount()); host.remove(); });
  return { host, onSet, update };
}

describe("firmware-driven power rows", () => {
  it.each(["en", "zh"])("shows the fifth uninstrumented output without inventing measurements in %s", (lang) => {
    const { outputs, readings } = x12PowerFixture();
    const view = render(outputs, readings, lang);
    const rows = [...view.host.querySelectorAll("[data-power-rail]")];
    expect(rows.map((row) => row.getAttribute("data-power-rail"))).toEqual(outputs.map((output) => output.name));
    expect(rows).toHaveLength(5);
    const hub = view.host.querySelector('[data-power-rail="5v_usb_hub"]')!;
    expect(hub.textContent).toContain(lang === "en" ? "Measurement unavailable" : "测量不可用");
    expect(hub.textContent).not.toMatch(/0(?:\.0+)? (?:m?A|m?V|m?W)/u);
    expect(hub.querySelector("[data-power-measurements], [data-chart-reading]")).toBeNull();
    expect(hub.querySelector('[role="switch"]')?.getAttribute("aria-checked")).toBe("true");
    expect(view.host.querySelectorAll("[data-power-measurements]")).toHaveLength(4);
    expect(view.host.querySelectorAll("[data-chart-reading]")).toHaveLength(8); // Four current + four ADC pads.
    expect(view.host.querySelector('[data-testid="pd-live-status"]')?.textContent).toBe(lang === "en" ? "Voltage not reported yet" : "电压尚未读取");
    expect(view.onSet).not.toHaveBeenCalled();
    act(() => (hub.querySelector('[role="switch"]') as HTMLButtonElement).click());
    expect(view.onSet).toHaveBeenCalledExactlyOnceWith("5v_usb_hub", false);
    expect(view.onSet.mock.calls.some(([name]) => name === "vdd_5v")).toBe(false);
  });

  it("keeps arbitrary output order and appends measurement-only rails in descriptor order", () => {
    const { readings } = x12PowerFixture();
    const view = render([
      { name: "future_aux", signal: "FUTURE", controllable: true, state: "off", value: 0 },
      { name: "20v_out", controllable: false, state: "locked", value: null },
    ], readings);
    expect([...view.host.querySelectorAll("[data-power-rail]")].map((row) => row.getAttribute("data-power-rail"))).toEqual([
      "future_aux", "20v_out", "5v_out", "12v_out", "vdd_5v",
    ]);
    expect(view.host.querySelectorAll('[role="switch"]')).toHaveLength(2);
    expect(view.host.querySelector('[data-power-rail="future_aux"] [data-measurement-unavailable]')).not.toBeNull();
    expect(view.host.querySelector('[data-power-rail="5v_out"] [role="switch"]')).toBeNull();
  });

  it("runs and resets dynamic output timers independently of ADC coverage", () => {
    vi.useFakeTimers(); vi.setSystemTime(10000);
    const output = { name: "future_aux", controllable: true, state: "on", value: 1 };
    const view = render([output], []);
    const row = () => view.host.querySelector('[data-power-rail="future_aux"]')!;
    expect(row().textContent).toContain("≥ 0s");
    act(() => vi.advanceTimersByTime(12000));
    expect(row().textContent).toContain("≥ 12s");
    view.update([{ ...output, state: "off", value: 0 }]);
    expect(row().textContent).not.toContain("On for");
    view.update([output]);
    expect(row().textContent).toContain("0s");
    expect(row().textContent).not.toContain("≥");
    view.update([]);
    act(() => vi.advanceTimersByTime(15000));
    view.update([output]);
    expect(row().textContent).toContain("≥ 0s");
    expect(row().textContent).not.toContain("15s");
  });
});
