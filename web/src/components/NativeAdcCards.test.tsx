import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import { LanguageProvider } from "@/lib/i18n";
import type { AdcReading, VoltageAdcReading } from "@/lib/types";
import { NativeAdcCards } from "./NativeAdcCards";
import { PowerCard } from "./PowerCard";
import { SwitchCard } from "./SwitchCard";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
const clean: (() => void)[] = [];
const native = (name: string, value = 1_650_000): VoltageAdcReading => ({ kind: "voltage", unit: "uV", role: "native", name, signal: `SIGNAL_${name}`, value });
function mount(readings: readonly AdcReading[], updatedAt?: number | null) {
  localStorage.setItem("lang", "en"); const host = document.createElement("div"); document.body.append(host); const root = createRoot(host);
  act(() => root.render(<LanguageProvider><div className="grid min-w-0 grid-cols-1"><PowerCard outputs={[]} readings={readings} onSet={vi.fn()} pdInputUpdatedAt={updatedAt} /><div className="grid min-w-0 gap-4"><NativeAdcCards readings={readings} /><div data-testid="routes"><SwitchCard switches={{}} onSet={vi.fn()} /></div></div></div></LanguageProvider>));
  clean.push(() => { act(() => root.unmount()); host.remove(); }); return host;
}
afterEach(() => { clean.splice(0).forEach((close) => close()); localStorage.clear(); vi.restoreAllMocks(); });
describe("PD source and independent native ADC composition", () => {
  it("places ADC1/2/3 in stable DOM/mobile order above routing without duplicating ADC0", () => {
    const host = mount([native("adc3"), { ...native("adc0", 20_000_000), role: "pd_input", mv: 2609 }, native("adc1"), native("adc2")]);
    const cards = [...host.querySelectorAll<HTMLElement>("[data-native-adc]")];
    expect(cards.map((card) => card.dataset.nativeAdc)).toEqual(["adc1", "adc2", "adc3"]);
    const routes = host.querySelector('[data-testid="routes"]')!;
    for (const card of cards) { expect(card.compareDocumentPosition(routes) & Node.DOCUMENT_POSITION_FOLLOWING).not.toBe(0); expect(card.querySelector('[role="switch"]')).toBeNull(); expect(card.textContent).toContain("Fixed 0–3.3 V"); }
    expect(host.querySelector('[data-native-adc="adc0"]')).toBeNull();
    expect(host.querySelector('[data-testid="pd-input-measurement"]')?.textContent).toContain("20.000 V");
    const sourceChart = host.querySelector('svg[aria-label^="adc0"]')!;
    expect(sourceChart.getAttribute("aria-label")).toContain("source-side");
    expect(sourceChart.parentElement?.textContent).not.toContain("0–3.3");
    expect(host.querySelector('[data-testid="native-adc-cards"]')?.classList.contains("grid-cols-1")).toBe(true);
    expect(cards.every((card) => card.classList.contains("min-w-0"))).toBe(true);
  });
  it.each([undefined, "future-role"])("does not render missing/unknown role %s in either PD or native cards", (role) => {
    const readings = ["adc0", "adc1", "adc2", "adc3", "future"].map((name) => ({ ...native(name, 2_608_696), role, note: "PD input divider" } as unknown as VoltageAdcReading));
    const host = mount(readings);
    expect(host.querySelector('[data-testid="pd-input-measurement"]')?.textContent).toContain("—");
    expect(host.querySelector('[data-testid="pd-input-measurement"]')?.textContent).not.toContain("2.609");
    expect(host.querySelector('[data-testid="native-adc-cards"]')).toBeNull();
    expect(host.querySelector('svg[role="img"]')).toBeNull();
  });
  it("uses firmware roles rather than channel names to choose the destination card", () => {
    const host = mount([native("adc0"), { ...native("supply_sensor", 20_000_000), role: "pd_input" }]);
    expect([...host.querySelectorAll("[data-native-adc]")].map((card) => card.getAttribute("data-native-adc"))).toEqual(["adc0"]);
    const pd = host.querySelector('[data-testid="pd-input-measurement"]')!;
    expect(pd.textContent).toContain("supply_sensor");
    expect(pd.textContent).toContain("20.000 V");
    expect(pd.textContent).not.toContain("SIGNAL_adc0");
  });
  it("keeps a valid zero distinct from missing or stale source measurement", () => {
    const source = { ...native("adc0", 0), role: "pd_input" as const };
    const fresh = mount([source], Date.now()); expect(fresh.querySelector('[data-testid="pd-input-measurement"]')?.textContent).toContain("0.000 V");
    const stale = mount([source], Date.now() - 31_000); expect(stale.querySelector('[data-testid="pd-input-measurement"]')?.textContent).not.toContain("0.000 V");
    expect(stale.querySelector('[data-testid="pd-input-measurement"]')?.textContent).toContain("Read-only");
  });
});
