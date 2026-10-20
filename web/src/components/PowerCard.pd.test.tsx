import { act } from "react";
import { createRoot } from "react-dom/client";
import { afterEach, describe, expect, it, vi } from "vitest";
import { LanguageProvider } from "@/lib/i18n";
import { EMPTY_PD_STATUS } from "@/lib/pd";
import type { PdStatus } from "@/lib/types";
import { PowerCard } from "./PowerCard";

Object.assign(globalThis, { IS_REACT_ACT_ENVIRONMENT: true });
const closeViews: (() => void)[] = [];

function render(lang: "en" | "zh" = "en", pdOverrides: Partial<PdStatus> = {}) {
  localStorage.setItem("lang", lang);
  const onSetPd = vi.fn().mockResolvedValue(undefined);
  const host = document.createElement("div");
  document.body.append(host);
  const root = createRoot(host);
  const pd: PdStatus = {
    ...EMPTY_PD_STATUS,
    available: true, pd: true, mode: 1, voltage_mv: 5000, capabilities_valid: true,
    fixed_profiles: [{ voltage_mv: 5000, max_current_ma: 3000 }],
    pps_ranges: [{ min_voltage_mv: 3300, max_voltage_mv: 11000, step_mv: 100, max_current_ma: 3000 }],
    ...pdOverrides,
  };
  act(() => root.render(
    <LanguageProvider>
      <PowerCard
        pd={pd} onSetPd={onSetPd} onSet={vi.fn()}
        outputs={["5v_out", "12v_out", "20v_out", "vdd_5v"].map((name) => ({ name, controllable: true, state: "off", value: 0 }))}
        readings={[{ kind: "current", unit: "uA", name: "5v_out", signal: "INA226_5V", value: 300_000,
          power_enabled: false, voltage_valid: true, voltage_uv: 4_800_000 }]}
      />
    </LanguageProvider>,
  ));
  closeViews.push(() => act(() => { root.unmount(); host.remove(); }));
  return {
    host, onSetPd,
    input: host.querySelector<HTMLInputElement>('input[type="number"]')!,
    apply: [...host.querySelectorAll<HTMLButtonElement>("button")].find((button) => button.textContent === (lang === "en" ? "Apply" : "应用"))!,
  };
}

function typeValue(input: HTMLInputElement, value: string) {
  act(() => {
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value")!.set!.call(input, value);
    input.dispatchEvent(new Event("input", { bubbles: true }));
  });
}

describe("PowerCard PD editing and real translated measurement copy", () => {
  afterEach(() => {
    closeViews.splice(0).forEach((close) => close());
    localStorage.clear();
  });

  it("keeps each PPS draft keystroke and submits a valid millivolt value", async () => {
    const view = render();
    expect(view.input.getAttribute("aria-label")).toBe("Requested voltage (mV)");
    expect(view.input.parentElement?.textContent).toContain("mV");
    for (const value of ["", "9", "90", "900", "9000"]) {
      typeValue(view.input, value);
      expect(view.input.value).toBe(value);
      expect(view.apply.disabled).toBe(value !== "9000");
    }
    await act(async () => view.apply.click());
    expect(view.onSetPd).toHaveBeenCalledExactlyOnceWith(1, 9000);
  });

  it.each(["en", "zh"] as const)("provides translated expanded-chart controls and a rail-named dialog in %s", (lang) => {
    const view = render(lang);
    const previewVoltage = view.host.querySelector('[data-measurement-view="preview"] li[data-metric="voltage"]')!;
    expect(previewVoltage.textContent).toBe(lang === "en" ? "Voltage (V)4.800 V" : "电压 (V)4.800 V");
    expect(previewVoltage.querySelector(".whitespace-nowrap")).not.toBeNull();
    const enlarge = view.host.querySelector<HTMLButtonElement>('button[aria-haspopup="dialog"]')!;
    expect(enlarge.getAttribute("aria-label")).toBe(lang === "en" ? "5V · Expand chart" : "5V · 放大查看");
    act(() => { enlarge.focus(); enlarge.click(); });
    const modal = document.querySelector<HTMLElement>('[role="dialog"]')!;
    expect(modal.querySelector('li[data-metric="voltage"]')?.textContent).toContain(lang === "en" ? "Measured voltage (V)4.800 V" : "实测电压 (V)4.800 V");
    const colors = { current: "brand", voltage: "ok", power: "warn" };
    for (const metric of ["current", "voltage", "power"] as const) {
      const swatch = modal.querySelector<HTMLElement>(`li[data-metric="${metric}"] span[aria-hidden="true"]`)!;
      expect(swatch.style.color).toBe(`rgb(var(--c-${colors[metric]}))`);
      expect(swatch.style.borderTopColor).toBe(`rgb(var(--c-${colors[metric]}))`);
    }
    expect(modal.textContent).toContain(lang === "en" ? "5V · Power trend" : "5V · 电源趋势");
    expect(modal.textContent).toContain("600");
    expect(modal.textContent).toContain(lang === "en" ? "horizontal scrollbar" : "水平滚动条");
    expect(modal.querySelector('[aria-label="' + (lang === "en" ? "Zoom in on time axis" : "放大时间轴") + '"]')).not.toBeNull();
    const close = modal.querySelector<HTMLButtonElement>('[aria-label="' + (lang === "en" ? "Close expanded chart" : "关闭放大图表") + '"]')!;
    expect(close).not.toBeNull();
    act(() => close.click());
    expect(document.activeElement).toBe(enlarge);
  });

  it.each(["", "3200", "11100", "9001", "9000.5"])("rejects invalid PPS draft %j without replacing it", (value) => {
    const view = render();
    typeValue(view.input, value);
    act(() => view.input.dispatchEvent(new FocusEvent("focusout", { bubbles: true })));
    expect(view.input.value).toBe(value);
    expect(view.input.getAttribute("aria-invalid")).toBe("true");
    expect(view.apply.disabled).toBe(true);
    expect(view.host.querySelector('[role="alert"]')?.textContent).toBe(
      "Enter a whole number of mV within a source PPS range and at its supported step.",
    );
    view.apply.click();
    expect(view.onSetPd).not.toHaveBeenCalled();
    typeValue(view.input, "9000");
    expect(view.host.querySelector('[role="alert"]')).toBeNull();
    expect(view.apply.disabled).toBe(false);
  });

  it("validates against individual source ranges rather than the envelope between them", () => {
    const view = render("en", { pps_ranges: [
      { min_voltage_mv: 3300, max_voltage_mv: 5900, step_mv: 100, max_current_ma: 3000 },
      { min_voltage_mv: 9000, max_voltage_mv: 11000, step_mv: 200, max_current_ma: 2000 },
    ] });
    for (const value of ["6000", "9100"]) {
      typeValue(view.input, value);
      expect(view.apply.disabled).toBe(true);
    }
    typeValue(view.input, "9200");
    expect(view.apply.disabled).toBe(false);
  });

  it.each([
    ["en", "rail switch off — showing actual measurements", "5V power", "Each line uses its own scale"],
    ["zh", "电源开关已关闭 — 仍显示实际测量值", "5V 电源", "各曲线使用独立量程"],
  ] as const)("shows honest %s copy and names the rail switch while actual load remains nonzero", (lang, copy, label, scales) => {
    const view = render(lang);
    expect(view.host.textContent).toContain(copy);
    expect(view.host.textContent).toContain("0.300 A");
    expect(view.host.textContent).toContain("4.800 V");
    expect(view.host.textContent).toContain("1.44 W");
    expect(view.host.textContent).toContain(scales);
    expect(view.host.querySelector('[role="switch"]')?.getAttribute("aria-label")).toBe(label);
    expect([...view.host.querySelectorAll('[role="switch"]')].map((item) => item.getAttribute("aria-label"))).toEqual(
      ["5V", "12V", "20V", "vdd_5v"].map((rail) => `${rail} ${lang === "en" ? "power" : "电源"}`),
    );
    expect(view.host.textContent).not.toMatch(/showing zero|显示为 0/);
    typeValue(view.input, "9001");
    act(() => view.input.dispatchEvent(new FocusEvent("focusout", { bubbles: true })));
    expect(view.host.querySelector('[role="alert"]')?.textContent).toBe(lang === "en"
      ? "Enter a whole number of mV within a source PPS range and at its supported step."
      : "请输入电源 PPS 范围内且符合步进的整数毫伏值。");
  });
});
